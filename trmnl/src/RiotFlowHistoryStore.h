#pragma once

#include <Arduino.h>
#include <FS.h>

namespace RiotFlowHistoryStore {

struct Entry {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint32_t hourKey;      // YYYYMMDDHH local time
  uint8_t run;           // 0-based
  uint8_t reserved[3];
  float volumeE3m3;
  float flowHours;
  float avgDP;
  float avgStatic;
  float avgTemp;
  uint32_t checksum;
};

struct Stats {
  uint16_t count;
  uint16_t capacity;
  uint32_t newestSequence;
  uint32_t rolloverCount;
};

static constexpr uint32_t ENTRY_MAGIC = 0x52464846UL; // RFHF
static constexpr uint16_t ENTRY_VERSION = 1;
static constexpr uint16_t MAX_RECORDS = 6000;
static constexpr const char* DEFAULT_PATH = "/flow_history_hour.bin";

void prepare(Entry& entry);
bool valid(const Entry& entry);
bool stats(fs::FS& fs, Stats& out, const char* path = DEFAULT_PATH);
uint16_t count(fs::FS& fs, const char* path = DEFAULT_PATH);
bool append(fs::FS& fs, Entry& entry, const char* path = DEFAULT_PATH);
bool readNewest(fs::FS& fs, uint16_t newestOffset, Entry& entry, const char* path = DEFAULT_PATH);
bool clear(fs::FS& fs, const char* path = DEFAULT_PATH);

} // namespace RiotFlowHistoryStore
