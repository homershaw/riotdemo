#include "codec.h"
#include <cmath>
#include <cstring>

namespace codec {
using profile::ByteOrder;
using profile::DataType;

uint16_t swapBytes(uint16_t w) { return uint16_t((w << 8) | (w >> 8)); }

namespace {
double rawFor(const profile::Tag& t, double value) {
  const double scale = t.scale != 0.0f ? double(t.scale) : 1.0;
  return value / scale;
}

long clampRound(double raw, long lo, long hi) {
  if (std::isnan(raw)) return 0;
  if (raw <= lo) return lo;
  if (raw >= hi) return hi;
  return std::lround(raw);
}

// Big-endian bytes A B C D of a 32-bit pattern, arranged into two words.
void put32(uint32_t bits, ByteOrder order, uint16_t* w) {
  const uint8_t a = bits >> 24, b = bits >> 16, c = bits >> 8, d = bits;
  switch (order) {
    case ByteOrder::ABCD: w[0] = uint16_t(a << 8 | b); w[1] = uint16_t(c << 8 | d); break;
    case ByteOrder::CDAB: w[0] = uint16_t(c << 8 | d); w[1] = uint16_t(a << 8 | b); break;
    case ByteOrder::BADC: w[0] = uint16_t(b << 8 | a); w[1] = uint16_t(d << 8 | c); break;
    case ByteOrder::DCBA: w[0] = uint16_t(d << 8 | c); w[1] = uint16_t(b << 8 | a); break;
  }
}

uint32_t get32(const uint16_t* w, ByteOrder order) {
  uint8_t a = 0, b = 0, c = 0, d = 0;
  switch (order) {
    case ByteOrder::ABCD: a = w[0] >> 8; b = w[0]; c = w[1] >> 8; d = w[1]; break;
    case ByteOrder::CDAB: c = w[0] >> 8; d = w[0]; a = w[1] >> 8; b = w[1]; break;
    case ByteOrder::BADC: b = w[0] >> 8; a = w[0]; d = w[1] >> 8; c = w[1]; break;
    case ByteOrder::DCBA: d = w[0] >> 8; c = w[0]; b = w[1] >> 8; a = w[1]; break;
  }
  return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | d;
}
}  // namespace

void encode(const profile::Tag& t, double value, uint16_t* words) {
  const double raw = rawFor(t, value);
  switch (t.type) {
    case DataType::UInt16: {
      const uint16_t w = uint16_t(clampRound(raw, 0, 65535));
      words[0] = t.swapBytes ? swapBytes(w) : w;
      break;
    }
    case DataType::Int16: {
      const uint16_t w = uint16_t(int16_t(clampRound(raw, -32768, 32767)));
      words[0] = t.swapBytes ? swapBytes(w) : w;
      break;
    }
    case DataType::Float32: {
      const float f = float(raw);
      uint32_t bits;
      std::memcpy(&bits, &f, sizeof bits);
      put32(bits, t.order, words);
      break;
    }
    case DataType::Bool:
      words[0] = value != 0.0 ? 1 : 0;
      break;
  }
}

double decode(const profile::Tag& t, const uint16_t* words) {
  const double scale = t.scale != 0.0f ? double(t.scale) : 1.0;
  switch (t.type) {
    case DataType::UInt16: return double(t.swapBytes ? swapBytes(words[0]) : words[0]) * scale;
    case DataType::Int16: return double(int16_t(t.swapBytes ? swapBytes(words[0]) : words[0])) * scale;
    case DataType::Float32: {
      const uint32_t bits = get32(words, t.order);
      float f;
      std::memcpy(&f, &bits, sizeof f);
      return double(f) * scale;
    }
    case DataType::Bool: return words[0] ? 1.0 : 0.0;
  }
  return 0.0;
}
}  // namespace codec
