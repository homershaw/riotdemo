#pragma once
#include <ESPAsyncWebServer.h>

// Captive portal for the riot-XXXX setup AP (PROMPT.md section 4.2).
namespace portal {
void registerRoutes(AsyncWebServer& server);
void start();    // DNS capture on the AP interface
void stop();
void service();  // DNS requests
}  // namespace portal
