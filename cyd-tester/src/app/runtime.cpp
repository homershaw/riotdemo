#include "runtime.h"
#include <vector>

namespace runtime {
namespace {
store::Store valueStore;
simeng::Engine engine(valueStore);

struct PendingSim {
  uint8_t unitId;
  sim::Config cfg;
};
SemaphoreHandle_t pendingLock = nullptr;
std::vector<PendingSim> pending;
std::vector<std::pair<uint8_t, int>> pendingResumes;  // unit, tag (-1 = all)
SemaphoreHandle_t simLock = nullptr;  // engine.configure() vs. reads of its entries from other tasks
}  // namespace

store::Store& values() { return valueStore; }
simeng::Engine& simulation() { return engine; }

void begin() {
  pendingLock = xSemaphoreCreateMutex();
  simLock = xSemaphoreCreateMutex();
  for (const auto& d : profiles::devices()) {
    if (d.status != profile::Status::Ok) continue;
    valueStore.devices.emplace_back(d.unitId, d.enabled, d.profile);
    auto& dev = valueStore.devices.back();
    for (size_t i = 0; i < d.profile->tags.size(); ++i) {
      const auto& tag = d.profile->tags[i];
      dev.setValue(i, sim::startValue(tag, d.sim.find(tag.id)));
    }
    engine.configure(valueStore.devices.size() - 1, d.sim, millis());
  }
#if HAS_LDR
  engine.ldr = [] { return double(analogRead(PIN_LDR)); };
#endif
}

void service() {
  std::vector<PendingSim> work;
  std::vector<std::pair<uint8_t, int>> resumes;
  if (pendingLock) {
    xSemaphoreTake(pendingLock, portMAX_DELAY);
    work.swap(pending);
    resumes.swap(pendingResumes);
    xSemaphoreGive(pendingLock);
  }
  for (const auto& [unit, tag] : resumes) {
    const int i = deviceIndex(unit);
    if (i < 0) continue;
    if (tag < 0) engine.resumeAll(size_t(i));
    else engine.resume(size_t(i), size_t(tag));
  }
  for (const auto& p : work) {
    const int i = deviceIndex(p.unitId);
    if (i < 0) continue;
    xSemaphoreTake(simLock, portMAX_DELAY);
    engine.configure(size_t(i), p.cfg, millis());
    xSemaphoreGive(simLock);
  }
  engine.tick(millis());
}

void queueResume(uint8_t unitId, int tag) {
  if (!pendingLock) return;
  xSemaphoreTake(pendingLock, portMAX_DELAY);
  pendingResumes.emplace_back(unitId, tag);
  xSemaphoreGive(pendingLock);
}

bool simEntry(int device, size_t tag, sim::Entry& out) {
  if (device < 0 || !simLock) return false;
  xSemaphoreTake(simLock, portMAX_DELAY);
  const sim::Entry* e = engine.entry(size_t(device), tag);
  if (e) out = *e;
  xSemaphoreGive(simLock);
  return e != nullptr;
}

int deviceIndex(uint8_t unitId) {
  for (size_t i = 0; i < valueStore.devices.size(); ++i)
    if (valueStore.devices[i].unitId == unitId) return int(i);
  return -1;
}

const profiles::LoadedDevice* loaded(uint8_t unitId) {
  for (const auto& d : profiles::devices())
    if (d.unitId == unitId) return &d;
  return nullptr;
}

void queueSimApply(uint8_t unitId, const sim::Config& cfg) {
  if (!pendingLock) return;
  xSemaphoreTake(pendingLock, portMAX_DELAY);
  pending.push_back({unitId, cfg});
  xSemaphoreGive(pendingLock);
}
}  // namespace runtime
