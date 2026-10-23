#include "button.h"

namespace button {
namespace {
constexpr uint32_t DebounceMs = 30;
bool stable = false, lastRaw = false;
uint32_t changedAt = 0, pressedAt = 0, lastHeld = 0, prevHeld = 0;
}  // namespace

void begin() { pinMode(PIN_BOOT, INPUT_PULLUP); }

void service() {
  const uint32_t now = millis();
  const bool raw = digitalRead(PIN_BOOT) == LOW;
  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (raw != stable && now - changedAt >= DebounceMs) {
    stable = raw;
    if (stable) pressedAt = now;
  }
  prevHeld = lastHeld;
  lastHeld = stable ? now - pressedAt : 0;
}

bool held() { return stable; }
uint32_t heldMs() { return lastHeld; }
bool crossed(uint32_t ms) { return lastHeld >= ms && prevHeld < ms; }
}  // namespace button
