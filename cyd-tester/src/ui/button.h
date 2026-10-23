#pragma once
#include <Arduino.h>

// BOOT button (GPIO 0, active LOW). Debounced; reports how long it has been held
// and fires each hold threshold once per press.
namespace button {
void begin();
void service();
bool held();
uint32_t heldMs();
// True exactly once per press when the hold passes `ms`.
bool crossed(uint32_t ms);
}  // namespace button
