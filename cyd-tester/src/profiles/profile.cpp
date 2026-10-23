#include "profile.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <toml++/toml.hpp>
#include "../modbus/address_map.h"
#include "toml_chunks.h"

namespace profile {
const char* toString(DataType t) {
  switch (t) {
    case DataType::Bool: return "bool";
    case DataType::UInt16: return "uint16";
    case DataType::Int16: return "int16";
    case DataType::Float32: return "float32";
  }
  return "?";
}
const char* toString(ByteOrder o) {
  switch (o) {
    case ByteOrder::ABCD: return "ABCD";
    case ByteOrder::CDAB: return "CDAB";
    case ByteOrder::BADC: return "BADC";
    case ByteOrder::DCBA: return "DCBA";
  }
  return "?";
}
const char* toString(Space s) {
  switch (s) {
    case Space::Coil: return "coil";
    case Space::DiscreteInput: return "discrete input";
    case Space::InputRegister: return "input register";
    case Space::HoldingRegister: return "holding register";
  }
  return "?";
}
const char* toString(AddressMode m) {
  switch (m) {
    case AddressMode::Reference: return "reference";
    case AddressMode::OneBased: return "one_based";
    case AddressMode::ZeroBased: return "zero_based";
    case AddressMode::Unstated: return "not stated";
  }
  return "?";
}

size_t Profile::mappedCount() const {
  return size_t(std::count_if(tags.begin(), tags.end(), [](const Tag& t) { return t.mapped; }));
}
const Tag* Profile::find(std::string_view id) const {
  for (const auto& t : tags)
    if (t.id == id) return &t;
  return nullptr;
}

namespace {
using Warnings = std::vector<std::string>;

void unknownKeys(const toml::table& t, std::initializer_list<const char*> known, const std::string& where, Warnings& w) {
  for (const auto& [key, node] : t) {
    const std::string_view k = key.str();
    if (std::none_of(known.begin(), known.end(), [&](const char* x) { return k == x; }))
      w.push_back(where + "unknown key '" + std::string(k) + "' ignored");
  }
}

std::string str(const toml::table& t, const char* key, const std::string& fallback = {}) {
  if (auto v = t[key].value<std::string>()) return *v;
  return fallback;
}

bool parseOrder(const std::string& s, ByteOrder& out) {
  if (s == "ABCD" || s == "HIGH_LOW") { out = ByteOrder::ABCD; return true; }
  if (s == "CDAB" || s == "LOW_HIGH") { out = ByteOrder::CDAB; return true; }
  if (s == "BADC") { out = ByteOrder::BADC; return true; }
  if (s == "DCBA") { out = ByteOrder::DCBA; return true; }
  return false;
}

bool parseType(const std::string& s, DataType& out) {
  if (s == "bool") { out = DataType::Bool; return true; }
  if (s == "uint16") { out = DataType::UInt16; return true; }
  if (s == "int16") { out = DataType::Int16; return true; }
  if (s == "float32") { out = DataType::Float32; return true; }
  return false;
}

// Two tags sharing a register (or a float straddling another tag) is almost always a profile error.
void checkOverlaps(const Profile& p, Warnings& w) {
  std::map<std::pair<int, uint32_t>, const Tag*> used;
  for (const auto& t : p.tags) {
    if (!t.mapped) continue;
    for (uint8_t i = 0; i < wordCount(t.type); ++i) {
      const auto key = std::make_pair(int(t.space), uint32_t(t.wire) + i);
      auto [it, inserted] = used.emplace(key, &t);
      if (!inserted && it->second != &t)
        w.push_back("tag '" + t.id + "' overlaps tag '" + it->second->id + "' at " + toString(t.space) + " wire " +
                    std::to_string(t.wire + i) + "; the later tag is served there");
    }
  }
}
}  // namespace

ParseResult parse(std::string_view text, Profile& out) {
  out = Profile{};
  Warnings& w = out.warnings;
  // One table at a time (see toml_chunks.h): small tables first, then the tags stream through.
  std::vector<tomlx::Chunk> chunks;
  std::string error;
  if (!tomlx::split(text, chunks, error)) return {Status::Error, error};
  toml::table root, deviceTable, modbusTable, driverTable;
  bool haveDevice = false, haveModbus = false, haveDriver = false;
  size_t tagChunks = 0;
  for (const auto& c : chunks) {
    if (c.array) {
      if (c.name == "tags") ++tagChunks;
      else w.push_back("unknown array [[" + c.name + "]] ignored");
      continue;
    }
    toml::table* target = c.name.empty() ? &root : c.name == "device" ? &deviceTable : c.name == "modbus" ? &modbusTable
                        : c.name == "driver" ? &driverTable : nullptr;
    if (!target) { w.push_back("unknown table [" + c.name + "] ignored"); continue; }
    bool& seen = c.name == "device" ? haveDevice : c.name == "modbus" ? haveModbus : haveDriver;
    if (!c.name.empty()) {
      if (seen) return {Status::Error, "[" + c.name + "] is defined twice"};
      seen = true;
    }
    if (!tomlx::parse(c, *target, error)) return {Status::Error, error};
  }
  unknownKeys(root, {"schema_version"}, "", w);

  const toml::table* modbus = haveModbus ? &modbusTable : nullptr;
  if (!modbus && !tagChunks) {
    if (haveDriver) {
      const std::string type = str(driverTable, "type", "unknown");
      return {Status::NotModbus, "This is a RIOT driver profile (type = \"" + type +
                                     "\"), not a Modbus device profile. It has no [modbus] table or [[tags]] to serve."};
    }
    return {Status::NotModbus, "No [modbus] table or [[tags]]: not a Modbus device profile."};
  }

  out.schemaVersion = int(root["schema_version"].value_or<int64_t>(0));
  if (out.schemaVersion != 1 && out.schemaVersion != 2)
    w.push_back("schema_version " + std::to_string(out.schemaVersion) + " not recognised; parsed with the version 2 rules");

  const toml::table* device = haveDevice ? &deviceTable : nullptr;
  if (!device) return {Status::Error, "[device] table is missing"};
  unknownKeys(*device, {"profile_id", "profile_version", "vendor", "model", "description"}, "[device] ", w);
  out.profileId = str(*device, "profile_id");
  if (out.profileId.empty()) return {Status::Error, "[device] profile_id is missing"};
  out.profileVersion = int(device->get("profile_version") ? (*device)["profile_version"].value_or<int64_t>(0) : 0);
  out.vendor = str(*device, "vendor");
  out.model = str(*device, "model");

  ByteOrder defaultOrder = ByteOrder::ABCD;
  bool swap16 = false;
  if (modbus) {
    unknownKeys(*modbus, {"transport", "unit_id", "address_format", "address_basis", "short_packing", "packing",
                          "long_packing", "default_baud", "baud"}, "[modbus] ", w);
    if (auto u = (*modbus)["unit_id"].value<int64_t>()) {
      if (*u >= 1 && *u <= 247) out.unitId = uint8_t(*u);
      else w.push_back("[modbus] unit_id " + std::to_string(*u) + " is outside 1-247; ignored");
    }
    const std::string format = str(*modbus, "address_format"), basis = str(*modbus, "address_basis");
    if (format == "reference") out.addressMode = AddressMode::Reference;
    else if (format == "zero_based") out.addressMode = AddressMode::ZeroBased;
    else if (!format.empty()) w.push_back("[modbus] address_format \"" + format + "\" not recognised");
    if (out.addressMode == AddressMode::Unstated) {
      if (basis == "one_based") out.addressMode = AddressMode::OneBased;
      else if (basis == "zero_based") out.addressMode = AddressMode::ZeroBased;
      else if (!basis.empty()) w.push_back("[modbus] address_basis \"" + basis + "\" not recognised");
    }
    if (out.addressMode == AddressMode::Unstated)
      w.push_back("address basis not stated: register numbers are used unchanged on the wire");

    const std::string sp = str(*modbus, "short_packing");
    if (sp == "BA") swap16 = true;
    else if (!sp.empty() && sp != "AB") w.push_back("[modbus] short_packing \"" + sp + "\" not recognised; using AB");
    std::string lp = str(*modbus, "long_packing");
    if (lp.empty()) lp = str(*modbus, "packing");
    if (!lp.empty() && !parseOrder(lp, defaultOrder))
      w.push_back("[modbus] long_packing/packing \"" + lp + "\" not recognised; using ABCD");
  } else {
    w.push_back("[modbus] table missing; address basis not stated: register numbers are used unchanged on the wire");
  }

  size_t index = 0;
  for (const auto& c : chunks) {
    if (!c.array || c.name != "tags") continue;
    std::string where = "tags[" + std::to_string(index++) + "]";
    toml::table tagTable;  // freed at the end of each iteration: only one tag's DOM is alive
    if (!tomlx::parse(c, tagTable, error)) return {Status::Error, error};
    const toml::table* t = &tagTable;
    Tag tag;
    tag.id = str(*t, "id");
    if (tag.id.empty()) { w.push_back(where + " has no id; skipped"); continue; }
    where = "tag '" + tag.id + "': ";
    if (out.find(tag.id)) { w.push_back(where + "duplicate id; skipped"); continue; }
    unknownKeys(*t, {"id", "name", "description", "read_function", "function", "write_function", "register", "data_type",
                     "word_order", "scale", "units", "access", "default_enabled", "poll_group", "pool_group"}, where, w);

    tag.name = str(*t, "name", tag.id);
    tag.units = str(*t, "units");
    tag.pollGroup = str(*t, "poll_group");
    tag.poolGroup = str(*t, "pool_group");
    tag.defaultEnabled = (*t)["default_enabled"].value_or(false);

    const std::string typeText = str(*t, "data_type");
    if (!parseType(typeText, tag.type)) { w.push_back(where + "unknown data_type \"" + typeText + "\"; skipped"); continue; }
    if (tag.type == DataType::Float32) {
      tag.order = defaultOrder;
      const std::string wo = str(*t, "word_order");
      if (!wo.empty() && !parseOrder(wo, tag.order)) { w.push_back(where + "unknown word_order \"" + wo + "\"; skipped"); continue; }
    } else {
      tag.swapBytes = swap16 && tag.type != DataType::Bool;
      if (t->get("word_order")) w.push_back(where + "word_order applies to float32 only; ignored");
    }

    if (auto s = (*t)["scale"].value<double>()) {
      if (*s != 0.0) tag.scale = float(*s);
      else w.push_back(where + "scale 0 is invalid; using 1.0");
    }

    const auto regNode = (*t)["register"].value<int64_t>();
    if (!regNode || *regNode < 0) { w.push_back(where + "register missing or negative; skipped"); continue; }
    tag.reg = uint32_t(*regNode);

    // Function code: read_function, else the older `function`, else unmapped.
    auto fc = (*t)["read_function"].value<int64_t>();
    if (!fc) fc = (*t)["function"].value<int64_t>();
    if (!fc) {
      tag.mapped = false;
      w.push_back(where + "no read_function or function; shown as unmapped and not served");
    } else if (*fc < 1 || *fc > 4 || !spaceForReadFc(uint8_t(*fc), tag.space)) {
      tag.mapped = false;
      w.push_back(where + "read function " + std::to_string(*fc) + " is not 1-4; shown as unmapped and not served");
    } else {
      tag.readFc = uint8_t(*fc);
    }
    if (tag.mapped) {
      const bool bitSpace = isBit(tag.space);
      if (bitSpace != (tag.type == DataType::Bool)) {
        w.push_back(where + std::string(toString(tag.type)) + " cannot be read with FC" + std::to_string(tag.readFc) + "; skipped");
        continue;
      }
      const addr::Conversion c = addr::toWire(tag.reg, tag.readFc, out.addressMode);
      if (!c.ok) {
        tag.mapped = false;
        w.push_back(where + c.error + "; shown as unmapped and not served");
      } else {
        tag.wire = c.wire;
        if (!c.warning.empty()) w.push_back(where + c.warning);
        if (tag.type == DataType::Float32 && tag.wire == 65535) {
          tag.mapped = false;
          w.push_back(where + "float32 at wire 65535 has no room for its second register; unmapped");
        }
      }
    }

    const std::string access = str(*t, "access", "read");
    if (access != "read" && access != "read_write") w.push_back(where + "access \"" + access + "\" not recognised; treated as read");
    const bool wantsWrite = access == "read_write";
    const bool writableSpace = tag.space == Space::Coil || tag.space == Space::HoldingRegister;
    if (wantsWrite && tag.mapped && !writableSpace)
      w.push_back(where + "read_write on a read-only " + std::string(toString(tag.space)) + "; served read-only");
    tag.writable = wantsWrite && tag.mapped && writableSpace;

    if (auto wf = (*t)["write_function"].value<int64_t>()) {
      const int f = int(*wf);
      const bool fits = (f == 5 || f == 15) ? tag.space == Space::Coil
                        : f == 6            ? tag.space == Space::HoldingRegister && wordCount(tag.type) == 1
                        : f == 16           ? tag.space == Space::HoldingRegister
                                            : false;
      if (!fits) w.push_back(where + "write_function " + std::to_string(f) + " does not suit this tag; ignored");
      else if (!tag.writable) w.push_back(where + "write_function " + std::to_string(f) + " set but access is read; ignored");
      else tag.writeFc = uint8_t(f);
    }
    out.tags.push_back(std::move(tag));
  }
  checkOverlaps(out, w);
  return {Status::Ok, {}};
}
}  // namespace profile
