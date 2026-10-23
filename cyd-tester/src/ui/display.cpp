#include "display.h"

namespace display {
TFT_eSPI tft;
namespace {
constexpr uint8_t BacklightChannel = 7;  // LEDC channel kept clear of other users
}

void begin(uint8_t rotation, uint8_t brightnessPercent) {
  ledcSetup(BacklightChannel, 5000, 8);
  ledcAttachPin(TFT_BL, BacklightChannel);
  setBrightness(0);
  tft.init();
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);
  setBrightness(brightnessPercent);
}

void setBrightness(uint8_t percent) {
  if (percent > 100) percent = 100;
  ledcWrite(BacklightChannel, uint32_t(percent) * 255 / 100);
}
}  // namespace display
