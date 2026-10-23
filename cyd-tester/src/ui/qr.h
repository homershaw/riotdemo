#pragma once
#include <TFT_eSPI.h>

namespace qr {
// Draws `text` as a QR code (black on white, with a quiet zone) in a box of
// `box` pixels at (x, y). Returns false if the text does not fit.
bool draw(TFT_eSPI& tft, const char* text, int16_t x, int16_t y, int16_t box);
}  // namespace qr
