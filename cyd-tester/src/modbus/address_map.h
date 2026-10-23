#pragma once
#include <cstdint>
#include <string>
#include "../profiles/profile_types.h"

// Profile register number -> on-wire Modbus address (PROMPT.md section 5.3).
namespace addr {
struct Conversion {
  bool ok = false;
  uint16_t wire = 0;
  std::string warning;  // set when the tag loads but something looks wrong
  std::string error;    // set when !ok
};
Conversion toWire(uint32_t reg, uint8_t readFc, profile::AddressMode mode);
}  // namespace addr
