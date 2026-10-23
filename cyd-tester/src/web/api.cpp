#include "api.h"
#include "version.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <LittleFS.h>
#include <WiFi.h>
#include "../app/runtime.h"
#include "../modbus/server.h"
#include "../net/wifi_manager.h"
#include "../profiles/unit_config.h"
#include "web_server.h"


namespace api {
namespace {
constexpr size_t MaxConfigBytes = 8192;
constexpr size_t MaxProfileBytes = 48 * 1024;
uint32_t rebootAt = 0;
AsyncWebSocket ws("/ws");

// ---------- helpers ----------
String num(double v) {
  if (std::isnan(v) || std::isinf(v)) return "null";
  char b[24];
  snprintf(b, sizeof b, "%.7g", v);
  return b;
}
String str(const String& s) { return "\"" + web::jsonEscape(s) + "\""; }
String str(const std::string& s) { return str(String(s.c_str())); }
String boolean(bool b) { return b ? "true" : "false"; }

void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
void sendError(AsyncWebServerRequest* r, int code, const String& msg) {
  sendJson(r, code, "{\"ok\":false,\"error\":" + str(msg) + "}");
}

// A parameter from the query string or a urlencoded body.
String param(AsyncWebServerRequest* r, const char* name) {
  if (const AsyncWebParameter* p = r->getParam(name, true)) return p->value();
  if (const AsyncWebParameter* p = r->getParam(name, false)) return p->value();
  return String();
}

// Collects a request body into a NUL-terminated malloc buffer; the server free()s _tempObject.
void collectBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (total > MaxProfileBytes) return;
    r->_tempObject = calloc(total + 1, 1);
  }
  if (r->_tempObject && index + len <= MaxProfileBytes) memcpy(static_cast<char*>(r->_tempObject) + index, data, len);
}

bool readText(const String& path, String& out) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  out = f.readString();
  f.close();
  return true;
}

bool writeText(const String& path, const char* data, size_t len) {
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  f.close();
  if (!ok) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

// Profile file names: a plain basename ending in .toml.
String safeName(String name) {
  const int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
  if (slash >= 0) name = name.substring(slash + 1);
  const int q = name.indexOf('?');
  if (q >= 0) name = name.substring(0, q);
  if (name.length() < 6 || name.length() > 48 || !name.endsWith(".toml")) return String();
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return String();
  }
  return name;
}

// The saved unit.toml (which may differ from the running config until reboot).
bool savedConfig(unitcfg::Config& out, String& error) {
  String text;
  if (!readText(unitcfg::Path, text)) { out = unitcfg::current(); return true; }
  std::vector<String> warnings;
  return unitcfg::parse(text, out, error, warnings);
}

// Path segments after a prefix: "/api/values/2/resume" with "/api/values" -> {"2", "resume"}.
std::vector<String> segments(const String& url, const char* prefix) {
  std::vector<String> out;
  String rest = url.substring(strlen(prefix));
  while (rest.startsWith("/")) rest.remove(0, 1);
  while (rest.length()) {
    const int s = rest.indexOf('/');
    out.push_back(s < 0 ? rest : rest.substring(0, s));
    rest = s < 0 ? String() : rest.substring(s + 1);
  }
  return out;
}

const char* statusName(profile::Status s) {
  return s == profile::Status::Ok ? "ok" : s == profile::Status::NotModbus ? "not_modbus" : "error";
}

String warningsJson(const std::vector<std::string>& w) {
  String j = "[";
  for (size_t i = 0; i < w.size(); ++i) j += (i ? "," : "") + str(w[i]);
  return j + "]";
}
String warningsJson(const std::vector<String>& w) {
  String j = "[";
  for (size_t i = 0; i < w.size(); ++i) j += (i ? "," : "") + str(w[i]);
  return j + "]";
}

// Validation result with the full ref -> wire table (spec 5.3: the user checks the RIOT's addressing with it).
String validationJson(const profile::ParseResult& r, const profile::Profile& p) {
  String j = "{\"status\":\"" + String(statusName(r.status)) + "\",\"message\":" + str(r.message);
  if (r.status != profile::Status::Ok) return j + "}";
  j += ",\"profileId\":" + str(p.profileId) + ",\"vendor\":" + str(p.vendor) + ",\"model\":" + str(p.model);
  j += ",\"addressMode\":\"" + String(profile::toString(p.addressMode)) + "\"";
  j += ",\"tags\":" + String(p.tags.size()) + ",\"mapped\":" + String(p.mappedCount());
  j += ",\"warnings\":" + warningsJson(p.warnings) + ",\"table\":[";
  for (size_t i = 0; i < p.tags.size(); ++i) {
    const auto& t = p.tags[i];
    if (i) j += ',';
    j += "{\"id\":" + str(t.id) + ",\"name\":" + str(t.name) + ",\"fc\":" + String(t.readFc) + ",\"reg\":" + String(t.reg);
    j += ",\"wire\":" + (t.mapped ? String(t.wire) : String("null")) + ",\"space\":\"" + profile::toString(t.space) + "\"";
    j += ",\"type\":\"" + String(profile::toString(t.type)) + "\"";
    if (t.type == profile::DataType::Float32) j += ",\"order\":\"" + String(profile::toString(t.order)) + "\"";
    j += ",\"scale\":" + num(t.scale) + ",\"units\":" + str(t.units) + ",\"writable\":" + boolean(t.writable);
    j += ",\"writeFc\":" + String(t.writeFc) + ",\"mapped\":" + boolean(t.mapped) + "}";
  }
  return j + "]}";
}

// ---------- status ----------
void getStatus(AsyncWebServerRequest* r) {
  const auto s = mbserver::stats();
  const bool connected = wifimgr::state() == wifimgr::State::Connected;
  String j = "{\"name\":" + str(wifimgr::unitName()) + ",\"firmware\":\"" FW_VERSION "\"";
  j += ",\"uptimeS\":" + String(millis() / 1000) + ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"minHeap\":" + String(ESP.getMinFreeHeap());
  j += ",\"wifi\":{\"state\":\"" + String(wifimgr::stateText()) + "\",\"ssid\":" + str(wifimgr::savedWifi().ssid);
  j += ",\"network\":" + str(wifimgr::network()) + ",\"pairedRiot\":" + str(wifimgr::pairedRiot());
  j += ",\"ip\":\"" + (connected ? wifimgr::stationIp().toString() : String()) + "\",\"rssi\":" + String(wifimgr::rssi());
  j += ",\"ap\":" + boolean(wifimgr::apActive()) + ",\"apClients\":" + String(wifimgr::apClients()) + "}";
  j += ",\"modbus\":{\"port\":" + String(unitcfg::current().modbus.port) + ",\"requests\":" + String(s.requests);
  j += ",\"replies\":" + String(s.replies) + ",\"exceptions\":" + String(s.exceptions) + ",\"silent\":" + String(s.silent);
  j += ",\"dropped\":" + String(s.dropped) + ",\"clients\":" + String(s.activeClients) + ",\"totalClients\":" + String(s.totalClients);
  j += ",\"rejectedClients\":" + String(s.rejectedClients);
  j += ",\"lastAgeMs\":" + (s.requests ? String(millis() - s.lastRequestMs) : String("null"));
  j += ",\"lastClient\":\"" + (s.requests ? IPAddress(s.lastClientIp).toString() : String()) + "\"";
  j += ",\"lastUnit\":" + String(s.lastUnit) + ",\"lastFc\":" + String(s.lastFc) + ",\"lastAddress\":" + String(s.lastAddress);
  j += ",\"lastException\":" + String(s.lastException) + ",\"byFc\":{";
  bool first = true;
  for (int fc = 1; fc <= 17; ++fc) {
    if (!s.byFc[fc]) continue;
    j += (first ? "\"" : ",\"") + String(fc == 17 ? 43 : fc) + "\":" + String(s.byFc[fc]);
    first = false;
  }
  j += "},\"exceptionsByCode\":{\"1\":" + String(s.exceptionsByCode[1]) + ",\"2\":" + String(s.exceptionsByCode[2]) +
       ",\"3\":" + String(s.exceptionsByCode[3]) + ",\"4\":" + String(s.exceptionsByCode[4]) + "}}";
  j += ",\"devices\":[";
  first = true;
  for (const auto& d : profiles::devices()) {
    j += (first ? "" : ",") + String("{\"unit\":") + d.unitId + ",\"enabled\":" + boolean(d.enabled) +
         ",\"loaded\":" + boolean(d.status == profile::Status::Ok) + ",\"model\":" + str(d.profile->model) + "}";
    first = false;
  }
  sendJson(r, 200, j + "]}");
}

// ---------- devices ----------
void getDevices(AsyncWebServerRequest* r) {
  String j = "{\"devices\":[";
  bool first = true;
  for (const auto& d : profiles::devices()) {
    const auto& p = *d.profile;
    j += first ? "" : ",";
    first = false;
    j += "{\"unit\":" + String(d.unitId) + ",\"enabled\":" + boolean(d.enabled) + ",\"profile\":" + str(d.file);
    j += ",\"sim\":" + str(d.simFile) + ",\"status\":\"" + statusName(d.status) + "\",\"message\":" + str(d.message);
    if (d.status == profile::Status::Ok) {
      j += ",\"profileId\":" + str(p.profileId) + ",\"vendor\":" + str(p.vendor) + ",\"model\":" + str(p.model);
      j += ",\"tags\":" + String(p.tags.size()) + ",\"mapped\":" + String(p.mappedCount()) + ",\"warnings\":" + warningsJson(p.warnings);
      j += ",\"simEntries\":" + String(d.sim.entries.size()) + ",\"simMessage\":" + str(d.simMessage);
      j += ",\"simWarnings\":" + warningsJson(d.sim.warnings);
    }
    j += "}";
  }
  // What unit.toml will load after the next reboot.
  unitcfg::Config saved;
  String error;
  j += "],\"saved\":[";
  if (savedConfig(saved, error)) {
    for (size_t i = 0; i < saved.devices.size(); ++i) {
      const auto& d = saved.devices[i];
      j += (i ? "," : "") + String("{\"unit\":") + d.unitId + ",\"profile\":" + str(d.profile) + ",\"sim\":" + str(d.sim) +
           ",\"enabled\":" + boolean(d.enabled) + "}";
    }
  }
  sendJson(r, 200, j + "]}");
}

// action=set: unit, profile, sim, enabled. action=delete: unit. Edits the saved unit.toml; applies after reboot.
void postDevices(AsyncWebServerRequest* r) {
  if (!authorized(r)) return r->requestAuthentication();
  unitcfg::Config c;
  String error;
  if (!savedConfig(c, error)) return sendError(r, 500, "Saved unit.toml is invalid: " + error);
  const String action = param(r, "action");
  const long unit = param(r, "unit").toInt();
  if (unit < 1 || unit > 247) return sendError(r, 400, "unit must be 1-247");
  auto it = std::find_if(c.devices.begin(), c.devices.end(), [&](const unitcfg::Device& d) { return d.unitId == unit; });
  if (action == "delete") {
    if (it == c.devices.end()) return sendError(r, 404, "No device with that unit ID");
    c.devices.erase(it);
  } else if (action == "set") {
    const String file = safeName(param(r, "profile"));
    if (!file.length()) return sendError(r, 400, "profile must be a .toml file name");
    if (!LittleFS.exists("/profiles/" + file)) return sendError(r, 404, "/profiles/" + file + " does not exist");
    String sim = param(r, "sim");
    sim.trim();
    if (sim.length() && !sim.endsWith(".sim.toml")) return sendError(r, 400, "sim must be a .sim.toml file name");
    unitcfg::Device d;
    d.unitId = uint8_t(unit);
    d.profile = file;
    d.sim = sim;
    d.enabled = param(r, "enabled") != "0";
    if (it == c.devices.end()) c.devices.push_back(d);
    else *it = d;
    std::sort(c.devices.begin(), c.devices.end(), [](const unitcfg::Device& a, const unitcfg::Device& b) { return a.unitId < b.unitId; });
  } else {
    return sendError(r, 400, "action must be set or delete");
  }
  if (!unitcfg::save(c)) return sendError(r, 500, "Could not write unit.toml");
  sendJson(r, 200, "{\"ok\":true,\"restartRequired\":true}");
}

// ---------- profiles ----------
String storeProfile(const String& name, const char* data, size_t len, int& code) {
  profile::Profile p;
  const auto r = profile::parse(std::string_view(data, len), p);
  String j = "{\"file\":" + str(name) + ",\"validation\":" + validationJson(r, p);
  if (r.status != profile::Status::Ok) {
    code = 422;
    return j + ",\"saved\":false}";
  }
  if (!LittleFS.exists("/profiles")) LittleFS.mkdir("/profiles");
  const bool saved = writeText("/profiles/" + name, data, len);  // byte-for-byte, never reformatted
  code = saved ? 200 : 500;
  return j + ",\"saved\":" + boolean(saved) + "}";
}

void listProfiles(AsyncWebServerRequest* r) {
  unitcfg::Config saved;
  String error;
  savedConfig(saved, error);
  String j = "{\"profiles\":[";
  File dir = LittleFS.open("/profiles");
  bool first = true;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String name = f.name();
    const size_t size = f.size();
    String text = f.readString();
    f.close();
    if (!name.endsWith(".toml")) continue;
    profile::Profile p;
    const auto res = profile::parse(std::string_view(text.c_str(), text.length()), p);
    j += first ? "" : ",";
    first = false;
    j += "{\"file\":" + str(name) + ",\"size\":" + String(size) + ",\"status\":\"" + statusName(res.status) + "\"";
    j += ",\"message\":" + str(res.message) + ",\"profileId\":" + str(p.profileId) + ",\"model\":" + str(p.model);
    j += ",\"vendor\":" + str(p.vendor) + ",\"tags\":" + String(p.tags.size()) + ",\"warnings\":" + String(p.warnings.size());
    j += ",\"usedBy\":[";
    bool firstUnit = true;
    for (const auto& d : saved.devices)
      if (d.profile == name) { j += (firstUnit ? "" : ",") + String(d.unitId); firstUnit = false; }
    j += "]}";
  }
  sendJson(r, 200, j + "]}");
}

void uploadProfile(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  // The multipart body is slightly larger than the file: enough room, without reserving the 48 KB limit.
  const size_t room = r->contentLength() ? r->contentLength() : MaxProfileBytes;
  if (index == 0) {
    if (room > MaxProfileBytes + 1024) return;
    r->_tempObject = calloc(room + 1, 1);
    if (!r->_tempObject) return;
  }
  auto* buf = static_cast<char*>(r->_tempObject);
  if (!buf) return;
  if (index + len > MaxProfileBytes || index + len > room) { free(buf); r->_tempObject = nullptr; return; }
  memcpy(buf + index, data, len);
  (void)filename;
  (void)final;
}

void handleProfilesImpl(AsyncWebServerRequest* r);

// Parsing a big profile is the one place the API can run short of heap: answer with
// an error instead of letting std::bad_alloc abort the whole unit.
void handleProfiles(AsyncWebServerRequest* r) {
  try {
    handleProfilesImpl(r);
  } catch (const std::bad_alloc&) {
    sendError(r, 507, "Not enough memory to process this profile on the test unit (free heap " +
                          String(ESP.getFreeHeap() / 1024) + " KB)");
  }
}

void handleProfilesImpl(AsyncWebServerRequest* r) {
  const auto seg = segments(r->url(), "/api/profiles");
  const auto m = r->method();
  if (seg.empty()) {
    if (m == HTTP_GET) return listProfiles(r);
    if (m == HTTP_POST) {  // multipart upload, field "file"
      if (!authorized(r)) return r->requestAuthentication();
      const char* buf = static_cast<const char*>(r->_tempObject);
      if (!buf) return sendError(r, 413, "Upload a .toml file of at most 48 KB");
      String name;
      if (r->hasParam("file", true, true)) name = r->getParam("file", true, true)->value();
      if (const String n = param(r, "name"); n.length()) name = n;
      name = safeName(name);
      if (!name.length()) return sendError(r, 400, "The file name must end in .toml and use letters, digits, _ - .");
      int code = 200;
      const String body = storeProfile(name, buf, strlen(buf), code);
      return sendJson(r, code, body);
    }
    return sendError(r, 405, "Method not allowed");
  }
  const String name = safeName(seg[0]);
  if (!name.length()) return sendError(r, 400, "Invalid profile file name");
  const String path = "/profiles/" + name;
  if (!LittleFS.exists(path)) return sendError(r, 404, path + " not found");
  if (seg.size() >= 2 && seg[1] == "validate") {
    String text;
    readText(path, text);
    profile::Profile p;
    const auto res = profile::parse(std::string_view(text.c_str(), text.length()), p);
    return sendJson(r, 200, validationJson(res, p));
  }
  if (m == HTTP_GET) {
    AsyncWebServerResponse* res = r->beginResponse(LittleFS, path, "application/toml");
    res->addHeader("Content-Disposition", "attachment; filename=" + name);
    return r->send(res);
  }
  if (m == HTTP_DELETE) {
    if (!authorized(r)) return r->requestAuthentication();
    unitcfg::Config saved;
    String error;
    savedConfig(saved, error);
    for (const auto& d : saved.devices)
      if (d.profile == name) return sendError(r, 409, "Unit " + String(d.unitId) + " uses this profile; change that device first");
    for (const auto& d : profiles::devices())
      if (d.file == name) return sendError(r, 409, "Unit " + String(d.unitId) + " is serving this profile until the next reboot");
    LittleFS.remove(path);
    return sendJson(r, 200, "{\"ok\":true}");
  }
  sendError(r, 405, "Method not allowed");
}

// ---------- values ----------
void handleValues(AsyncWebServerRequest* r) {
  const auto seg = segments(r->url(), "/api/values");
  if (seg.empty()) return sendError(r, 400, "Use /api/values/<unit>");
  const long unit = seg[0].toInt();
  const int di = runtime::deviceIndex(uint8_t(unit));
  if (unit < 1 || di < 0) return sendError(r, 404, "No device with that unit ID");
  auto& st = runtime::values();
  auto& dev = st.devices[size_t(di)];
  const auto& tags = dev.profile->tags;
  const auto m = r->method();

  if (seg.size() >= 2 && seg[1] == "resume") {
    if (!authorized(r)) return r->requestAuthentication();
    const String tag = param(r, "tag");
    if (!tag.length() || tag == "*") { runtime::queueResume(uint8_t(unit), -1); return sendJson(r, 200, "{\"ok\":true}"); }
    for (size_t i = 0; i < tags.size(); ++i)
      if (tag == tags[i].id.c_str()) { runtime::queueResume(uint8_t(unit), int(i)); return sendJson(r, 200, "{\"ok\":true}"); }
    return sendError(r, 404, "No tag " + tag);
  }
  if (m == HTTP_POST || m == HTTP_PUT) {  // override: tag, value (engineering units; bools 0/1)
    if (!authorized(r)) return r->requestAuthentication();
    const String tag = param(r, "tag"), value = param(r, "value");
    char* end = nullptr;
    const double v = strtod(value.c_str(), &end);
    if (!value.length() || (end && *end)) return sendError(r, 400, "value must be a number");
    for (size_t i = 0; i < tags.size(); ++i) {
      if (tag != tags[i].id.c_str()) continue;
      if (!tags[i].mapped) return sendError(r, 409, "Tag is unmapped and not served");
      mb::Guard g(st.lock);
      dev.override(i, v);
      return sendJson(r, 200, "{\"ok\":true,\"value\":" + num(dev.values[i].value) + "}");
    }
    return sendError(r, 404, "No tag " + tag);
  }
  // GET: copy the values under the lock, then format.
  std::vector<store::TagValue> snap;
  {
    mb::Guard g(st.lock);
    snap = dev.values;
  }
  const uint32_t now = millis();
  String j = "{\"unit\":" + String(unit) + ",\"model\":" + str(dev.profile->model) + ",\"frozen\":" +
             boolean(runtime::simulation().frozen(size_t(di))) + ",\"tags\":[";
  for (size_t i = 0; i < tags.size(); ++i) {
    const auto& t = tags[i];
    const auto& v = snap[i];
    sim::Entry e;
    const bool simulated = runtime::simEntry(di, i, e);
    if (i) j += ',';
    j += "{\"id\":" + str(t.id) + ",\"name\":" + str(t.name) + ",\"fc\":" + String(t.readFc);
    j += ",\"wire\":" + (t.mapped ? String(t.wire) : String("null")) + ",\"type\":\"" + profile::toString(t.type) + "\"";
    j += ",\"units\":" + str(t.units) + ",\"writable\":" + boolean(t.writable) + ",\"mapped\":" + boolean(t.mapped);
    j += ",\"value\":" + num(v.value) + ",\"raw\":[" + String(v.words[0]);
    if (profile::wordCount(t.type) == 2) j += "," + String(v.words[1]);
    j += "],\"overridden\":" + boolean(v.overridden) + ",\"sim\":\"" + String(simulated ? sim::toString(e.mode) : "static") + "\"";
    j += ",\"reads\":" + String(v.readCount) + ",\"lastReadAgoMs\":" + (v.readCount ? String(now - v.lastReadMs) : String("null"));
    j += ",\"pollMs\":" + (v.pollIntervalMs ? String(v.pollIntervalMs) : String("null"));
    j += ",\"writes\":" + String(v.writeCount) + ",\"lastWriteAgoMs\":" + (v.writeCount ? String(now - v.lastWriteMs) : String("null")) + "}";
  }
  sendJson(r, 200, j + "]}");
}

// ---------- sim ----------
void handleSimImpl(AsyncWebServerRequest* r);
void handleSim(AsyncWebServerRequest* r) {
  try {
    handleSimImpl(r);
  } catch (const std::bad_alloc&) {
    sendError(r, 507, "Not enough memory to process this simulation file on the test unit");
  }
}

void handleSimImpl(AsyncWebServerRequest* r) {
  const auto seg = segments(r->url(), "/api/sim");
  if (seg.empty()) return sendError(r, 400, "Use /api/sim/<unit>");
  const long unit = seg[0].toInt();
  const profiles::LoadedDevice* d = runtime::loaded(uint8_t(unit));
  if (!d || d->status != profile::Status::Ok) return sendError(r, 404, "No loaded device with that unit ID");
  const String simFile = d->simFile.length() ? d->simFile : String(d->profile->profileId.c_str()) + ".sim.toml";
  const String path = "/sims/" + simFile;
  if (seg.size() >= 2 && seg[1] == "template") {
    AsyncWebServerResponse* res = r->beginResponse(200, "application/toml", sim::templateToml(*d->profile).c_str());
    res->addHeader("Content-Disposition", "attachment; filename=" + String(d->profile->profileId.c_str()) + ".sim.toml");
    return r->send(res);
  }
  if (r->method() == HTTP_GET) {
    String text;
    if (!readText(path, text)) text = sim::toToml(d->sim).c_str();
    return r->send(200, "application/toml", text);
  }
  if (r->method() == HTTP_PUT || r->method() == HTTP_POST) {
    if (!authorized(r)) return r->requestAuthentication();
    const char* body = static_cast<const char*>(r->_tempObject);
    if (!body || !*body) return sendError(r, 400, "Send the .sim.toml text as the request body");
    sim::Config cfg;
    std::string error;
    if (!sim::parse(std::string_view(body, strlen(body)), *d->profile, cfg, error)) return sendError(r, 422, error.c_str());
    if (!LittleFS.exists("/sims")) LittleFS.mkdir("/sims");
    if (!writeText(path, body, strlen(body))) return sendError(r, 500, "Could not write " + path);
    runtime::queueSimApply(uint8_t(unit), cfg);  // live, no reboot
    return sendJson(r, 200, "{\"ok\":true,\"file\":" + str(simFile) + ",\"entries\":" + String(cfg.entries.size()) +
                                ",\"warnings\":" + warningsJson(cfg.warnings) + "}");
  }
  sendError(r, 405, "Method not allowed");
}

// ---------- config ----------
void putConfig(AsyncWebServerRequest* r) {
  if (!authorized(r)) return r->requestAuthentication();
  const char* body = static_cast<const char*>(r->_tempObject);
  if (!body || !*body || strlen(body) > MaxConfigBytes)
    return sendError(r, 400, "Send unit.toml as the request body (max 8 KB)");
  unitcfg::Config next;
  String error;
  std::vector<String> warnings;
  if (!unitcfg::parse(String(body), next, error, warnings)) return sendError(r, 400, error);
  // Spec 7: a redacted password on import keeps the current one. The running config is
  // left alone (this runs on AsyncTCP's task); the saved file takes effect after a reboot.
  const auto& cur = unitcfg::current();
  if (next.wifi.password == unitcfg::RedactedPassword) next.wifi.password = cur.wifi.password;
  if (next.apPassword == unitcfg::RedactedPassword) next.apPassword = cur.apPassword;
  if (next.pairing.password == unitcfg::RedactedPassword) next.pairing.password = cur.pairing.password;
  if (next.adminPassword == unitcfg::RedactedPassword) next.adminPassword = cur.adminPassword;
  if (!unitcfg::save(next)) return sendError(r, 500, "Could not write /config/unit.toml");
  sendJson(r, 200, "{\"ok\":true,\"restartRequired\":true,\"warnings\":" + warningsJson(warnings) + "}");
}

// ---------- websocket ----------
void pushLive() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  ws.cleanupClients();
  if (!ws.count()) return;
  const auto s = mbserver::stats();
  String j = "{\"t\":\"status\",\"requests\":" + String(s.requests) + ",\"exceptions\":" + String(s.exceptions) +
             ",\"clients\":" + String(s.activeClients) + ",\"lastAgeMs\":" + (s.requests ? String(millis() - s.lastRequestMs) : String("null")) +
             ",\"lastUnit\":" + String(s.lastUnit) + ",\"lastFc\":" + String(s.lastFc) + ",\"lastAddress\":" + String(s.lastAddress) +
             ",\"lastException\":" + String(s.lastException) + ",\"heap\":" + String(ESP.getFreeHeap()) +
             ",\"rssi\":" + String(wifimgr::rssi()) + "}";
  ws.textAll(j);
  auto& st = runtime::values();
  for (auto& dev : st.devices) {
    std::vector<store::TagValue> snap;
    {
      mb::Guard g(st.lock);
      snap = dev.values;
    }
    String v = "{\"t\":\"values\",\"unit\":" + String(dev.unitId) + ",\"v\":[";
    String o = "],\"o\":[", n = "],\"r\":[";
    for (size_t i = 0; i < snap.size(); ++i) {
      v += (i ? "," : "") + num(snap[i].value);
      o += (i ? "," : "") + String(snap[i].overridden ? 1 : 0);
      n += (i ? "," : "") + String(snap[i].readCount);
    }
    ws.textAll(v + o + n + "]}");
  }
}
}  // namespace

bool authorized(AsyncWebServerRequest* r) {
  const String& pw = unitcfg::current().adminPassword;
  return !pw.length() || r->authenticate("admin", pw.c_str());
}

void registerRoutes(AsyncWebServer& server) {
  server.addHandler(&ws);
  server.on("/api/status", HTTP_GET, getStatus);
  server.on("/api/devices", HTTP_GET, getDevices);
  server.on("/api/devices", HTTP_POST, postDevices);
  server.on("/api/profiles", HTTP_ANY, handleProfiles, uploadProfile);
  server.on("/api/values", HTTP_ANY, handleValues);
  server.on("/api/sim", HTTP_ANY, handleSim, nullptr, collectBody);
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "application/toml", unitcfg::toToml(unitcfg::current(), true));
    res->addHeader("Content-Disposition", "inline; filename=unit.toml");
    r->send(res);
  });
  server.on("/api/config", HTTP_PUT, putConfig, nullptr, collectBody);
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!authorized(r)) return r->requestAuthentication();
    sendJson(r, 202, "{\"ok\":true,\"rebootingInMs\":500}");
    rebootAt = millis() + 500;  // let the response go out first
  });
}

void service() {
  pushLive();
  if (rebootAt && int32_t(millis() - rebootAt) >= 0) ESP.restart();
}
}  // namespace api
