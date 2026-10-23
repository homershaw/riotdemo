#pragma once
#include <ESPAsyncWebServer.h>

// REST API under /api (PROMPT.md section 11). JSON over HTTP, except the
// configuration itself, which is TOML.
namespace api {
void registerRoutes(AsyncWebServer& server);
// True if the request may change state: no admin password set, or valid Basic auth.
bool authorized(AsyncWebServerRequest* r);
// Main-loop work requested by API calls (deferred reboot).
void service();
}  // namespace api
