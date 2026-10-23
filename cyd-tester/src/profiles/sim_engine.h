#pragma once
#include <cstdint>
#include <vector>
#include "../modbus/value_store.h"
#include "sim_config.h"

// Drives simulated tag values in the value store (PROMPT.md section 6). Portable C++17.
//   static  initial value (+ noise)
//   ramp    min -> max linearly over period_s, then restarts (sawtooth)
//   sine    centred between min and max, one cycle per period_s
//   random  uniform in [min, max] on every update
//   step    climbs min -> max in 5 equal steps per period_s, then restarts
//   toggle  on (max, or 1) for the first half of period_s, off (min, or 0) for the second
//   counter +1 per update from initial (or min); wraps to min after max when both are set
//   echo    holds its value; only Modbus writes change it
//   ldr     onboard light sensor, raw 0-4095 scaled to [min, max] when given
// A Modbus write overrides the simulation for that tag until resume().
namespace simeng {
using LdrReader = double (*)();  // raw ADC 0-4095, or NAN when the board has no sensor

class Engine {
 public:
  explicit Engine(store::Store& s) : store_(s) {}
  // Call once per device in store order. The config is copied.
  void configure(size_t device, const sim::Config& cfg, uint32_t nowMs);
  // Updates every tag that is due. Call from the main loop.
  void tick(uint32_t nowMs);
  void resume(size_t device, size_t tag);
  void resumeAll(size_t device);
  void setFrozen(size_t device, bool frozen);
  bool frozen(size_t device) const;
  // The entry driving a tag, or nullptr (static at its start value).
  const sim::Entry* entry(size_t device, size_t tag) const;

  LdrReader ldr = nullptr;

 private:
  struct TagSim {
    int16_t entry = -1;  // index into DeviceSim::cfg.entries
    uint32_t next = 0;
    double counter = 0;
  };
  struct DeviceSim {
    sim::Config cfg;
    std::vector<TagSim> tags;
    bool frozen = false;
  };
  double compute(const profile::Tag& t, const sim::Entry& e, TagSim& ts, uint32_t nowMs);
  double noise(double amplitude);

  store::Store& store_;
  std::vector<DeviceSim> devices_;
  uint32_t startMs_ = 0;
  bool started_ = false;
  uint32_t rng_ = 0x2545F491u;
};
}  // namespace simeng
