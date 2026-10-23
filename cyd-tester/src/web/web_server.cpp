#include "web_server.h"
#include "../net/captive_portal.h"
#include "api.h"
#include "www_assets.h"

namespace web {
AsyncWebServer server(80);

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (uint8_t(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

void begin() {
  www::install();
  www::registerRoutes(server);
  api::registerRoutes(server);
  portal::registerRoutes(server);
  server.begin();
}
}  // namespace web
