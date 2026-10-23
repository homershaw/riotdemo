#include "sim_engine.h"
#include <cmath>

namespace simeng {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr int StepCount = 5;

bool hasRange(const sim::Entry& e) { return !std::isnan(e.min) && !std::isnan(e.max); }
}  // namespace

void Engine::configure(size_t device, const sim::Config& cfg, uint32_t nowMs) {
  if (!started_) { startMs_ = nowMs; started_ = true; }
  if (devices_.size() <= device) devices_.resize(device + 1);
  DeviceSim& d = devices_[device];
  d.cfg = cfg;
  const auto& tags = store_.devices[device].profile->tags;
  d.tags.assign(tags.size(), TagSim{});
  for (size_t i = 0; i < tags.size(); ++i) {
    for (size_t k = 0; k < d.cfg.entries.size(); ++k)
      if (d.cfg.entries[k].tag == tags[i].id) d.tags[i].entry = int16_t(k);
    if (d.tags[i].entry >= 0) {
      const auto& e = d.cfg.entries[size_t(d.tags[i].entry)];
      d.tags[i].counter = sim::startValue(tags[i], &e);
      if (e.mode == sim::Mode::Counter && std::isnan(e.initial) && !std::isnan(e.min)) d.tags[i].counter = e.min;
    }
    d.tags[i].next = nowMs;  // first update on the next tick
  }
}

double Engine::noise(double amplitude) {
  if (amplitude <= 0) return 0;
  rng_ ^= rng_ << 13;  // xorshift32
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return (double(rng_) / 4294967295.0 * 2.0 - 1.0) * amplitude;
}

double Engine::compute(const profile::Tag& t, const sim::Entry& e, TagSim& ts, uint32_t nowMs) {
  const double secs = double(uint32_t(nowMs - startMs_)) / 1000.0;
  const double phase = std::fmod(secs, e.periodS) / e.periodS;  // 0..1 within the period
  const bool range = hasRange(e);
  const double lo = range ? e.min : 0, hi = range ? e.max : 0;
  double v = sim::startValue(t, &e);
  switch (e.mode) {
    case sim::Mode::Static:
    case sim::Mode::Echo:
      break;
    case sim::Mode::Ramp:
      if (range) v = lo + (hi - lo) * phase;
      break;
    case sim::Mode::Sine:
      if (range) v = (lo + hi) / 2 + (hi - lo) / 2 * std::sin(2 * Pi * phase);
      break;
    case sim::Mode::Random:
      if (range) v = lo + (hi - lo) * (noise(1.0) + 1.0) / 2;
      break;
    case sim::Mode::Step:
      if (range) v = lo + (hi - lo) * std::floor(phase * StepCount) / (StepCount - 1);
      break;
    case sim::Mode::Toggle: {
      const bool on = phase < 0.5;
      v = range ? (on ? hi : lo) : (on ? 1 : 0);
      break;
    }
    case sim::Mode::Counter:
      v = ts.counter;
      ts.counter += 1;
      if (range && ts.counter > hi) ts.counter = lo;
      break;
    case sim::Mode::Ldr: {
      const double raw = ldr ? ldr() : NAN;
      if (std::isnan(raw)) return NAN;  // no sensor: leave the value where it is
      v = range ? lo + (hi - lo) * raw / 4095.0 : raw;
      break;
    }
  }
  if (e.mode != sim::Mode::Echo && e.mode != sim::Mode::Counter) v += noise(e.noise);
  if (range && e.mode != sim::Mode::Static) v = v < lo ? lo : v > hi ? hi : v;
  return v;
}

void Engine::tick(uint32_t nowMs) {
  for (size_t di = 0; di < devices_.size() && di < store_.devices.size(); ++di) {
    DeviceSim& d = devices_[di];
    if (d.frozen) continue;  // a stale device: values hold
    store::Device& dev = store_.devices[di];
    for (size_t i = 0; i < d.tags.size(); ++i) {
      TagSim& ts = d.tags[i];
      if (ts.entry < 0 || int32_t(nowMs - ts.next) < 0) continue;
      const sim::Entry& e = d.cfg.entries[size_t(ts.entry)];
      ts.next = nowMs + e.updateMs;
      if (e.mode == sim::Mode::Echo || dev.values[i].overridden) continue;
      const double v = compute(dev.profile->tags[i], e, ts, nowMs);  // outside the lock
      if (std::isnan(v)) continue;
      mb::Guard g(store_.lock);
      dev.setValue(i, v);
    }
  }
}

void Engine::resume(size_t device, size_t tag) {
  if (device >= store_.devices.size() || tag >= store_.devices[device].values.size()) return;
  mb::Guard g(store_.lock);
  store_.devices[device].values[tag].overridden = false;
  if (device < devices_.size() && tag < devices_[device].tags.size()) devices_[device].tags[tag].next = 0;
}

void Engine::resumeAll(size_t device) {
  if (device >= store_.devices.size()) return;
  for (size_t i = 0; i < store_.devices[device].values.size(); ++i) resume(device, i);
}

void Engine::setFrozen(size_t device, bool f) {
  if (devices_.size() <= device) devices_.resize(device + 1);
  devices_[device].frozen = f;
}
bool Engine::frozen(size_t device) const { return device < devices_.size() && devices_[device].frozen; }

const sim::Entry* Engine::entry(size_t device, size_t tag) const {
  if (device >= devices_.size() || tag >= devices_[device].tags.size()) return nullptr;
  const int16_t k = devices_[device].tags[tag].entry;
  return k < 0 ? nullptr : &devices_[device].cfg.entries[size_t(k)];
}
}  // namespace simeng
