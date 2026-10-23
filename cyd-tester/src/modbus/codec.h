#pragma once
#include <cstdint>
#include "../profiles/profile.h"

// Engineering value <-> Modbus register words for one tag (PROMPT.md section 5.4).
// Engineering value = raw x scale. Integer types round and clamp; float32 sends
// raw = value / scale as an IEEE-754 float in the tag's byte order.
namespace codec {
uint16_t swapBytes(uint16_t w);
// Writes profile::wordCount(t.type) words. Not used for bool tags.
void encode(const profile::Tag& t, double value, uint16_t* words);
double decode(const profile::Tag& t, const uint16_t* words);
}  // namespace codec
