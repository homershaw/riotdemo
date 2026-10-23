#pragma once
#include <TFT_eSPI.h>

// ILI9341 display and PWM backlight.
namespace display {
extern TFT_eSPI tft;
void begin(uint8_t rotation, uint8_t brightnessPercent);
void setBrightness(uint8_t percent);
}  // namespace display
