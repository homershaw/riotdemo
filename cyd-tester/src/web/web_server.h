#pragma once
#include <ESPAsyncWebServer.h>

// The single HTTP server on port 80, shared by the captive portal (phase 2)
// and the web UI / REST API (phase 6).
namespace web {
extern AsyncWebServer server;
void begin();
String jsonEscape(const String& s);
}  // namespace web
