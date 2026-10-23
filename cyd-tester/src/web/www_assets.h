#pragma once
#include <ESPAsyncWebServer.h>

// The web UI: gzipped at build time (tools/gzip_www.py), embedded in the firmware,
// copied to /www on LittleFS when the build changes, and served from there.
namespace www {
void install();
void registerRoutes(AsyncWebServer& server);
// The UI's index page (also used by the portal's "/" on the station side).
void sendIndex(AsyncWebServerRequest* r);
}  // namespace www
