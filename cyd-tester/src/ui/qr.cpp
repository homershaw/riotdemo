#include "qr.h"
#include <qrcode.h>

namespace qr {
bool draw(TFT_eSPI& tft, const char* text, int16_t x, int16_t y, int16_t box) {
  for (uint8_t version = 2; version <= 6; ++version) {
    QRCode code;
    uint8_t buffer[qrcode_getBufferSize(6)];
    if (qrcode_initText(&code, buffer, version, ECC_LOW, text) != 0) continue;
    const int16_t modules = code.size + 4;  // 2-module quiet zone each side
    const int16_t scale = box / modules;
    if (scale < 1) return false;
    const int16_t size = modules * scale;
    const int16_t ox = x + (box - size) / 2, oy = y + (box - size) / 2;
    tft.fillRect(ox, oy, size, size, TFT_WHITE);
    for (uint8_t my = 0; my < code.size; ++my)
      for (uint8_t mx = 0; mx < code.size; ++mx)
        if (qrcode_getModule(&code, mx, my))
          tft.fillRect(ox + (mx + 2) * scale, oy + (my + 2) * scale, scale, scale, TFT_BLACK);
    return true;
  }
  return false;
}
}  // namespace qr
