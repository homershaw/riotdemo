#include "RiotHistoryStore.h"

#include <stddef.h>
#include <string.h>

namespace RiotHistoryStore {

namespace {

static constexpr uint32_t META_MAGIC = 0x52464D32UL; // RFM2
static constexpr uint16_t META_VERSION = 1;

struct Meta {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint16_t count;
  uint16_t nextIndex;
  uint32_t nextSequence;
  uint32_t rolloverCount;
  uint32_t checksum;
};

static uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

static uint32_t metaChecksum(const Meta& meta) {
  return fnv1a(
    reinterpret_cast<const uint8_t*>(&meta),
    offsetof(Meta, checksum)
  );
}

static String metaPathFor(const char* path) {
  return String(path) + ".meta";
}

static bool metaValid(const Meta& meta) {
  if (meta.magic != META_MAGIC) return false;
  if (meta.version != META_VERSION) return false;
  if (meta.size != sizeof(Meta)) return false;
  if (meta.count > MAX_RECORDS) return false;
  if (meta.nextIndex >= MAX_RECORDS && meta.count == MAX_RECORDS) return false;
  if (meta.count < MAX_RECORDS && meta.nextIndex != meta.count) return false;
  if (meta.nextSequence == 0) return false;
  return meta.checksum == metaChecksum(meta);
}

static bool saveMeta(fs::FS& fs, Meta& meta, const char* path) {
  meta.magic = META_MAGIC;
  meta.version = META_VERSION;
  meta.size = sizeof(Meta);
  meta.checksum = metaChecksum(meta);

  const String target = metaPathFor(path);
  const String temp = target + ".tmp";
  if (fs.exists(temp)) fs.remove(temp);

  File out = fs.open(temp, FILE_WRITE);
  if (!out) return false;
  const size_t written = out.write(
    reinterpret_cast<const uint8_t*>(&meta),
    sizeof(Meta)
  );
  out.flush();
  out.close();
  if (written != sizeof(Meta)) {
    fs.remove(temp);
    return false;
  }

  if (fs.exists(target)) fs.remove(target);
  if (!fs.rename(temp, target)) {
    fs.remove(temp);
    return false;
  }
  return true;
}

static bool readMeta(fs::FS& fs, Meta& meta, const char* path) {
  const String mp = metaPathFor(path);
  File in = fs.open(mp, FILE_READ);
  if (!in || in.size() != sizeof(Meta)) {
    if (in) in.close();
    return false;
  }
  const size_t got = in.read(
    reinterpret_cast<uint8_t*>(&meta),
    sizeof(Meta)
  );
  in.close();
  return got == sizeof(Meta) && metaValid(meta);
}

static uint32_t entryChecksum(const Entry& entry) {
  return fnv1a(
    reinterpret_cast<const uint8_t*>(&entry),
    offsetof(Entry, checksum)
  );
}

static bool readPhysical(File& in, uint16_t index, Entry& entry) {
  const size_t position = static_cast<size_t>(index) * sizeof(Entry);
  if (!in.seek(position, SeekSet)) return false;
  const size_t got = in.read(
    reinterpret_cast<uint8_t*>(&entry),
    sizeof(Entry)
  );
  return got == sizeof(Entry) && valid(entry);
}

// A8b compatibility migration for a legacy 512-record ring that had already
// reached capacity. Non-full legacy stores are already physically sequential
// and need no rewrite when capacity expands to 2048.
static bool migrateLegacy512RingIfNeeded(fs::FS& fs, const char* path) {
  const String mp = metaPathFor(path);
  File metaIn = fs.open(mp, FILE_READ);
  if (!metaIn || metaIn.size() != sizeof(Meta)) {
    if (metaIn) metaIn.close();
    return true;
  }

  Meta legacy = {};
  const size_t metaBytes = metaIn.read(
    reinterpret_cast<uint8_t*>(&legacy),
    sizeof(Meta)
  );
  metaIn.close();

  if (metaBytes != sizeof(Meta)) return true;
  if (legacy.magic != META_MAGIC ||
      legacy.version != META_VERSION ||
      legacy.size != sizeof(Meta) ||
      legacy.checksum != metaChecksum(legacy)) {
    return true;
  }

  // An expanded store at exactly 512 records uses nextIndex=512. A legacy
  // full ring can only have nextIndex 0..511.
  if (legacy.count != LEGACY_MAX_RECORDS ||
      legacy.nextIndex >= LEGACY_MAX_RECORDS ||
      legacy.nextSequence <= LEGACY_MAX_RECORDS) {
    return true;
  }

  File in = fs.open(path, FILE_READ);
  if (!in ||
      in.size() < static_cast<size_t>(LEGACY_MAX_RECORDS) * sizeof(Entry)) {
    if (in) in.close();
    return false;
  }

  const String temp = String(path) + ".expand2048.tmp";
  const String backup = String(path) + ".legacy512.bak";
  if (fs.exists(temp)) fs.remove(temp);
  if (fs.exists(backup)) fs.remove(backup);

  File out = fs.open(temp, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }

  const uint32_t newestSequence = legacy.nextSequence - 1U;
  const uint32_t oldestSequence =
    newestSequence - static_cast<uint32_t>(LEGACY_MAX_RECORDS) + 1U;

  bool ok = true;
  for (uint32_t seq = oldestSequence; seq <= newestSequence; ++seq) {
    const uint16_t physicalIndex =
      static_cast<uint16_t>((seq - 1U) % LEGACY_MAX_RECORDS);
    Entry entry = {};
    if (!readPhysical(in, physicalIndex, entry) || entry.sequence != seq) {
      ok = false;
      break;
    }
    if (out.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(Entry))
        != sizeof(Entry)) {
      ok = false;
      break;
    }
  }
  out.flush();
  out.close();
  in.close();

  if (!ok) {
    fs.remove(temp);
    return false;
  }

  if (!fs.rename(path, backup)) {
    fs.remove(temp);
    return false;
  }
  if (!fs.rename(temp, path)) {
    fs.rename(backup, path);
    return false;
  }

  legacy.nextIndex = LEGACY_MAX_RECORDS;
  legacy.checksum = metaChecksum(legacy);
  if (!saveMeta(fs, legacy, path)) {
    fs.remove(path);
    fs.rename(backup, path);
    return false;
  }

  fs.remove(backup);
  return true;
}

static bool rebuildMeta(fs::FS& fs, Meta& meta, const char* path) {
  memset(&meta, 0, sizeof(meta));
  meta.magic = META_MAGIC;
  meta.version = META_VERSION;
  meta.size = sizeof(Meta);
  meta.nextSequence = 1;

  File in = fs.open(path, FILE_READ);
  if (!in) {
    meta.nextIndex = 0;
    meta.checksum = metaChecksum(meta);
    return true;
  }

  const size_t physicalRecords = min(
    static_cast<size_t>(MAX_RECORDS),
    in.size() / sizeof(Entry)
  );

  uint32_t newestSequence = 0;
  uint16_t validCount = 0;
  for (uint16_t i = 0; i < physicalRecords; ++i) {
    Entry entry = {};
    if (!readPhysical(in, i, entry)) continue;
    validCount++;
    if (entry.sequence > newestSequence) newestSequence = entry.sequence;
  }
  in.close();

  meta.count = validCount;
  meta.nextSequence = newestSequence ? newestSequence + 1U : 1U;
  if (meta.count < MAX_RECORDS) {
    meta.nextIndex = meta.count;
    meta.rolloverCount = 0;
  } else {
    meta.nextIndex = newestSequence
      ? static_cast<uint16_t>(newestSequence % MAX_RECORDS)
      : 0;
    meta.rolloverCount = newestSequence > meta.count
      ? newestSequence - meta.count
      : 0;
  }
  meta.checksum = metaChecksum(meta);
  saveMeta(fs, meta, path);
  return true;
}

static bool loadMeta(fs::FS& fs, Meta& meta, const char* path) {
  if (!migrateLegacy512RingIfNeeded(fs, path)) return false;
  if (readMeta(fs, meta, path)) return true;
  return rebuildMeta(fs, meta, path);
}

} // namespace

void prepare(Entry& entry) {
  entry.magic = ENTRY_MAGIC;
  entry.version = ENTRY_VERSION;
  entry.size = sizeof(Entry);
  entry.reserved = 0;
  entry.label[sizeof(entry.label) - 1] = 0;
  entry.message[sizeof(entry.message) - 1] = 0;
  entry.checksum = entryChecksum(entry);
}

bool valid(const Entry& entry) {
  if (entry.magic != ENTRY_MAGIC) return false;
  if (entry.version != ENTRY_VERSION) return false;
  if (entry.size != sizeof(Entry)) return false;
  return entry.checksum == entryChecksum(entry);
}

bool stats(fs::FS& fs, Stats& out, const char* path) {
  Meta meta = {};
  if (!loadMeta(fs, meta, path)) return false;
  out.count = meta.count;
  out.capacity = MAX_RECORDS;
  out.newestSequence = meta.nextSequence > 1 ? meta.nextSequence - 1U : 0U;
  out.oldestSequence =
    meta.count && out.newestSequence >= meta.count
      ? out.newestSequence - meta.count + 1U
      : 0U;
  out.rolloverCount = meta.rolloverCount;
  return true;
}

uint16_t count(fs::FS& fs, const char* path) {
  Stats s = {};
  return stats(fs, s, path) ? s.count : 0;
}

bool append(fs::FS& fs, Entry& entry, const char* path) {
  Meta meta = {};
  if (!loadMeta(fs, meta, path)) return false;

  entry.sequence = meta.nextSequence;
  prepare(entry);

  bool writtenOK = false;
  if (meta.count < MAX_RECORDS) {
    File out = fs.open(path, FILE_APPEND);
    if (!out) return false;
    const size_t written = out.write(
      reinterpret_cast<const uint8_t*>(&entry),
      sizeof(Entry)
    );
    out.flush();
    out.close();
    writtenOK = written == sizeof(Entry);
  } else {
    File out = fs.open(path, "r+");
    if (!out) return false;
    const size_t position = static_cast<size_t>(meta.nextIndex) * sizeof(Entry);
    if (out.seek(position, SeekSet)) {
      const size_t written = out.write(
        reinterpret_cast<const uint8_t*>(&entry),
        sizeof(Entry)
      );
      out.flush();
      writtenOK = written == sizeof(Entry);
    }
    out.close();
  }
  if (!writtenOK) return false;

  meta.nextSequence++;
  if (meta.count < MAX_RECORDS) {
    meta.count++;
    meta.nextIndex = meta.count < MAX_RECORDS ? meta.count : 0;
  } else {
    meta.nextIndex = static_cast<uint16_t>((meta.nextIndex + 1U) % MAX_RECORDS);
    meta.rolloverCount++;
  }

  // Record data is committed first. If metadata write is interrupted, the
  // next boot/read safely rebuilds metadata by scanning validated records.
  return saveMeta(fs, meta, path);
}

bool readNewest(fs::FS& fs, uint16_t newestOffset, Entry& entry, const char* path) {
  Meta meta = {};
  if (!loadMeta(fs, meta, path)) return false;
  if (meta.count == 0 || newestOffset >= meta.count) return false;

  const uint32_t targetSequence =
    (meta.nextSequence - 1U) - static_cast<uint32_t>(newestOffset);

  uint16_t physicalIndex = 0;
  if (meta.count < MAX_RECORDS) {
    // Before the expanded ring fills, records are stored oldest->newest.
    physicalIndex =
      static_cast<uint16_t>(meta.count - 1U - newestOffset);
  } else {
    // At capacity nextIndex always points to the oldest slot / next write.
    physicalIndex = static_cast<uint16_t>(
      (static_cast<uint32_t>(meta.nextIndex) + MAX_RECORDS - 1U -
       newestOffset) % MAX_RECORDS
    );
  }

  File in = fs.open(path, FILE_READ);
  if (!in) return false;
  const bool ok = readPhysical(in, physicalIndex, entry);
  in.close();
  return ok && entry.sequence == targetSequence;
}

bool clear(fs::FS& fs, const char* path) {
  bool ok = true;
  if (fs.exists(path)) ok = fs.remove(path) && ok;

  const String mp = metaPathFor(path);
  const String temp = mp + ".tmp";
  if (fs.exists(mp)) ok = fs.remove(mp) && ok;
  if (fs.exists(temp)) ok = fs.remove(temp) && ok;
  return ok;
}

} // namespace RiotHistoryStore
