#include "www_assets.h"
#include <LittleFS.h>

#define EMBEDDED(sym)                                                     \
  extern const uint8_t sym##_start[] asm("_binary_data_www_" #sym "_gz_start"); \
  extern const uint8_t sym##_end[] asm("_binary_data_www_" #sym "_gz_end");
EMBEDDED(index_html)
EMBEDDED(app_js)
EMBEDDED(style_css)

namespace www {
namespace {
struct Asset {
  const char* path;
  const char* type;
  const uint8_t* start;
  const uint8_t* end;
};
const Asset assets[] = {
    {"/www/index.html.gz", "text/html", index_html_start, index_html_end},
    {"/www/app.js.gz", "application/javascript", app_js_start, app_js_end},
    {"/www/style.css.gz", "text/css", style_css_start, style_css_end},
};
// Fingerprint of the embedded files: the copy on LittleFS is refreshed exactly when the UI changes.
String fingerprint() {
  uint32_t h = 2166136261u;
  for (const auto& a : assets)
    for (const uint8_t* p = a.start; p < a.end; ++p) h = (h ^ *p) * 16777619u;
  char b[12];
  snprintf(b, sizeof b, "%08x", unsigned(h));
  return b;
}

void send(AsyncWebServerRequest* r, const Asset& a) {
  if (!LittleFS.exists(a.path)) { r->send(503, "text/plain", "Web UI files missing; reboot to reinstall"); return; }
  AsyncWebServerResponse* res = r->beginResponse(LittleFS, a.path, a.type);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  r->send(res);
}
}  // namespace

void install() {
  if (!LittleFS.exists("/www")) LittleFS.mkdir("/www");
  File v = LittleFS.open("/www/.build", FILE_READ);
  const String installed = v ? v.readString() : String();
  if (v) v.close();
  const String build = fingerprint();
  if (installed == build) return;
  size_t total = 0;
  for (const auto& a : assets) {
    File f = LittleFS.open(a.path, FILE_WRITE);
    if (!f) { Serial.printf("Web UI: cannot write %s\n", a.path); return; }
    const size_t len = size_t(a.end - a.start);
    f.write(a.start, len);
    f.close();
    total += len;
  }
  File b = LittleFS.open("/www/.build", FILE_WRITE);
  if (b) { b.print(build); b.close(); }
  Serial.printf("Web UI installed to /www (%u bytes gzipped)\n", unsigned(total));
}

void sendIndex(AsyncWebServerRequest* r) { send(r, assets[0]); }

void registerRoutes(AsyncWebServer& server) {
  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* r) { send(r, assets[1]); });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* r) { send(r, assets[2]); });
}
}  // namespace www
