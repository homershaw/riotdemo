#pragma once
#include <Arduino.h>

// XPT2046 touch on its own VSPI bus, mapped to screen coordinates with a
// calibration stored in /config/touch.toml.
namespace touch {
struct Calibration {
  bool swapXY = false;
  int16_t xRawAt0 = 300, xRawAtMax = 3800;  // raw value at screen x = 0 and x = SCREEN_W - 1
  int16_t yRawAt0 = 300, yRawAtMax = 3800;  // raw value at screen y = 0 and y = SCREEN_H - 1
};
struct Point {
  int16_t x = 0, y = 0;
  bool pressed = false;
};

constexpr const char* CalibrationPath = "/config/touch.toml";

void begin();
// True when a saved calibration was loaded.
bool loadCalibration();
// True while the panel is touched right now (raw, uncalibrated check).
bool touchedNow();
// Interactive 3-target calibration on the display; saves the result.
bool runCalibration();
Point read();
const Calibration& calibration();
}  // namespace touch
