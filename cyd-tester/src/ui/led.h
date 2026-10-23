#pragma once
#include <Arduino.h>

// Onboard RGB LED (active LOW). Non-blocking: flash() starts a timed pulse,
// service() ends it, and a background pattern (off / blink) shows between pulses.
namespace led {
enum class Color : uint8_t { Off, Red, Green, Blue, Yellow, White };
enum class Pattern : uint8_t { Off, BlueSolid, YellowSlowBlink };

void begin();
void flash(Color color, uint16_t ms = 30);
void setPattern(Pattern pattern);
void service();
}  // namespace led
