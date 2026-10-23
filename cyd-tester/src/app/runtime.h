#pragma once
#include <Arduino.h>
#include "../modbus/value_store.h"
#include "../profiles/profile_store.h"
#include "../profiles/sim_engine.h"

// The live emulation: value store, simulation engine and the loaded devices,
// shared by the Modbus server, the web API and the screen.
namespace runtime {
store::Store& values();
simeng::Engine& simulation();

// Builds the store from profiles::devices() and configures the simulation.
void begin();
// Main loop: ticks the simulation and applies changes queued by the web API.
void service();

// Store index for an enabled or disabled device with this unit ID, or -1.
int deviceIndex(uint8_t unitId);
// The loaded device (profile, sim, files) for a unit ID, or nullptr.
const profiles::LoadedDevice* loaded(uint8_t unitId);

// From AsyncTCP's task: replace a device's sim config; applied on the next service().
void queueSimApply(uint8_t unitId, const sim::Config& cfg);
// From AsyncTCP's task: hand a tag (or every tag, tag < 0) back to the simulation.
void queueResume(uint8_t unitId, int tag);
// Copy of the sim entry driving a tag (safe from any task). False if the tag is static.
bool simEntry(int device, size_t tag, sim::Entry& out);
}  // namespace runtime
