#pragma once
#include <cstddef>
#include <cstdint>
#include "value_store.h"

// Modbus request processing, independent of the TCP transport (PROMPT.md section 8).
namespace mbproc {
enum Exception : uint8_t {
  None = 0,
  IllegalFunction = 0x01,
  IllegalDataAddress = 0x02,
  IllegalDataValue = 0x03,
};

struct Settings {
  bool respondToAnyUnitId = false;  // answer every unit ID with the first enabled device
  bool unmappedReadZero = false;    // false: unmapped reads return exception 02
  bool strictFunctionCodes = true;  // writes must use the tag's declared write_function
  const char* firmwareVersion = "";
};

struct Result {
  bool reply = true;       // false: send nothing (broadcast, unknown unit ID)
  size_t length = 0;       // response PDU length in `out`
  uint8_t exception = 0;   // exception code sent, 0 if none
  uint8_t unitId = 0;      // unit that answered (0 if none)
  int16_t firstTag = -1;   // first tag touched, for the traffic log
  uint16_t tagCount = 0;
};

constexpr size_t MaxPdu = 253;
// `pdu` starts at the function code. `out` must hold MaxPdu bytes.
Result handle(store::Store& s, const Settings& cfg, uint8_t unitId, const uint8_t* pdu, size_t pduLen, uint8_t* out);
}  // namespace mbproc
