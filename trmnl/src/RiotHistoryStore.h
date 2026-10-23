#pragma once

#include <Arduino.h>
#include <FS.h>

namespace RiotHistoryStore {

enum EntryType : uint8_t {
  ENTRY_ALARM = 0,
  ENTRY_EVENT = 1,
  ENTRY_CLEAR = 2,
  ENTRY_TEST = 3,
  ENTRY_ACK = 4
};

struct Entry {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint32_t epoch;
  uint8_t type;
  uint8_t sourceIndex;
  uint16_t reserved;
  float value;
  float threshold;
  char label[25];
  char message[49];
  uint32_t checksum;
};

struct Stats {
  uint16_t count;
  uint16_t capacity;
  uint32_t oldestSequence;
  uint32_t newestSequence;
  uint32_t rolloverCount;
};

static constexpr uint32_t ENTRY_MAGIC = 0x52464832UL; // RFH2
static constexpr uint16_t ENTRY_VERSION = 2;
// RIOT A8B PROTECTED AUDIT LOG V1
static constexpr uint16_t LEGACY_MAX_RECORDS = 512;
static constexpr uint16_t MAX_RECORDS = 2048;
static constexpr const char* DEFAULT_PATH = "/riot_history_v2.bin";

void prepare(Entry& entry);
bool valid(const Entry& entry);
uint16_t count(fs::FS& fs, const char* path = DEFAULT_PATH);
bool stats(fs::FS& fs, Stats& out, const char* path = DEFAULT_PATH);
bool append(fs::FS& fs, Entry& entry, const char* path = DEFAULT_PATH);
bool readNewest(fs::FS& fs, uint16_t newestOffset, Entry& entry, const char* path = DEFAULT_PATH);
bool clear(fs::FS& fs, const char* path = DEFAULT_PATH);

} // namespace RiotHistoryStore
