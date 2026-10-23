// Host tests for the simulation engine and sim TOML output (pio test -e native).
#include <unity.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include "profiles/sim_engine.h"

using namespace profile;
namespace fs = std::filesystem;

static std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

static std::shared_ptr<Profile> bench;
static sim::Config benchSim;
static store::Store st;

static size_t idx(const char* id) {
  for (size_t i = 0; i < bench->tags.size(); ++i)
    if (bench->tags[i].id == id) return i;
  TEST_FAIL_MESSAGE("tag not found");
  return 0;
}
static double value(const char* id) { return st.devices[0].values[idx(id)].value; }

void setUp() {
  bench = std::make_shared<Profile>();
  TEST_ASSERT_EQUAL(int(Status::Ok), int(parse(readFile("data/profiles/test_bench.toml"), *bench).status));
  std::string error;
  TEST_ASSERT_TRUE(sim::parse(readFile("data/sims/test_bench.sim.toml"), *bench, benchSim, error));
  st.devices.clear();
  st.devices.emplace_back(1, true, bench);
  for (size_t i = 0; i < bench->tags.size(); ++i)
    st.devices[0].setValue(i, sim::startValue(bench->tags[i], benchSim.find(bench->tags[i].id)));
}
void tearDown() {}

void test_waveforms_over_time() {
  simeng::Engine eng(st);
  eng.configure(0, benchSim, 0);
  eng.tick(0);
  TEST_ASSERT_DOUBLE_WITHIN(0.06, 12.5, value("dp"));                 // sine 5..20 starts mid-range (+ noise 0.05)
  TEST_ASSERT_DOUBLE_WITHIN(1e-4, 10.0, value("temperature_cdab"));   // ramp 10..40 starts at min
  TEST_ASSERT_EQUAL_DOUBLE(1.0, value("high_alarm"));                 // toggle: on for the first half
  eng.tick(15000);                                                    // a quarter of the 60 s sine
  TEST_ASSERT_DOUBLE_WITHIN(0.06, 20.0, value("dp"));
  TEST_ASSERT_TRUE(value("dp") <= 20.0);                              // noise is clamped to max
  TEST_ASSERT_EQUAL_DOUBLE(0.0, value("high_alarm"));                 // 15 s of a 30 s toggle
  eng.tick(150000);                                                   // half of the 300 s ramp
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, 25.0, value("temperature_cdab"));
  TEST_ASSERT_DOUBLE_WITHIN(0.06, 12.8, value("battery_voltage"));    // static with noise
  TEST_ASSERT_EQUAL_DOUBLE(15.0, value("setpoint"));                  // echo never moves by itself
  const double sp = value("sp");
  TEST_ASSERT_TRUE(sp >= 3400 && sp <= 3500);                         // random within range
}

void test_write_overrides_until_resume() {
  simeng::Engine eng(st);
  eng.configure(0, benchSim, 0);
  const uint16_t words[2] = {0x41F0, 0x0000};  // 30.0, as a client write
  {
    mb::Guard g(st.lock);
    st.devices[0].setWords(idx("dp"), words);
  }
  eng.tick(15000);
  TEST_ASSERT_EQUAL_DOUBLE(30.0, value("dp"));  // the simulation leaves it alone
  eng.resume(0, idx("dp"));
  eng.tick(15001);
  TEST_ASSERT_DOUBLE_WITHIN(0.1, 20.0, value("dp"));
}

void test_freeze_holds_values() {
  simeng::Engine eng(st);
  eng.configure(0, benchSim, 0);
  eng.tick(0);
  const double before = value("temperature_cdab");
  eng.setFrozen(0, true);
  eng.tick(150000);
  TEST_ASSERT_EQUAL_DOUBLE(before, value("temperature_cdab"));
  eng.setFrozen(0, false);
  eng.tick(150000);
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, 25.0, value("temperature_cdab"));
}

void test_step_counter_and_ldr() {
  sim::Config c;
  std::string error;
  const char* text = R"(schema_version = 1
profile_id = "test_bench"
[[sim]]
tag = "dp"
mode = "step"
min = 0
max = 100
period_s = 10
update_ms = 100
[[sim]]
tag = "run_mode"
mode = "counter"
min = 0
max = 2
update_ms = 100
[[sim]]
tag = "light_level"
mode = "ldr"
update_ms = 100
)";
  TEST_ASSERT_TRUE(sim::parse(text, *bench, c, error));
  TEST_ASSERT_EQUAL(0, int(c.warnings.size()));
  simeng::Engine eng(st);
  eng.ldr = [] { return 2047.5; };
  eng.configure(0, c, 0);
  const double steps[] = {0, 25, 50, 75, 100};
  const double counts[] = {0, 1, 2, 0, 1};
  for (int i = 0; i < 5; ++i) {
    eng.tick(uint32_t(i * 2000));  // 2 s per stair of the 10 s period
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, steps[i], value("dp"));
    TEST_ASSERT_EQUAL_DOUBLE(counts[i], value("run_mode"));
  }
  TEST_ASSERT_EQUAL_DOUBLE(2048, value("light_level"));  // uint16: rounded raw ADC
  simeng::Engine noSensor(st);
  noSensor.configure(0, c, 0);  // ldr not set: the value stays where it was
  noSensor.tick(0);
  TEST_ASSERT_EQUAL_DOUBLE(2048, value("light_level"));
}

void test_sim_toml_round_trip() {
  const std::string text = sim::toToml(benchSim);
  sim::Config back;
  std::string error;
  TEST_ASSERT_TRUE_MESSAGE(sim::parse(text, *bench, back, error), error.c_str());
  TEST_ASSERT_EQUAL(0, int(back.warnings.size()));
  TEST_ASSERT_EQUAL(int(benchSim.entries.size()), int(back.entries.size()));
  for (size_t i = 0; i < back.entries.size(); ++i) {
    const auto &a = benchSim.entries[i], &b = back.entries[i];
    TEST_ASSERT_EQUAL_STRING(a.tag.c_str(), b.tag.c_str());
    TEST_ASSERT_EQUAL(int(a.mode), int(b.mode));
    TEST_ASSERT_TRUE(std::isnan(a.min) ? std::isnan(b.min) : a.min == b.min);
    TEST_ASSERT_TRUE(std::isnan(a.initial) ? std::isnan(b.initial) : a.initial == b.initial);
    TEST_ASSERT_EQUAL_UINT32(a.updateMs, b.updateMs);
  }
}

void test_templates_for_every_profile() {
  int profiles = 0;
  for (const auto& entry : fs::directory_iterator("test/fixtures")) {
    if (entry.path().extension() != ".toml") continue;
    Profile p;
    if (parse(readFile(entry.path()), p).status != Status::Ok) continue;
    ++profiles;
    const std::string tmpl = sim::templateToml(p);
    sim::Config c;
    std::string error;
    const std::string name = entry.path().filename().string();
    TEST_ASSERT_TRUE_MESSAGE(sim::parse(tmpl, p, c, error), (name + ": " + error).c_str());
    TEST_ASSERT_EQUAL_MESSAGE(0, int(c.warnings.size()), (name + ": template has warnings").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(int(p.mappedCount()), int(c.entries.size()), name.c_str());
    for (const auto& t : p.tags) {
      if (!t.writable) continue;
      TEST_ASSERT_EQUAL_MESSAGE(int(sim::Mode::Echo), int(c.find(t.id)->mode), (name + ": setpoints echo").c_str());
    }
  }
  TEST_ASSERT_EQUAL(9, profiles);
}

void test_m31_emulation_sim() {
  Profile p;
  TEST_ASSERT_EQUAL(int(Status::Ok), int(parse(readFile("data/profiles/riot_m31_emulation.toml"), p).status));
  sim::Config c;
  std::string error;
  TEST_ASSERT_TRUE_MESSAGE(sim::parse(readFile("data/sims/riot_m31_emulation.sim.toml"), p, c, error), error.c_str());
  TEST_ASSERT_EQUAL(0, int(c.warnings.size()));
  TEST_ASSERT_EQUAL(16, int(c.entries.size()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_m31_emulation_sim);
  RUN_TEST(test_waveforms_over_time);
  RUN_TEST(test_write_overrides_until_resume);
  RUN_TEST(test_freeze_holds_values);
  RUN_TEST(test_step_counter_and_ldr);
  RUN_TEST(test_sim_toml_round_trip);
  RUN_TEST(test_templates_for_every_profile);
  return UNITY_END();
}
