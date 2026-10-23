// Host unit tests (pio test -e native): profile parser, address conversion, value codec.
#include <unity.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "modbus/address_map.h"
#include "modbus/codec.h"
#include "profiles/profile.h"

using namespace profile;
namespace fs = std::filesystem;

void setUp() {}
void tearDown() {}

static std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static Tag tagOf(DataType type, ByteOrder order = ByteOrder::ABCD, float scale = 1.0f, bool swap = false) {
  Tag t;
  t.type = type;
  t.order = order;
  t.scale = scale;
  t.swapBytes = swap;
  return t;
}

// ---- Address conversion (spec section 12) ----
static void expectWire(uint32_t reg, uint8_t fc, AddressMode mode, uint16_t wire) {
  const auto c = addr::toWire(reg, fc, mode);
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error.c_str());
  TEST_ASSERT_EQUAL_UINT16(wire, c.wire);
  TEST_ASSERT_TRUE_MESSAGE(c.warning.empty(), c.warning.c_str());
}

void test_address_spec_vectors() {
  expectWire(40401, 3, AddressMode::Reference, 400);
  expectWire(30001, 4, AddressMode::Reference, 0);
  expectWire(10001, 2, AddressMode::Reference, 0);
  expectWire(1, 1, AddressMode::Reference, 0);
  expectWire(401, 3, AddressMode::OneBased, 400);
  expectWire(401, 3, AddressMode::ZeroBased, 401);
  expectWire(401, 3, AddressMode::Unstated, 401);
}

void test_address_six_digit_and_errors() {
  expectWire(400001, 3, AddressMode::Reference, 0);
  expectWire(465536, 3, AddressMode::Reference, 65535);
  expectWire(300010, 4, AddressMode::Reference, 9);
  // A 3xxxx reference on an FC3 tag loads, at the reference's own offset, with a warning.
  auto mismatch = addr::toWire(30005, 3, AddressMode::Reference);
  TEST_ASSERT_TRUE(mismatch.ok);
  TEST_ASSERT_EQUAL_UINT16(4, mismatch.wire);
  TEST_ASSERT_FALSE(mismatch.warning.empty());
  TEST_ASSERT_FALSE(addr::toWire(25000, 3, AddressMode::Reference).ok);   // not a reference number
  TEST_ASSERT_FALSE(addr::toWire(0, 3, AddressMode::OneBased).ok);
  TEST_ASSERT_FALSE(addr::toWire(70000, 3, AddressMode::ZeroBased).ok);   // beyond 16 bits
}

// ---- Codec (spec section 12) ----
void test_codec_float_orders() {
  uint16_t w[2];
  codec::encode(tagOf(DataType::Float32, ByteOrder::ABCD), 12.5, w);
  TEST_ASSERT_EQUAL_HEX16(0x4148, w[0]);
  TEST_ASSERT_EQUAL_HEX16(0x0000, w[1]);
  codec::encode(tagOf(DataType::Float32, ByteOrder::CDAB), 12.5, w);
  TEST_ASSERT_EQUAL_HEX16(0x0000, w[0]);
  TEST_ASSERT_EQUAL_HEX16(0x4148, w[1]);
  codec::encode(tagOf(DataType::Float32, ByteOrder::BADC), 12.5, w);
  TEST_ASSERT_EQUAL_HEX16(0x4841, w[0]);
  TEST_ASSERT_EQUAL_HEX16(0x0000, w[1]);
  codec::encode(tagOf(DataType::Float32, ByteOrder::DCBA), 12.5, w);
  TEST_ASSERT_EQUAL_HEX16(0x0000, w[0]);
  TEST_ASSERT_EQUAL_HEX16(0x4841, w[1]);
  for (auto o : {ByteOrder::ABCD, ByteOrder::CDAB, ByteOrder::BADC, ByteOrder::DCBA}) {
    const Tag t = tagOf(DataType::Float32, o);
    codec::encode(t, -1234.5625, w);
    TEST_ASSERT_EQUAL_DOUBLE(-1234.5625, codec::decode(t, w));
  }
}

void test_codec_integers() {
  uint16_t w[1];
  const Tag i16 = tagOf(DataType::Int16);
  codec::encode(i16, -1, w);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, w[0]);
  TEST_ASSERT_EQUAL_DOUBLE(-1, codec::decode(i16, w));
  const Tag u16 = tagOf(DataType::UInt16);
  codec::encode(u16, 65535, w);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, w[0]);
  TEST_ASSERT_EQUAL_DOUBLE(65535, codec::decode(u16, w));
  codec::encode(u16, 70000, w);  // clamps
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, w[0]);
  codec::encode(u16, -5, w);
  TEST_ASSERT_EQUAL_HEX16(0x0000, w[0]);
  const Tag swapped = tagOf(DataType::UInt16, ByteOrder::ABCD, 1.0f, true);  // short_packing = "BA"
  codec::encode(swapped, 0x1234, w);
  TEST_ASSERT_EQUAL_HEX16(0x3412, w[0]);
  TEST_ASSERT_EQUAL_DOUBLE(0x1234, codec::decode(swapped, w));
  const Tag scaled = tagOf(DataType::UInt16, ByteOrder::ABCD, 0.01f);
  codec::encode(scaled, 12.8, w);
  TEST_ASSERT_EQUAL_UINT16(1280, w[0]);
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 12.8, codec::decode(scaled, w));
  const Tag scaledSigned = tagOf(DataType::Int16, ByteOrder::ABCD, 0.1f);
  codec::encode(scaledSigned, -10.5, w);
  TEST_ASSERT_EQUAL_HEX16(uint16_t(int16_t(-105)), w[0]);
}

// ---- Parser ----
void test_bench_profile() {
  Profile p;
  const auto r = parse(readFile("data/profiles/test_bench.toml"), p);
  TEST_ASSERT_EQUAL_MESSAGE(int(Status::Ok), int(r.status), r.message.c_str());
  TEST_ASSERT_EQUAL(11, int(p.tags.size()));
  TEST_ASSERT_EQUAL(11, int(p.mappedCount()));
  TEST_ASSERT_EQUAL(int(AddressMode::Reference), int(p.addressMode));
  const Tag* dp = p.find("dp");
  TEST_ASSERT_NOT_NULL(dp);
  TEST_ASSERT_EQUAL_UINT16(400, dp->wire);
  TEST_ASSERT_EQUAL(int(Space::HoldingRegister), int(dp->space));
  TEST_ASSERT_EQUAL(int(ByteOrder::CDAB), int(p.find("temperature_cdab")->order));
  const Tag* batt = p.find("battery_voltage");
  TEST_ASSERT_EQUAL(int(Space::InputRegister), int(batt->space));
  TEST_ASSERT_EQUAL_UINT16(0, batt->wire);
  TEST_ASSERT_EQUAL_FLOAT(0.01f, batt->scale);
  const Tag* sp = p.find("setpoint");
  TEST_ASSERT_TRUE(sp->writable);
  TEST_ASSERT_EQUAL_UINT8(16, sp->writeFc);
  const Tag* pump = p.find("pump_run");
  TEST_ASSERT_EQUAL(int(Space::Coil), int(pump->space));
  TEST_ASSERT_EQUAL_UINT16(0, pump->wire);
  TEST_ASSERT_EQUAL_UINT8(5, pump->writeFc);
  TEST_ASSERT_FALSE(p.find("high_alarm")->writable);
}

void test_all_reference_profiles() {
  int files = 0, modbusProfiles = 0;
  for (const auto& entry : fs::directory_iterator("test/fixtures")) {
    if (entry.path().extension() != ".toml") continue;
    ++files;
    const std::string name = entry.path().filename().string();
    Profile p;
    const auto r = parse(readFile(entry.path()), p);
    if (name == "riot_m31.toml") {
      TEST_ASSERT_EQUAL_MESSAGE(int(Status::NotModbus), int(r.status), "riot_m31.toml must be rejected as non-Modbus");
      std::printf("  %-34s rejected: %s\n", name.c_str(), r.message.c_str());
      continue;
    }
    TEST_ASSERT_EQUAL_MESSAGE(int(Status::Ok), int(r.status), (name + ": " + r.message).c_str());
    TEST_ASSERT_TRUE_MESSAGE(!p.tags.empty(), (name + ": no tags").c_str());
    ++modbusProfiles;
    std::printf("  %-34s %3zu tags (%zu mapped), address %-10s, %zu warning(s)\n", name.c_str(), p.tags.size(),
                p.mappedCount(), toString(p.addressMode), p.warnings.size());
    for (const auto& w : p.warnings) std::printf("      - %s\n", w.c_str());
  }
  TEST_ASSERT_EQUAL_MESSAGE(10, files, "expected the 10 reference profiles in test/fixtures");
  TEST_ASSERT_EQUAL(9, modbusProfiles);
}

void test_scadapack_dp_wire() {
  Profile p;
  TEST_ASSERT_EQUAL(int(Status::Ok), int(parse(readFile("test/fixtures/scadapack_4102_metric.toml"), p).status));
  const Tag* dp = p.find("dp");
  TEST_ASSERT_NOT_NULL(dp);
  TEST_ASSERT_EQUAL_UINT16(400, dp->wire);  // spec example: 40401 -> 400
}

void test_parser_edge_cases() {
  Profile p;
  const char* text = R"(schema_version = 2
extra_top = 1
[device]
profile_id = "edge"
[modbus]
address_format = "reference"
[[tags]]
id = "old_style"
function = 3
register = 40010
data_type = "uint16"
access = "read"
[[tags]]
id = "no_fc"
register = 40011
data_type = "uint16"
[[tags]]
id = "bad_type"
read_function = 3
register = 40012
data_type = "float64"
[[tags]]
id = "mystery"
read_function = 3
register = 40013
data_type = "int16"
colour = "red"
)";
  const auto r = parse(text, p);
  TEST_ASSERT_EQUAL_MESSAGE(int(Status::Ok), int(r.status), r.message.c_str());
  TEST_ASSERT_EQUAL(3, int(p.tags.size()));  // bad_type skipped
  TEST_ASSERT_EQUAL_UINT8(3, p.find("old_style")->readFc);
  TEST_ASSERT_EQUAL_UINT16(9, p.find("old_style")->wire);
  TEST_ASSERT_FALSE(p.find("no_fc")->mapped);
  TEST_ASSERT_NULL(p.find("bad_type"));
  int unknown = 0;
  for (const auto& w : p.warnings) unknown += w.find("unknown key") != std::string::npos;
  TEST_ASSERT_EQUAL(2, unknown);  // extra_top and colour

  TEST_ASSERT_EQUAL(int(Status::Error), int(parse("this is = = not toml", p).status));
  TEST_ASSERT_EQUAL(int(Status::NotModbus), int(parse("schema_version = 2\n[device]\nprofile_id = \"x\"\n", p).status));
}

void test_m31_emulation_profile() {
  Profile p;
  const auto r = parse(readFile("data/profiles/riot_m31_emulation.toml"), p);
  TEST_ASSERT_EQUAL_MESSAGE(int(Status::Ok), int(r.status), r.message.c_str());
  TEST_ASSERT_EQUAL(16, int(p.mappedCount()));
  TEST_ASSERT_EQUAL(0, int(p.warnings.size()));
  // The RIOT M31 driver: AI FC4 0-3 (uA), DI FC2 0-3, DO FC1 0-3 written with FC15, AO FC3 0-3 written with FC6.
  TEST_ASSERT_EQUAL_UINT16(3, p.find("ai4")->wire);
  TEST_ASSERT_EQUAL(int(Space::InputRegister), int(p.find("ai1")->space));
  TEST_ASSERT_EQUAL_FLOAT(0.001f, p.find("ai1")->scale);
  TEST_ASSERT_EQUAL(int(Space::DiscreteInput), int(p.find("di1")->space));
  TEST_ASSERT_EQUAL_UINT8(15, p.find("do2")->writeFc);
  TEST_ASSERT_EQUAL_UINT8(6, p.find("ao1")->writeFc);
}

void test_chunked_parsing() {
  // A multi-line string holding a "[table]"-looking line must not split the document,
  // header comments are allowed, and errors report the line in the whole file.
  const char* text = R"(schema_version = 2
[device]   # identity
profile_id = "chunks"
description = """
[not a header]
"""
[modbus]
address_format = "zero_based"
[[tags]]
id = "a"
read_function = 3
register = 5
data_type = "uint16"
)";
  Profile p;
  const auto r = parse(text, p);
  TEST_ASSERT_EQUAL_MESSAGE(int(Status::Ok), int(r.status), r.message.c_str());
  TEST_ASSERT_EQUAL(1, int(p.tags.size()));
  TEST_ASSERT_EQUAL_UINT16(5, p.find("a")->wire);
  const auto bad = parse("schema_version = 2\n[device]\nprofile_id = \"x\"\n[[tags]]\nid = \"a\"\nregister = = 1\n", p);
  TEST_ASSERT_EQUAL(int(Status::Error), int(bad.status));
  TEST_ASSERT_TRUE_MESSAGE(bad.message.find("line 6") != std::string::npos, bad.message.c_str());
  TEST_ASSERT_EQUAL(int(Status::Error), int(parse("[device\nprofile_id = \"x\"\n", p).status));  // malformed header
  TEST_ASSERT_EQUAL(int(Status::Error),
                    int(parse("[device]\nprofile_id = \"x\"\n[device]\nmodel = \"y\"\n", p).status));  // table twice
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_chunked_parsing);
  RUN_TEST(test_m31_emulation_profile);
  RUN_TEST(test_address_spec_vectors);
  RUN_TEST(test_address_six_digit_and_errors);
  RUN_TEST(test_codec_float_orders);
  RUN_TEST(test_codec_integers);
  RUN_TEST(test_bench_profile);
  RUN_TEST(test_scadapack_dp_wire);
  RUN_TEST(test_parser_edge_cases);
  RUN_TEST(test_all_reference_profiles);
  return UNITY_END();
}
