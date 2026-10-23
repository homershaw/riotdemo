// RIOT Modbus TCP Test Unit (CYD-TESTER)
// Board bring-up, Wi-Fi and portal, profiles, Modbus TCP server, simulation and web API.
#include <Arduino.h>
#include "version.h"
#include <LittleFS.h>
#include "app/runtime.h"
#include "modbus/server.h"
#include "net/wifi_manager.h"
#include "profiles/profile_store.h"
#include "profiles/unit_config.h"
#include "ui/button.h"
#include "ui/display.h"
#include "ui/led.h"
#include "ui/qr.h"
#include "ui/touch.h"
#include "web/api.h"
#include "web/web_server.h"


namespace {
auto& tft = display::tft;
constexpr int16_t ValueX = 74, RowY = 40, RowH = 20;
constexpr int16_t ButtonX = 8, ButtonY = 198, ButtonW = 150, ButtonH = 36;
bool apShown = false;

void header() {
  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("CYD-TESTER", 6, 13, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(FW_VERSION, SCREEN_W - 6, 13, 2);
}

void drawLayout(bool ap) {
  tft.fillScreen(TFT_BLACK);
  header();
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  const char* labels[] = {"Wi-Fi", "SSID", "IP", "RSSI", "Host", "Devices", "Modbus"};
  for (int i = 0; i < 7; ++i) tft.drawString(labels[i], 8, RowY + i * RowH, 2);
  tft.drawFastHLine(0, 172, SCREEN_W, TFT_DARKGREY);
  if (ap) {
    const String& name = wifimgr::unitName();
    const String& pw = unitcfg::current().apPassword;
    const String payload = pw.length() ? "WIFI:T:WPA;S:" + name + ";P:" + pw + ";;" : "WIFI:T:nopass;S:" + name + ";;";
    qr::draw(tft, payload.c_str(), 214, 30, 100);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(name, 264, 136, 2);
  } else {
    tft.fillRoundRect(ButtonX, ButtonY, ButtonW, ButtonH, 6, TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.drawString("Setup AP", ButtonX + ButtonW / 2, ButtonY + ButtonH / 2, 2);
  }
  apShown = ap;
}

void drawValue(int row, const String& value, uint16_t color = TFT_WHITE) {
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  // Rows beside the QR code stop short of it; rows below it use the full width.
  const int16_t y = RowY + row * RowH;
  tft.setTextPadding(apShown && y < 140 ? 132 : SCREEN_W - ValueX - 4);
  tft.drawString(value, ValueX, y, 2);
  tft.setTextPadding(0);
}

void drawLine(int16_t y, const String& value, uint16_t color) {
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextPadding(SCREEN_W - 16);
  tft.drawString(value, 8, y, 2);
  tft.setTextPadding(0);
}

void refresh() {
  const auto st = wifimgr::state();
  const bool connected = st == wifimgr::State::Connected;
  const bool busy = st == wifimgr::State::Connecting || st == wifimgr::State::Searching;
  const uint16_t stateColor = connected ? TFT_GREEN : busy ? TFT_YELLOW : TFT_ORANGE;
  String state = wifimgr::stateText();
  if (!connected && wifimgr::lastFailure().length()) state += ": " + wifimgr::lastFailure();
  drawValue(0, state, stateColor);
  const String paired = wifimgr::pairedRiot();
  drawValue(1, connected ? wifimgr::network() + (paired.length() ? " (RIOT pairing)" : "") : wifimgr::savedWifi().ssid,
            paired.length() ? TFT_CYAN : TFT_WHITE);
  drawValue(2, connected ? wifimgr::stationIp().toString() : String("--"));
  drawValue(3, connected ? String(wifimgr::rssi()) + " dBm" : String("--"));
  drawValue(4, wifimgr::unitName() + ".local");

  const auto& devs = profiles::devices();
  size_t ok = 0;
  for (const auto& d : devs) ok += d.status == profile::Status::Ok;
  String devices = String(ok) + " of " + String(devs.size()) + " loaded";
  if (!devs.empty() && devs[0].status == profile::Status::Ok)
    devices += ", unit " + String(devs[0].unitId) + " " + devs[0].profile->model.c_str();
  drawValue(5, devices, ok == devs.size() ? TFT_WHITE : TFT_ORANGE);

  const auto s = mbserver::stats();
  const uint32_t age = s.requests ? millis() - s.lastRequestMs : UINT32_MAX;
  String mb = "port " + String(unitcfg::current().modbus.port) + ", " + String(s.activeClients) + " client(s), " +
              String(s.requests) + " req";
  if (s.requests) mb += age < 5000 ? ", POLLED" : ", idle " + String(age / 1000) + " s";
  drawValue(6, mb, age < 5000 ? TFT_GREEN : TFT_LIGHTGREY);

  if (button::held()) {
    const uint32_t sec = button::heldMs() / 1000;
    drawLine(184, sec >= 10 ? "Release: network settings reset"
                  : sec >= 3 ? "Setup AP on. Hold to 10 s to reset network"
                             : "Setup AP in " + String(3 - sec) + " s, network reset at 10 s",
             sec >= 10 ? TFT_RED : TFT_YELLOW);
  } else if (apShown) {
    const uint32_t off = wifimgr::apOffInS();
    drawLine(184, "Join " + wifimgr::unitName() + ", open " + wifimgr::apIp().toString(), TFT_CYAN);
    drawLine(206, String(wifimgr::apClients()) + " phone(s) connected" + (off ? ", AP closes in " + String(off) + " s" : String()),
             TFT_LIGHTGREY);
  } else if (s.requests) {
    drawLine(184, "Last: unit " + String(s.lastUnit) + " FC" + String(s.lastFc) + " @" + String(s.lastAddress) +
                      (s.lastException ? " -> exception " + String(s.lastException) : String(" OK")) + ", exc " +
                      String(s.exceptions),
             s.lastException ? TFT_ORANGE : TFT_LIGHTGREY);
  } else {
    drawLine(184, connected ? "Modbus TCP " + wifimgr::stationIp().toString() + ":" + String(unitcfg::current().modbus.port)
                            : String("Hold BOOT 3 s for the setup AP"),
             TFT_LIGHTGREY);
  }
}

void ledPattern() {
  if (wifimgr::apActive()) led::setPattern(led::Pattern::BlueSolid);
  else if (wifimgr::state() == wifimgr::State::Connected) led::setPattern(led::Pattern::Off);
  else led::setPattern(led::Pattern::YellowSlowBlink);
}

// 30 ms flash per poll: green for a reply, red for an exception or a dropped reply.
void ledPolls() {
  static uint32_t lastReplies = 0, lastBad = 0;
  const auto s = mbserver::stats();
  const uint32_t bad = s.exceptions + s.dropped;
  if (bad != lastBad) led::flash(led::Color::Red, 30);
  else if (s.replies != lastReplies) led::flash(led::Color::Green, 30);
  lastReplies = s.replies;
  lastBad = bad;
}
}  // namespace

// The default 8 KB loop stack overflowed while parsing several profiles at boot
// (toml++ parses use a lot of stack). Heap is plentiful, so give the loop 16 KB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\nCYD-TESTER %s\n", FW_VERSION);
  led::begin();
  button::begin();

  const bool fsOk = LittleFS.begin(true);  // the data partition is ours alone
  Serial.printf("LittleFS %s total=%u used=%u\n", fsOk ? "mounted" : "FAILED", LittleFS.totalBytes(), LittleFS.usedBytes());
  String error;
  std::vector<String> warnings;
  if (!unitcfg::load(error, warnings)) Serial.printf("unit.toml: %s (using defaults)\n", error.c_str());
  for (const auto& w : warnings) Serial.printf("unit.toml: %s\n", w.c_str());
  profiles::installBuiltins();
  profiles::loadDevices();
  Serial.printf("Loop stack: %u bytes unused after loading profiles\n", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  runtime::begin();

  const auto& dc = unitcfg::current().display;
  display::begin(dc.rotation, dc.brightness);
  touch::begin();
  delay(100);
  const bool forceCal = touch::touchedNow();  // screen held during power-up
  if (forceCal || !touch::loadCalibration()) touch::runCalibration();

  wifimgr::begin();
  web::begin();
  const auto& mc = unitcfg::current().modbus;
  mbproc::Settings settings;
  settings.respondToAnyUnitId = mc.respondToAnyUnitId;
  settings.unmappedReadZero = mc.unmappedReadZero;
  settings.strictFunctionCodes = mc.strictFunctionCodes;
  settings.firmwareVersion = FW_VERSION;
  mbserver::begin(runtime::values(), settings, mc.port, mc.maxClients, mc.idleTimeoutS);
  Serial.printf("Modbus TCP server on port %u, %u device(s), max %u clients\n", mc.port, unsigned(runtime::values().devices.size()),
                mc.maxClients);
  drawLayout(wifimgr::apActive());
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  static bool wasPressed = false;
  button::service();
  wifimgr::service();
  runtime::service();
  api::service();
  ledPattern();
  ledPolls();
  led::service();

  if (button::crossed(3000)) { Serial.println("BOOT 3 s: setup AP requested"); wifimgr::requestPortal(); }
  if (button::crossed(10000)) { Serial.println("BOOT 10 s: network reset"); wifimgr::resetNetwork(); }

  // The Setup AP button needs a deliberate press: held inside the button for 500 ms,
  // so a brush of the screen or a stray touch reading cannot open the hotspot.
  static uint32_t buttonSince = 0;
  static bool buttonFired = false;
  const touch::Point p = touch::read();
  const bool onButton = p.pressed && !apShown && p.x >= ButtonX && p.x < ButtonX + ButtonW && p.y >= ButtonY &&
                        p.y < ButtonY + ButtonH;
  if (!onButton) {
    buttonSince = 0;
    buttonFired = false;
  } else if (!buttonSince) {
    buttonSince = millis();
  } else if (!buttonFired && millis() - buttonSince >= 500) {
    buttonFired = true;
    Serial.printf("Touch: Setup AP button held at x=%d y=%d; setup AP requested\n", p.x, p.y);
    led::flash(led::Color::Blue, 60);
    wifimgr::requestPortal();
  }
  wasPressed = p.pressed;

  if (wifimgr::apActive() != apShown) drawLayout(wifimgr::apActive());
  const uint32_t now = millis();
  if (now - lastDraw >= 250) { lastDraw = now; refresh(); }
  if (now - lastLog >= 10000) {
    lastLog = now;
    const auto s = mbserver::stats();
    Serial.printf("wifi=%s ip=%s ap=%d modbus req=%u exc=%u clients=%u heap=%u min=%u stack_free=%u\n", wifimgr::stateText(),
                  wifimgr::stationIp().toString().c_str(), wifimgr::apActive(), s.requests, s.exceptions, s.activeClients,
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
  delay(5);
}
