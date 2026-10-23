#include "led.h"

namespace led {
namespace {
Color flashColor = Color::Off;
uint32_t flashUntil = 0;
Pattern pattern = Pattern::Off;

void write(Color c) {
  const bool r = c == Color::Red || c == Color::Yellow || c == Color::White;
  const bool g = c == Color::Green || c == Color::Yellow || c == Color::White;
  const bool b = c == Color::Blue || c == Color::White;
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

Color patternColor(uint32_t now) {
  switch (pattern) {
    case Pattern::BlueSolid: return Color::Blue;
    case Pattern::YellowSlowBlink: return (now / 1000) % 2 ? Color::Yellow : Color::Off;
    default: return Color::Off;
  }
}
}  // namespace

void begin() {
  for (int pin : {PIN_LED_R, PIN_LED_G, PIN_LED_B}) pinMode(pin, OUTPUT);
  write(Color::Off);
}

void flash(Color color, uint16_t ms) {
  flashColor = color;
  flashUntil = millis() + ms;
  write(color);
}

void setPattern(Pattern p) { pattern = p; }

void service() {
  const uint32_t now = millis();
  write(flashColor != Color::Off && int32_t(flashUntil - now) > 0 ? flashColor : patternColor(now));
  if (int32_t(flashUntil - now) <= 0) flashColor = Color::Off;
}
}  // namespace led
