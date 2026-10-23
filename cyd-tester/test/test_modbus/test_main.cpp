// Host tests for the Modbus request handler and sim sidecar (pio test -e native).
#include <unity.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include "modbus/codec.h"
#include "modbus/request_handler.h"
#include "profiles/sim_config.h"

using namespace profile;

static std::string readFile(const char* p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static std::shared_ptr<Profile> bench;
static store::Store st;
static mbproc::Settings cfg;
static uint8_t out[mbproc::MaxPdu];

static size_t tagIndex(const char* id) {
  for (size_t i = 0; i < bench->tags.size(); ++i)
    if (bench->tags[i].id == id) return i;
  TEST_FAIL_MESSAGE("tag not found");
  return 0;
}

void setUp() {
  bench = std::make_shared<Profile>();
  TEST_ASSERT_EQUAL(int(Status::Ok), int(parse(readFile("data/profiles/test_bench.toml"), *bench).status));
  st.devices.clear();
  st.devices.emplace_back(1, true, bench);
  auto& d = st.devices.back();
  d.setValue(tagIndex("dp"), 12.5);
  d.setValue(tagIndex("temperature_cdab"), 12.5);
  d.setValue(tagIndex("battery_voltage"), 12.8);
  d.setValue(tagIndex("enclosure_temp"), -10.5);
  d.setValue(tagIndex("door_open"), 1);
  cfg = mbproc::Settings{};
  cfg.firmwareVersion = "test-fw";
}
void tearDown() {}

static mbproc::Result send(uint8_t unit, std::vector<uint8_t> pdu) {
  return mbproc::handle(st, cfg, unit, pdu.data(), pdu.size(), out);
}
static uint16_t word(size_t i) { return uint16_t(out[2 + i * 2] << 8 | out[3 + i * 2]); }

void test_read_float_abcd_and_cdab() {
  auto r = send(1, {3, 0x01, 0x90, 0x00, 0x02});  // FC3 @400 x2: dp, ABCD
  TEST_ASSERT_TRUE(r.reply);
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_UINT8(4, out[1]);
  TEST_ASSERT_EQUAL_HEX16(0x4148, word(0));
  TEST_ASSERT_EQUAL_HEX16(0x0000, word(1));
  r = send(1, {3, 0x01, 0x94, 0x00, 0x02});  // @404: temperature_cdab
  TEST_ASSERT_EQUAL_HEX16(0x0000, word(0));
  TEST_ASSERT_EQUAL_HEX16(0x4148, word(1));
}

void test_read_input_registers_scaled() {
  auto r = send(1, {4, 0x00, 0x00, 0x00, 0x02});  // FC4 @0 x2: battery (0.01), enclosure (0.1)
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_UINT16(1280, word(0));
  TEST_ASSERT_EQUAL_HEX16(uint16_t(int16_t(-105)), word(1));
  TEST_ASSERT_EQUAL(2, r.tagCount);
}

void test_read_bits() {
  auto r = send(1, {2, 0x00, 0x00, 0x00, 0x02});  // FC2 @0 x2: high_alarm=0, door_open=1
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_UINT8(1, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x02, out[2]);
}

void test_unmapped_read_policy() {
  auto r = send(1, {3, 0x01, 0x91, 0x00, 0x02});  // @401 x2: half of dp, then the first word of sp
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);        // 401 and 402 are both mapped
  r = send(1, {3, 0x01, 0x8F, 0x00, 0x01});       // @399: nothing there
  TEST_ASSERT_EQUAL_HEX8(0x83, out[0]);
  TEST_ASSERT_EQUAL_UINT8(2, r.exception);
  cfg.unmappedReadZero = true;
  r = send(1, {3, 0x01, 0x8F, 0x00, 0x03});       // @399 x3: zero, then dp
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_HEX16(0x0000, word(0));
  TEST_ASSERT_EQUAL_HEX16(0x4148, word(1));
}

void test_write_float_fc16_and_partial() {
  // setpoint @500 (float, FC16). Write 15.0 = 0x4170 0000.
  auto r = send(1, {16, 0x01, 0xF4, 0x00, 0x02, 0x04, 0x41, 0x70, 0x00, 0x00});
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_UINT8(5, r.length);
  const auto& tv = st.devices[0].values[tagIndex("setpoint")];
  TEST_ASSERT_EQUAL_DOUBLE(15.0, tv.value);
  TEST_ASSERT_TRUE(tv.overridden);
  r = send(1, {16, 0x01, 0xF4, 0x00, 0x01, 0x02, 0x41, 0x70});  // only half the float
  TEST_ASSERT_EQUAL_UINT8(2, r.exception);
  r = send(1, {6, 0x01, 0xF4, 0x41, 0x70});                      // FC6 on a float
  TEST_ASSERT_EQUAL_UINT8(1, r.exception);                         // strict: setpoint declares FC16
  cfg.strictFunctionCodes = false;
  r = send(1, {6, 0x01, 0xF4, 0x41, 0x70});
  TEST_ASSERT_EQUAL_UINT8(2, r.exception);                         // and a half-float write is still refused
}

void test_write_rules_strict_fc() {
  auto r = send(1, {6, 0x01, 0xF6, 0x00, 0x03});  // run_mode @502 declares FC6
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_DOUBLE(3, st.devices[0].values[tagIndex("run_mode")].value);
  r = send(1, {16, 0x01, 0xF6, 0x00, 0x01, 0x02, 0x00, 0x04});  // FC16 on an FC6-only tag
  TEST_ASSERT_EQUAL_UINT8(1, r.exception);
  r = send(1, {6, 0x01, 0x90, 0x00, 0x01});  // dp is read-only
  TEST_ASSERT_EQUAL_UINT8(2, r.exception);
  r = send(1, {6, 0x00, 0x10, 0x00, 0x01});  // unmapped write
  TEST_ASSERT_EQUAL_UINT8(2, r.exception);
}

void test_write_coils() {
  auto r = send(1, {5, 0x00, 0x00, 0xFF, 0x00});  // pump_run on
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_DOUBLE(1, st.devices[0].values[tagIndex("pump_run")].value);
  r = send(1, {5, 0x00, 0x00, 0x12, 0x34});        // bad value
  TEST_ASSERT_EQUAL_UINT8(3, r.exception);
  r = send(1, {15, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00});  // FC15 on an FC5 tag (strict)
  TEST_ASSERT_EQUAL_UINT8(1, r.exception);
  r = send(1, {1, 0x00, 0x00, 0x00, 0x01});
  TEST_ASSERT_EQUAL_HEX8(0x01, out[2]);
}

void test_unit_ids_and_broadcast() {
  auto r = send(7, {3, 0x01, 0x90, 0x00, 0x02});
  TEST_ASSERT_FALSE(r.reply);  // unknown unit: silence
  cfg.respondToAnyUnitId = true;
  r = send(7, {3, 0x01, 0x90, 0x00, 0x02});
  TEST_ASSERT_TRUE(r.reply);
  r = send(0, {6, 0x01, 0xF6, 0x00, 0x09});  // broadcast write: applied, no reply
  TEST_ASSERT_FALSE(r.reply);
  TEST_ASSERT_EQUAL_DOUBLE(9, st.devices[0].values[tagIndex("run_mode")].value);
  st.devices[0].enabled = false;  // a disabled device does not answer
  cfg.respondToAnyUnitId = false;
  TEST_ASSERT_FALSE(send(1, {3, 0x01, 0x90, 0x00, 0x02}).reply);
}

void test_diagnostics_and_device_id() {
  auto r = send(1, {8, 0x00, 0x00, 0xAB, 0xCD});
  TEST_ASSERT_EQUAL(5, int(r.length));
  TEST_ASSERT_EQUAL_HEX8(0xCD, out[4]);
  r = send(1, {8, 0x00, 0x01, 0x00, 0x00});  // other sub-functions unsupported
  TEST_ASSERT_EQUAL_UINT8(1, r.exception);
  r = send(1, {0x2B, 0x0E, 0x01, 0x00});
  TEST_ASSERT_EQUAL_UINT8(0, r.exception);
  TEST_ASSERT_EQUAL_UINT8(3, out[6]);  // three objects
  const std::string vendor(reinterpret_cast<const char*>(out + 9), out[8]);
  TEST_ASSERT_EQUAL_STRING("RIOT Test", vendor.c_str());
  r = send(1, {0x2B, 0x0E, 0x04, 0x02});  // individual: firmware version
  TEST_ASSERT_EQUAL_UINT8(1, out[6]);
  TEST_ASSERT_EQUAL_STRING("test-fw", std::string(reinterpret_cast<const char*>(out + 9), out[8]).c_str());
  r = send(1, {0x41});
  TEST_ASSERT_EQUAL_UINT8(1, r.exception);  // unknown function
}

void test_bad_quantities() {
  TEST_ASSERT_EQUAL_UINT8(3, send(1, {3, 0x01, 0x90, 0x00, 0x00}).exception);
  TEST_ASSERT_EQUAL_UINT8(3, send(1, {3, 0x01, 0x90, 0x00, 0x7E}).exception);  // 126 > 125
  TEST_ASSERT_EQUAL_UINT8(2, send(1, {3, 0xFF, 0xFF, 0x00, 0x02}).exception);  // runs past 65535
  TEST_ASSERT_EQUAL_UINT8(3, send(1, {3, 0x01, 0x90}).exception);               // truncated
}

void test_sim_sidecar() {
  sim::Config c;
  std::string error;
  TEST_ASSERT_TRUE(sim::parse(readFile("data/sims/test_bench.sim.toml"), *bench, c, error));
  TEST_ASSERT_EQUAL(11, int(c.entries.size()));
  TEST_ASSERT_EQUAL(0, int(c.warnings.size()));
  const auto* dp = c.find("dp");
  TEST_ASSERT_EQUAL(int(sim::Mode::Sine), int(dp->mode));
  TEST_ASSERT_EQUAL_DOUBLE(12.5, sim::startValue(*bench->find("dp"), dp));  // middle of 5..20
  TEST_ASSERT_EQUAL_DOUBLE(15.0, sim::startValue(*bench->find("setpoint"), c.find("setpoint")));
  TEST_ASSERT_EQUAL_DOUBLE(0.0, sim::startValue(*bench->find("light_level"), c.find("light_level")));
  TEST_ASSERT_TRUE(sim::parse("schema_version = 1\nprofile_id = \"test_bench\"\n[[sim]]\ntag = \"nope\"\n", *bench, c, error));
  TEST_ASSERT_EQUAL(0, int(c.entries.size()));
  TEST_ASSERT_EQUAL(1, int(c.warnings.size()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_read_float_abcd_and_cdab);
  RUN_TEST(test_read_input_registers_scaled);
  RUN_TEST(test_read_bits);
  RUN_TEST(test_unmapped_read_policy);
  RUN_TEST(test_write_float_fc16_and_partial);
  RUN_TEST(test_write_rules_strict_fc);
  RUN_TEST(test_write_coils);
  RUN_TEST(test_unit_ids_and_broadcast);
  RUN_TEST(test_diagnostics_and_device_id);
  RUN_TEST(test_bad_quantities);
  RUN_TEST(test_sim_sidecar);
  return UNITY_END();
}
