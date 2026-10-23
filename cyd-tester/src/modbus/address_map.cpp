#include "address_map.h"

namespace addr {
namespace {
using profile::Space;

struct Range {
  uint32_t lo, hi, base;
  Space space;
};
// 6-digit references first, then the classic 5-digit ranges.
constexpr Range References[] = {
    {400001, 465536, 400001, Space::HoldingRegister},
    {300001, 365536, 300001, Space::InputRegister},
    {100001, 165536, 100001, Space::DiscreteInput},
    {40001, 49999, 40001, Space::HoldingRegister},
    {30001, 39999, 30001, Space::InputRegister},
    {10001, 19999, 10001, Space::DiscreteInput},
    {1, 9999, 1, Space::Coil},
};

const char* refName(Space s) {
  switch (s) {
    case Space::Coil: return "0xxxx (coil)";
    case Space::DiscreteInput: return "1xxxx (discrete input)";
    case Space::InputRegister: return "3xxxx (input register)";
    case Space::HoldingRegister: return "4xxxx (holding register)";
  }
  return "?";
}
}  // namespace

Conversion toWire(uint32_t reg, uint8_t readFc, profile::AddressMode mode) {
  Conversion c;
  uint32_t wire = 0;
  switch (mode) {
    case profile::AddressMode::OneBased:
      if (reg < 1) { c.error = "register 0 is invalid with one_based addressing"; return c; }
      wire = reg - 1;
      break;
    case profile::AddressMode::ZeroBased:
    case profile::AddressMode::Unstated:
      wire = reg;
      break;
    case profile::AddressMode::Reference: {
      const Range* match = nullptr;
      for (const auto& r : References)
        if (reg >= r.lo && reg <= r.hi) { match = &r; break; }
      if (!match) { c.error = "register " + std::to_string(reg) + " is not a Modbus reference number"; return c; }
      wire = reg - match->base;
      Space expected;
      if (profile::spaceForReadFc(readFc, expected) && expected != match->space) {
        c.warning = "reference " + std::to_string(reg) + " is a " + refName(match->space) + " number but the tag reads with FC" +
                    std::to_string(readFc) + "; serving it at wire " + std::to_string(wire) + " in the FC" +
                    std::to_string(readFc) + " space";
      }
      break;
    }
  }
  if (wire > 65535) { c.error = "register " + std::to_string(reg) + " is beyond the 16-bit Modbus address range"; return c; }
  c.ok = true;
  c.wire = uint16_t(wire);
  return c;
}
}  // namespace addr
