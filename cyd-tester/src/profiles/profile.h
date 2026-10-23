#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "profile_types.h"

// RIOT Device Library profile (PROMPT.md section 5), parsed from unmodified
// TOML into compact structs. Comments and descriptions are not retained.
namespace profile {
struct Tag {
  std::string id, name, units, pollGroup, poolGroup;
  uint32_t reg = 0;      // register number as written in the profile
  uint16_t wire = 0;     // on-wire address in `space`
  uint8_t readFc = 0;    // 1-4; 0 when unmapped
  uint8_t writeFc = 0;   // 5, 6, 15 or 16; 0 = not declared
  Space space = Space::HoldingRegister;
  DataType type = DataType::UInt16;
  ByteOrder order = ByteOrder::ABCD;  // float32 only
  bool swapBytes = false;             // 16-bit types: short_packing = "BA"
  float scale = 1.0f;
  bool writable = false;              // access = "read_write" on a writable space
  bool defaultEnabled = false;
  bool mapped = true;                 // false: shown in the UI but not served
};

struct Profile {
  int schemaVersion = 0;
  std::string profileId, vendor, model;
  int profileVersion = 0;
  uint8_t unitId = 0;                 // 0 = not stated
  AddressMode addressMode = AddressMode::Unstated;
  std::vector<Tag> tags;
  std::vector<std::string> warnings;

  size_t mappedCount() const;
  const Tag* find(std::string_view id) const;
};

enum class Status : uint8_t { Ok, NotModbus, Error };
struct ParseResult {
  Status status = Status::Error;
  std::string message;  // the reason for NotModbus or Error
};

ParseResult parse(std::string_view tomlText, Profile& out);
}  // namespace profile
