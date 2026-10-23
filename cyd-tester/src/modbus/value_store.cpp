#include "value_store.h"
#include <algorithm>
#include "codec.h"

#ifdef ARDUINO
#include <Arduino.h>
static uint32_t arduinoMillis() { return millis(); }
uint32_t (*store::nowMs)() = arduinoMillis;
#else
#include <chrono>
static uint32_t steadyMillis() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
uint32_t (*store::nowMs)() = steadyMillis;
#endif

namespace store {
using profile::DataType;

Device::Device(uint8_t unit, bool en, std::shared_ptr<const profile::Profile> p)
    : unitId(unit), enabled(en), profile(std::move(p)) {
  values.resize(profile->tags.size());
  for (size_t i = 0; i < profile->tags.size(); ++i) {
    const auto& t = profile->tags[i];
    if (!t.mapped) continue;
    for (uint8_t w = 0; w < profile::wordCount(t.type); ++w)
      slots_[int(t.space)].push_back({uint16_t(t.wire + w), uint16_t(i), w});
    setValue(i, 0.0);
  }
  for (auto& s : slots_) {
    // Later tags win on overlap (the parser warns about it); stable sort keeps profile order.
    std::stable_sort(s.begin(), s.end(), [](const Slot& a, const Slot& b) { return a.wire < b.wire; });
    std::vector<Slot> unique;
    for (const auto& slot : s) {
      if (!unique.empty() && unique.back().wire == slot.wire) unique.back() = slot;
      else unique.push_back(slot);
    }
    s.swap(unique);
  }
}

const Slot* Device::find(profile::Space s, uint16_t wire) const {
  const auto& v = slots_[int(s)];
  auto it = std::lower_bound(v.begin(), v.end(), wire, [](const Slot& a, uint16_t w) { return a.wire < w; });
  return it != v.end() && it->wire == wire ? &*it : nullptr;
}

void Device::setValue(size_t i, double v) {
  const auto& t = profile->tags[i];
  auto& tv = values[i];
  if (t.type == DataType::Bool) {
    tv.value = v != 0.0 ? 1.0 : 0.0;
    tv.words[0] = v != 0.0 ? 1 : 0;
    return;
  }
  codec::encode(t, v, tv.words);
  tv.value = codec::decode(t, tv.words);  // what a client actually reads back (rounding, clamping)
}

void Device::setWords(size_t i, const uint16_t* words) {
  const auto& t = profile->tags[i];
  auto& tv = values[i];
  const uint8_t n = profile::wordCount(t.type);
  for (uint8_t w = 0; w < n; ++w) tv.words[w] = words[w];
  tv.value = t.type == DataType::Bool ? (words[0] ? 1.0 : 0.0) : codec::decode(t, tv.words);
  tv.overridden = true;
  tv.lastWriteMs = nowMs();
  ++tv.writeCount;
}

void Device::override(size_t i, double v) {
  setValue(i, v);
  values[i].overridden = true;
}

void Device::noteRead(size_t i, uint32_t now) {
  auto& tv = values[i];
  if (tv.readCount) {
    const uint32_t gap = now - tv.lastReadMs;
    // Smooth over a few polls so one late request doesn't swing the figure.
    tv.pollIntervalMs = tv.pollIntervalMs ? (tv.pollIntervalMs * 3 + gap) / 4 : gap;
  }
  ++tv.readCount;
  tv.lastReadMs = now;
}

Device* Store::find(uint8_t unitId) {
  for (auto& d : devices)
    if (d.enabled && d.unitId == unitId) return &d;
  return nullptr;
}

Device* Store::firstEnabled() {
  for (auto& d : devices)
    if (d.enabled) return &d;
  return nullptr;
}
}  // namespace store
