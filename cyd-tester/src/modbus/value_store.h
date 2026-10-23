#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "../profiles/profile.h"
#include "lock.h"

// Live values for every emulated device. Each tag keeps its engineering value
// and its pre-encoded register words; the Modbus handler copies words under the
// store lock, so a float is never served half old and half new.
namespace store {
struct TagValue {
  double value = 0;
  uint16_t words[2] = {0, 0};  // register words, or words[0] = 0/1 for bit tags
  bool overridden = false;     // written by a Modbus client or the web UI: the simulation leaves it alone
  uint32_t readCount = 0, lastReadMs = 0, lastWriteMs = 0, writeCount = 0;
  uint32_t pollIntervalMs = 0; // smoothed time between client reads (0 until read twice)
};

// Where a wire address lands: tag index and which of its words.
struct Slot {
  uint16_t wire;
  uint16_t tag;
  uint8_t word;
};

class Device {
 public:
  Device(uint8_t unitId, bool enabled, std::shared_ptr<const profile::Profile> p);
  uint8_t unitId;
  bool enabled;
  std::shared_ptr<const profile::Profile> profile;
  std::vector<TagValue> values;  // parallel to profile->tags

  const Slot* find(profile::Space s, uint16_t wire) const;
  // Caller holds the store lock.
  void setValue(size_t tag, double engineering);
  void setWords(size_t tag, const uint16_t* words);  // from a Modbus write: words as sent
  void override(size_t tag, double engineering);    // from the web UI: holds until resumed
  void noteRead(size_t tag, uint32_t nowMs);

 private:
  std::vector<Slot> slots_[4];  // per Space, sorted by wire
};

class Store {
 public:
  mb::Lock lock;
  std::vector<Device> devices;
  // Enabled device with this unit ID, or nullptr.
  Device* find(uint8_t unitId);
  Device* firstEnabled();
};

// Millisecond clock (millis() on the device; replaceable in host tests).
extern uint32_t (*nowMs)();
}  // namespace store
