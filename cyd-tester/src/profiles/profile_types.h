#pragma once
#include <cstdint>

// Shared enums for profiles, address conversion and the value codec.
// Plain C++17 (no Arduino) so the same code runs in host unit tests.
namespace profile {
enum class DataType : uint8_t { Bool, UInt16, Int16, Float32 };
// Byte order of a 32-bit value across two registers, as A B C D = big-endian bytes.
enum class ByteOrder : uint8_t { ABCD, CDAB, BADC, DCBA };
// The four independent Modbus address spaces.
enum class Space : uint8_t { Coil, DiscreteInput, InputRegister, HoldingRegister };
enum class AddressMode : uint8_t { Reference, OneBased, ZeroBased, Unstated };

inline uint8_t wordCount(DataType t) { return t == DataType::Float32 ? 2 : 1; }
inline bool isBit(Space s) { return s == Space::Coil || s == Space::DiscreteInput; }
// Space read by a function code 1-4; false for anything else.
inline bool spaceForReadFc(uint8_t fc, Space& out) {
  switch (fc) {
    case 1: out = Space::Coil; return true;
    case 2: out = Space::DiscreteInput; return true;
    case 3: out = Space::HoldingRegister; return true;
    case 4: out = Space::InputRegister; return true;
    default: return false;
  }
}
const char* toString(DataType t);
const char* toString(ByteOrder o);
const char* toString(Space s);
const char* toString(AddressMode m);
}  // namespace profile
