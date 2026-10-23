#include "unit_config.h"
#include <LittleFS.h>
#include <toml++/toml.hpp>
#include "toml_writer.h"

namespace unitcfg {
namespace {
Config live = defaults();

using Warnings = std::vector<String>;

void warn(Warnings& w, const String& msg) { w.push_back(msg); }

void unknownKeys(const toml::table& t, std::initializer_list<const char*> known, const String& where, Warnings& w) {
  for (const auto& [key, node] : t) {
    bool found = false;
    for (const char* k : known) found |= key.str() == k;
    if (!found) warn(w, "Unknown key " + where + std::string(key.str()).c_str() + " ignored");
  }
}

void getString(const toml::table& t, const char* key, String& out, const String& where, Warnings& w) {
  if (const auto* n = t.get(key)) {
    if (auto v = n->value<std::string>()) out = v->c_str();
    else warn(w, where + key + " must be a string; default kept");
  }
}
template <typename T>
void getInt(const toml::table& t, const char* key, T& out, long lo, long hi, const String& where, Warnings& w) {
  if (const auto* n = t.get(key)) {
    auto v = n->value<int64_t>();
    if (v && *v >= lo && *v <= hi) out = T(*v);
    else warn(w, where + key + " must be an integer " + String(lo) + ".." + String(hi) + "; default kept");
  }
}
void getBool(const toml::table& t, const char* key, bool& out, const String& where, Warnings& w) {
  if (const auto* n = t.get(key)) {
    if (auto v = n->value<bool>()) out = *v;
    else warn(w, where + key + " must be true or false; default kept");
  }
}
}  // namespace

Config defaults() {
  Config c;
  Device d;
  d.unitId = 1;
  d.profile = "test_bench.toml";
  d.sim = "test_bench.sim.toml";
  c.devices.push_back(d);
  // Unit 2 answers like a RIOT M31 rack, for testing the RIOT's built-in M31 driver.
  d.unitId = 2;
  d.profile = "riot_m31_emulation.toml";
  d.sim = "riot_m31_emulation.sim.toml";
  c.devices.push_back(d);
  return c;
}

bool parse(const String& text, Config& out, String& error, Warnings& w) {
  auto result = toml::parse(std::string_view(text.c_str(), text.length()));
  if (!result) {
    const auto& e = result.error();
    error = String("TOML error at line ") + e.source().begin.line + ": " + std::string(e.description()).c_str();
    return false;
  }
  const toml::table& root = result.table();
  Config c = defaults();
  unknownKeys(root, {"schema_version", "wifi", "pairing", "portal", "modbus", "devices", "web", "display"}, "", w);
  int64_t schema = root["schema_version"].value_or<int64_t>(1);
  if (schema != 1) warn(w, "schema_version " + String(long(schema)) + " is newer than this firmware understands");

  if (const auto* t = root["wifi"].as_table()) {
    unknownKeys(*t, {"ssid", "password", "ip_mode", "static_ip", "netmask", "gateway", "dns", "connect_timeout_s", "portal_after_s"}, "wifi.", w);
    getString(*t, "ssid", c.wifi.ssid, "wifi.", w);
    getString(*t, "password", c.wifi.password, "wifi.", w);
    String mode = c.wifi.staticIp ? "static" : "dhcp";
    getString(*t, "ip_mode", mode, "wifi.", w);
    if (mode == "static") c.wifi.staticIp = true;
    else if (mode == "dhcp") c.wifi.staticIp = false;
    else warn(w, "wifi.ip_mode must be \"dhcp\" or \"static\"; using dhcp");
    getString(*t, "static_ip", c.wifi.ip, "wifi.", w);
    getString(*t, "netmask", c.wifi.netmask, "wifi.", w);
    getString(*t, "gateway", c.wifi.gateway, "wifi.", w);
    getString(*t, "dns", c.wifi.dns, "wifi.", w);
    getInt(*t, "connect_timeout_s", c.wifi.connectTimeoutS, 5, 300, "wifi.", w);
    getInt(*t, "portal_after_s", c.wifi.portalAfterS, 10, 3600, "wifi.", w);
  }
  if (const auto* t = root["pairing"].as_table()) {
    unknownKeys(*t, {"enabled", "ssid_prefix", "password", "ip", "netmask", "gateway"}, "pairing.", w);
    getBool(*t, "enabled", c.pairing.enabled, "pairing.", w);
    getString(*t, "ssid_prefix", c.pairing.ssidPrefix, "pairing.", w);
    getString(*t, "password", c.pairing.password, "pairing.", w);
    getString(*t, "ip", c.pairing.ip, "pairing.", w);
    getString(*t, "netmask", c.pairing.netmask, "pairing.", w);
    getString(*t, "gateway", c.pairing.gateway, "pairing.", w);
    if (!c.pairing.ssidPrefix.length()) { warn(w, "pairing.ssid_prefix is empty; pairing disabled"); c.pairing.enabled = false; }
  }
  if (const auto* t = root["portal"].as_table()) {
    unknownKeys(*t, {"ap_password"}, "portal.", w);
    getString(*t, "ap_password", c.apPassword, "portal.", w);
    if (c.apPassword.length() && (c.apPassword.length() < 8 || c.apPassword.length() > 63)) {
      warn(w, "portal.ap_password must be 8-63 characters; setup AP stays open");
      c.apPassword = "";
    }
  }
  if (const auto* t = root["modbus"].as_table()) {
    unknownKeys(*t, {"port", "max_clients", "respond_to_any_unit_id", "unmapped_read", "strict_function_codes", "idle_timeout_s"}, "modbus.", w);
    getInt(*t, "port", c.modbus.port, 1, 65535, "modbus.", w);
    getInt(*t, "max_clients", c.modbus.maxClients, 1, 8, "modbus.", w);
    getBool(*t, "respond_to_any_unit_id", c.modbus.respondToAnyUnitId, "modbus.", w);
    String unmapped = "exception";
    getString(*t, "unmapped_read", unmapped, "modbus.", w);
    if (unmapped == "zero") c.modbus.unmappedReadZero = true;
    else if (unmapped != "exception") warn(w, "modbus.unmapped_read must be \"exception\" or \"zero\"; using exception");
    getBool(*t, "strict_function_codes", c.modbus.strictFunctionCodes, "modbus.", w);
    getInt(*t, "idle_timeout_s", c.modbus.idleTimeoutS, 0, 3600, "modbus.", w);
  }
  if (const auto* arr = root["devices"].as_array()) {
    c.devices.clear();
    size_t i = 0;
    for (const auto& node : *arr) {
      const String where = "devices[" + String(i++) + "].";
      const auto* t = node.as_table();
      if (!t) { warn(w, where + " is not a table; skipped"); continue; }
      unknownKeys(*t, {"unit_id", "profile", "sim", "enabled"}, where, w);
      Device d;
      getInt(*t, "unit_id", d.unitId, 1, 247, where, w);
      getString(*t, "profile", d.profile, where, w);
      getString(*t, "sim", d.sim, where, w);
      getBool(*t, "enabled", d.enabled, where, w);
      if (!d.profile.length()) { warn(w, where + "profile missing; device skipped"); continue; }
      bool duplicate = false;
      for (const auto& other : c.devices) duplicate |= other.unitId == d.unitId;
      if (duplicate) { warn(w, where + "unit_id " + String(d.unitId) + " already used; device skipped"); continue; }
      c.devices.push_back(d);
    }
  }
  if (const auto* t = root["web"].as_table()) {
    unknownKeys(*t, {"admin_password"}, "web.", w);
    getString(*t, "admin_password", c.adminPassword, "web.", w);
  }
  if (const auto* t = root["display"].as_table()) {
    unknownKeys(*t, {"rotation", "brightness", "sleep_after_s"}, "display.", w);
    getInt(*t, "rotation", c.display.rotation, 0, 3, "display.", w);
    getInt(*t, "brightness", c.display.brightness, 5, 100, "display.", w);
    getInt(*t, "sleep_after_s", c.display.sleepAfterS, 0, 86400, "display.", w);
  }
  out = c;
  return true;
}

String toToml(const Config& c, bool redact) {
  auto pw = [&](const String& p) { return redact && p.length() ? String(RedactedPassword) : p; };
  TomlWriter w;
  w.comment("RIOT Modbus Test Unit - device configuration").kv("schema_version", 1);
  w.table("wifi")
      .kv("ssid", c.wifi.ssid).kv("password", pw(c.wifi.password))
      .kv("ip_mode", c.wifi.staticIp ? "static" : "dhcp")
      .kv("static_ip", c.wifi.ip).kv("netmask", c.wifi.netmask)
      .kv("gateway", c.wifi.gateway).kv("dns", c.wifi.dns)
      .kv("connect_timeout_s", int(c.wifi.connectTimeoutS)).kv("portal_after_s", int(c.wifi.portalAfterS));
  w.table("pairing")
      .kv("enabled", c.pairing.enabled).kv("ssid_prefix", c.pairing.ssidPrefix)
      .kv("password", pw(c.pairing.password)).kv("ip", c.pairing.ip)
      .kv("netmask", c.pairing.netmask).kv("gateway", c.pairing.gateway);
  w.table("portal").kv("ap_password", pw(c.apPassword));
  w.table("modbus")
      .kv("port", int(c.modbus.port)).kv("max_clients", int(c.modbus.maxClients))
      .kv("respond_to_any_unit_id", c.modbus.respondToAnyUnitId)
      .kv("unmapped_read", c.modbus.unmappedReadZero ? "zero" : "exception")
      .kv("strict_function_codes", c.modbus.strictFunctionCodes)
      .kv("idle_timeout_s", int(c.modbus.idleTimeoutS));
  for (const auto& d : c.devices) {
    w.arrayTable("devices").kv("unit_id", int(d.unitId)).kv("profile", d.profile);
    if (d.sim.length()) w.kv("sim", d.sim);
    w.kv("enabled", d.enabled);
  }
  w.table("web").kv("admin_password", pw(c.adminPassword));
  w.table("display")
      .kv("rotation", int(c.display.rotation)).kv("brightness", int(c.display.brightness))
      .kv("sleep_after_s", long(c.display.sleepAfterS));
  return w.str();
}

Config& current() { return live; }

bool load(String& error, Warnings& warnings) {
  if (!LittleFS.exists(Path)) {
    live = defaults();
    if (!save()) { error = "Could not write default unit.toml"; return false; }
    warnings.push_back("unit.toml created from defaults");
    return true;
  }
  File f = LittleFS.open(Path, FILE_READ);
  if (!f) { error = "Could not open unit.toml"; live = defaults(); return false; }
  const String text = f.readString();
  f.close();
  Config parsed;
  if (!parse(text, parsed, error, warnings)) { live = defaults(); return false; }
  live = parsed;
  return true;
}

bool save() { return save(live); }

bool save(const Config& c) {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  const String text = toToml(c, false);
  // Write to a temp file and rename so a power cut never leaves a half-written config.
  const char* tmp = "/config/unit.toml.tmp";
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) return false;
  const bool ok = f.print(text) == text.length();
  f.close();
  if (!ok) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(Path);
  return LittleFS.rename(tmp, Path);
}
}  // namespace unitcfg
