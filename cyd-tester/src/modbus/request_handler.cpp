#include "request_handler.h"
#include <cstring>

namespace mbproc {
namespace {
using profile::DataType;
using profile::Space;
using store::Device;

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

Result exception(uint8_t fc, uint8_t code, uint8_t* out, Result r = {}) {
  out[0] = uint8_t(fc | 0x80);
  out[1] = code;
  r.length = 2;
  r.exception = code;
  return r;
}

void touch(Result& r, uint16_t tag) {
  if (r.firstTag < 0) r.firstTag = int16_t(tag);
  ++r.tagCount;
}

// FC1/2 (bits) and FC3/4 (registers).
Result read(Device& d, store::Store& s, const Settings& cfg, uint8_t fc, const uint8_t* pdu, size_t len, uint8_t* out) {
  if (len != 5) return exception(fc, IllegalDataValue, out);
  const uint16_t start = be16(pdu + 1), qty = be16(pdu + 3);
  const bool bits = fc == 1 || fc == 2;
  const Space space = fc == 1 ? Space::Coil : fc == 2 ? Space::DiscreteInput : fc == 3 ? Space::HoldingRegister : Space::InputRegister;
  if (qty < 1 || qty > (bits ? 2000 : 125)) return exception(fc, IllegalDataValue, out);
  if (uint32_t(start) + qty > 65536) return exception(fc, IllegalDataAddress, out);

  Result r;
  const size_t bytes = bits ? (qty + 7) / 8 : qty * 2u;
  out[0] = fc;
  out[1] = uint8_t(bytes);
  std::memset(out + 2, 0, bytes);
  const uint32_t now = store::nowMs();
  int16_t lastCounted = -1;
  mb::Guard g(s.lock);  // one snapshot for the whole request: floats stay consistent
  for (uint16_t i = 0; i < qty; ++i) {
    const store::Slot* slot = d.find(space, uint16_t(start + i));
    if (!slot) {
      if (!cfg.unmappedReadZero) return exception(fc, IllegalDataAddress, out);
      continue;  // stays zero
    }
    auto& tv = d.values[slot->tag];
    if (bits) {
      if (tv.words[0]) out[2 + i / 8] |= uint8_t(1u << (i % 8));
    } else {
      put16(out + 2 + i * 2, tv.words[slot->word]);
    }
    if (lastCounted != int16_t(slot->tag)) {  // count each tag once per request
      lastCounted = int16_t(slot->tag);
      d.noteRead(slot->tag, now);
      touch(r, slot->tag);
    }
  }
  r.length = 2 + bytes;
  return r;
}

// Checks that a write may go to this tag with this function code.
uint8_t writeAllowed(const profile::Tag& t, uint8_t fc, const Settings& cfg) {
  if (!t.writable) return IllegalDataAddress;
  // A tag with no declared write_function accepts any suitable FC; a declared one must match when strict.
  if (cfg.strictFunctionCodes && t.writeFc && t.writeFc != fc) return IllegalFunction;
  return None;
}

Result writeCoils(Device& d, store::Store& s, const Settings& cfg, uint8_t fc, const uint8_t* pdu, size_t len, uint8_t* out) {
  uint16_t start, qty;
  const uint8_t* data;
  uint8_t single[1];
  if (fc == 5) {
    if (len != 5) return exception(fc, IllegalDataValue, out);
    start = be16(pdu + 1);
    const uint16_t v = be16(pdu + 3);
    if (v != 0xFF00 && v != 0x0000) return exception(fc, IllegalDataValue, out);
    qty = 1;
    single[0] = v == 0xFF00 ? 1 : 0;
    data = single;
  } else {
    if (len < 6) return exception(fc, IllegalDataValue, out);
    start = be16(pdu + 1);
    qty = be16(pdu + 3);
    if (qty < 1 || qty > 1968 || pdu[5] != (qty + 7) / 8 || len != 6u + pdu[5]) return exception(fc, IllegalDataValue, out);
    if (uint32_t(start) + qty > 65536) return exception(fc, IllegalDataAddress, out);
    data = pdu + 6;
  }
  Result r;
  mb::Guard g(s.lock);
  for (uint16_t i = 0; i < qty; ++i) {  // validate everything before changing anything
    const store::Slot* slot = d.find(Space::Coil, uint16_t(start + i));
    if (!slot) return exception(fc, IllegalDataAddress, out);
    if (const uint8_t e = writeAllowed(d.profile->tags[slot->tag], fc, cfg)) return exception(fc, e, out);
  }
  for (uint16_t i = 0; i < qty; ++i) {
    const store::Slot* slot = d.find(Space::Coil, uint16_t(start + i));
    const uint16_t bit = (data[i / 8] >> (i % 8)) & 1;
    d.setWords(slot->tag, &bit);
    touch(r, slot->tag);
  }
  std::memcpy(out, pdu, 5);  // FC5 echoes the request; FC15 echoes address and quantity
  r.length = 5;
  return r;
}

Result writeRegisters(Device& d, store::Store& s, const Settings& cfg, uint8_t fc, const uint8_t* pdu, size_t len, uint8_t* out) {
  uint16_t start, qty;
  const uint8_t* data;
  if (fc == 6) {
    if (len != 5) return exception(fc, IllegalDataValue, out);
    start = be16(pdu + 1);
    qty = 1;
    data = pdu + 3;
  } else {
    if (len < 6) return exception(fc, IllegalDataValue, out);
    start = be16(pdu + 1);
    qty = be16(pdu + 3);
    if (qty < 1 || qty > 123 || pdu[5] != qty * 2 || len != 6u + pdu[5]) return exception(fc, IllegalDataValue, out);
    if (uint32_t(start) + qty > 65536) return exception(fc, IllegalDataAddress, out);
    data = pdu + 6;
  }
  Result r;
  mb::Guard g(s.lock);
  // Validate: every register mapped and writable, and every float covered by both of its registers.
  for (uint16_t i = 0; i < qty; ++i) {
    const store::Slot* slot = d.find(Space::HoldingRegister, uint16_t(start + i));
    if (!slot) return exception(fc, IllegalDataAddress, out);
    const auto& t = d.profile->tags[slot->tag];
    if (const uint8_t e = writeAllowed(t, fc, cfg)) return exception(fc, e, out);
    if (t.type == DataType::Float32) {
      const uint32_t first = uint32_t(start + i) - slot->word;  // wire of the float's first register
      if (first < start || first + 1 >= uint32_t(start) + qty) return exception(fc, IllegalDataAddress, out);
    }
  }
  for (uint16_t i = 0; i < qty;) {
    const store::Slot* slot = d.find(Space::HoldingRegister, uint16_t(start + i));
    const uint8_t n = profile::wordCount(d.profile->tags[slot->tag].type);
    uint16_t words[2] = {be16(data + i * 2), n == 2 ? be16(data + i * 2 + 2) : uint16_t(0)};
    d.setWords(slot->tag, words);
    touch(r, slot->tag);
    i = uint16_t(i + n);
  }
  std::memcpy(out, pdu, 5);  // FC6 echoes the request; FC16 echoes address and quantity
  r.length = 5;
  return r;
}

Result deviceId(Device& d, const Settings& cfg, const uint8_t* pdu, size_t len, uint8_t* out) {
  if (len != 4 || pdu[1] != 0x0E) return exception(0x2B, IllegalFunction, out);
  const uint8_t code = pdu[2], object = pdu[3];
  if (code < 1 || code > 4) return exception(0x2B, IllegalDataValue, out);
  const std::string* values[3] = {&d.profile->vendor, &d.profile->model, nullptr};
  const std::string fw(cfg.firmwareVersion);
  values[2] = &fw;
  size_t n = 0;
  out[n++] = 0x2B;
  out[n++] = 0x0E;
  out[n++] = code;
  out[n++] = 0x81;  // conformity: basic identification, stream and individual access
  out[n++] = 0x00;  // more follows
  out[n++] = 0x00;  // next object id
  const size_t countAt = n++;
  uint8_t first = 0, last = 2;
  if (code == 4) {
    if (object > 2) return exception(0x2B, IllegalDataAddress, out);
    first = last = object;
  }
  uint8_t count = 0;
  for (uint8_t id = first; id <= last; ++id) {
    const std::string& v = *values[id];
    const size_t vlen = v.size() > 64 ? 64 : v.size();
    out[n++] = id;
    out[n++] = uint8_t(vlen);
    std::memcpy(out + n, v.data(), vlen);
    n += vlen;
    ++count;
  }
  out[countAt] = count;
  Result r;
  r.length = n;
  return r;
}

Result dispatch(Device& d, store::Store& s, const Settings& cfg, const uint8_t* pdu, size_t len, uint8_t* out) {
  const uint8_t fc = pdu[0];
  switch (fc) {
    case 1: case 2: case 3: case 4: return read(d, s, cfg, fc, pdu, len, out);
    case 5: case 15: return writeCoils(d, s, cfg, fc, pdu, len, out);
    case 6: case 16: return writeRegisters(d, s, cfg, fc, pdu, len, out);
    case 8:
      if (len >= 3 && be16(pdu + 1) == 0x0000 && len <= MaxPdu) {  // Return Query Data: echo
        std::memcpy(out, pdu, len);
        Result r;
        r.length = len;
        return r;
      }
      return exception(fc, IllegalFunction, out);
    case 0x2B: return deviceId(d, cfg, pdu, len, out);
    default: return exception(fc, IllegalFunction, out);
  }
}
}  // namespace

Result handle(store::Store& s, const Settings& cfg, uint8_t unitId, const uint8_t* pdu, size_t len, uint8_t* out) {
  Result none;
  none.reply = false;
  if (len < 1 || len > MaxPdu) return none;
  if (unitId == 0) {  // broadcast: apply writes on every device, never reply
    const uint8_t fc = pdu[0];
    if (fc == 5 || fc == 6 || fc == 15 || fc == 16) {
      uint8_t scratch[MaxPdu];
      for (auto& d : s.devices)
        if (d.enabled) dispatch(d, s, cfg, pdu, len, scratch);
    }
    return none;
  }
  Device* d = s.find(unitId);
  if (!d && cfg.respondToAnyUnitId) d = s.firstEnabled();
  if (!d) return none;  // like a dead device behind a gateway
  Result r = dispatch(*d, s, cfg, pdu, len, out);
  r.unitId = unitId;
  return r;
}
}  // namespace mbproc
