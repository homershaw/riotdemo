#include "touch.h"
#include <LittleFS.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <toml++/toml.hpp>
#include "display.h"
#include "../profiles/toml_writer.h"

namespace touch {
namespace {
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen ts(PIN_TOUCH_CS, PIN_TOUCH_IRQ);
Calibration cal;
constexpr int16_t PressureMin = 300;

struct Raw { int32_t x = 0, y = 0; };

int16_t mapAxis(int32_t raw, int16_t at0, int16_t atMax, int16_t size) {
  if (atMax == at0) return 0;
  const int32_t v = (raw - at0) * int32_t(size - 1) / (atMax - at0);
  return int16_t(constrain(v, 0, size - 1));
}

// Waits for a press, averages the raw reading while held, then waits for release.
Raw sampleTarget(int16_t sx, int16_t sy) {
  auto& tft = display::tft;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 12, 2);
  tft.drawString("Tap the centre of the cross", SCREEN_W / 2, SCREEN_H / 2 + 8, 2);
  tft.drawFastHLine(sx - 10, sy, 21, TFT_YELLOW);
  tft.drawFastVLine(sx, sy - 10, 21, TFT_YELLOW);
  tft.drawCircle(sx, sy, 6, TFT_YELLOW);

  while (ts.touched()) delay(10);  // release from the previous target
  delay(150);
  Raw sum;
  int count = 0;
  for (;;) {
    if (ts.touched()) {
      const TS_Point p = ts.getPoint();
      if (p.z >= PressureMin) { sum.x += p.x; sum.y += p.y; ++count; }
    } else if (count >= 8) {
      break;
    }
    delay(10);
  }
  tft.fillCircle(sx, sy, 6, TFT_GREEN);
  delay(200);
  return {sum.x / count, sum.y / count};
}

bool save() {
  if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
  TomlWriter w;
  w.comment("Touch calibration for this panel. Delete this file or hold the screen at power-up to recalibrate.")
      .kv("schema_version", 1)
      .table("touch")
      .kv("swap_xy", cal.swapXY)
      .kv("x_raw_at_0", int(cal.xRawAt0))
      .kv("x_raw_at_max", int(cal.xRawAtMax))
      .kv("y_raw_at_0", int(cal.yRawAt0))
      .kv("y_raw_at_max", int(cal.yRawAtMax));
  File f = LittleFS.open(CalibrationPath, FILE_WRITE);
  if (!f) return false;
  const size_t written = f.print(w.str());
  f.close();
  return written == w.str().length();
}
}  // namespace

void begin() {
  touchSpi.begin(PIN_TOUCH_CLK, PIN_TOUCH_MISO, PIN_TOUCH_MOSI, PIN_TOUCH_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);
}

bool loadCalibration() {
  if (!LittleFS.exists(CalibrationPath)) return false;
  File f = LittleFS.open(CalibrationPath, FILE_READ);
  if (!f) return false;
  const String text = f.readString();
  f.close();
  auto result = toml::parse(std::string_view(text.c_str(), text.length()));
  if (!result) return false;
  const auto& t = result.table();
  const auto* touch = t["touch"].as_table();
  if (!touch) return false;
  Calibration c;
  c.swapXY = (*touch)["swap_xy"].value_or(false);
  c.xRawAt0 = (*touch)["x_raw_at_0"].value_or<int64_t>(-1);
  c.xRawAtMax = (*touch)["x_raw_at_max"].value_or<int64_t>(-1);
  c.yRawAt0 = (*touch)["y_raw_at_0"].value_or<int64_t>(-1);
  c.yRawAtMax = (*touch)["y_raw_at_max"].value_or<int64_t>(-1);
  if (c.xRawAt0 < 0 || c.xRawAtMax < 0 || c.yRawAt0 < 0 || c.yRawAtMax < 0 ||
      c.xRawAt0 == c.xRawAtMax || c.yRawAt0 == c.yRawAtMax) return false;
  cal = c;
  return true;
}

bool touchedNow() { return ts.touched(); }

bool runCalibration() {
  constexpr int16_t M = 24;  // target inset from the edges
  const Raw a = sampleTarget(M, M);                         // top left
  const Raw b = sampleTarget(SCREEN_W - 1 - M, M);          // top right: only screen x changes
  const Raw c = sampleTarget(M, SCREEN_H - 1 - M);          // bottom left: only screen y changes

  Calibration n;
  // Whichever raw axis moved most between a and b follows screen x.
  n.swapXY = abs(b.y - a.y) > abs(b.x - a.x);
  const int32_t ax = n.swapXY ? a.y : a.x, bx = n.swapXY ? b.y : b.x;
  const int32_t ay = n.swapXY ? a.x : a.y, cy = n.swapXY ? c.x : c.y;
  // Extrapolate from the inset targets to the screen edges.
  const int32_t spanX = SCREEN_W - 1 - 2 * M, spanY = SCREEN_H - 1 - 2 * M;
  n.xRawAt0 = ax - (bx - ax) * M / spanX;
  n.xRawAtMax = bx + (bx - ax) * M / spanX;
  n.yRawAt0 = ay - (cy - ay) * M / spanY;
  n.yRawAtMax = cy + (cy - ay) * M / spanY;
  if (n.xRawAt0 == n.xRawAtMax || n.yRawAt0 == n.yRawAtMax) return false;
  cal = n;
  const bool ok = save();
  Serial.printf("Touch calibration %s: swap=%d x=%d..%d y=%d..%d\n", ok ? "saved" : "NOT saved",
                cal.swapXY, cal.xRawAt0, cal.xRawAtMax, cal.yRawAt0, cal.yRawAtMax);
  display::tft.fillScreen(TFT_BLACK);
  return ok;
}

Point read() {
  Point p;
  if (!ts.touched()) return p;
  const TS_Point r = ts.getPoint();
  if (r.z < PressureMin) return p;
  const int32_t rx = cal.swapXY ? r.y : r.x, ry = cal.swapXY ? r.x : r.y;
  p.x = mapAxis(rx, cal.xRawAt0, cal.xRawAtMax, SCREEN_W);
  p.y = mapAxis(ry, cal.yRawAt0, cal.yRawAtMax, SCREEN_H);
  p.pressed = true;
  return p;
}

const Calibration& calibration() { return cal; }
}  // namespace touch
