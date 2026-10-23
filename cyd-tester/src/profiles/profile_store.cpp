#include "profile_store.h"
#include <new>
#include <LittleFS.h>
#include "unit_config.h"

// Embedded from data/ by board_build.embed_txtfiles (NUL-terminated).
extern const char builtinProfile[] asm("_binary_data_profiles_test_bench_toml_start");
extern const char builtinSim[] asm("_binary_data_sims_test_bench_sim_toml_start");
extern const char builtinM31[] asm("_binary_data_profiles_riot_m31_emulation_toml_start");
extern const char builtinM31Sim[] asm("_binary_data_sims_riot_m31_emulation_sim_toml_start");

namespace profiles {
namespace {
std::vector<LoadedDevice> loaded;

void writeIfMissing(const char* dir, const char* path, const char* text) {
  if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
  if (LittleFS.exists(path)) return;
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) { Serial.printf("Could not create %s\n", path); return; }
  f.print(text);
  f.close();
  Serial.printf("Installed built-in %s\n", path);
}

bool readFile(const String& path, String& out) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  out = f.readString();
  f.close();
  return true;
}
}  // namespace

void installBuiltins() {
  writeIfMissing("/profiles", "/profiles/test_bench.toml", builtinProfile);
  writeIfMissing("/sims", "/sims/test_bench.sim.toml", builtinSim);
  writeIfMissing("/profiles", "/profiles/riot_m31_emulation.toml", builtinM31);
  writeIfMissing("/sims", "/sims/riot_m31_emulation.sim.toml", builtinM31Sim);
}

size_t loadDevices() {
  loaded.clear();
  size_t ok = 0;
  for (const auto& d : unitcfg::current().devices) {
    LoadedDevice dev;
    dev.unitId = d.unitId;
    dev.enabled = d.enabled;
    dev.file = d.profile;
    String text;
    if (!readFile("/profiles/" + d.profile, text)) {
      dev.message = "profile file /profiles/" + d.profile + " not found";
      loaded.push_back(std::move(dev));
      continue;
    }
    profile::ParseResult r;
    try {
      r = profile::parse(std::string_view(text.c_str(), text.length()), *dev.profile);
    } catch (const std::bad_alloc&) {
      r = {profile::Status::Error, "not enough memory to load this profile"};
      *dev.profile = profile::Profile{};
    }
    text = String();  // the parsed structs are all that is kept
    dev.status = r.status;
    dev.message = r.message.c_str();
    if (r.status == profile::Status::Ok) {
      ++ok;
      // The sidecar is optional: unit.toml names it, else <profile_id>.sim.toml is tried.
      dev.simFile = d.sim.length() ? d.sim : String(dev.profile->profileId.c_str()) + ".sim.toml";
      String simText;
      if (readFile("/sims/" + dev.simFile, simText)) {
        std::string error;
        if (!sim::parse(std::string_view(simText.c_str(), simText.length()), *dev.profile, dev.sim, error))
          dev.simMessage = error.c_str();
#if !HAS_LDR
        for (const auto& e : dev.sim.entries)
          if (e.mode == sim::Mode::Ldr)
            dev.sim.warnings.push_back("sim '" + e.tag + "': no light sensor on this board; the value stays static");
#endif
      } else if (d.sim.length()) {
        dev.simMessage = "sim file /sims/" + dev.simFile + " not found";
      } else {
        dev.simFile = "";
      }
    }
    loaded.push_back(std::move(dev));
  }
  for (const auto& d : loaded) {
    if (d.status != profile::Status::Ok) {
      Serial.printf("Unit %u: %s NOT loaded: %s\n", d.unitId, d.file.c_str(), d.message.c_str());
      continue;
    }
    const auto& p = *d.profile;
    Serial.printf("Unit %u: %s (%s %s) %u tags, %u mapped, address %s, %u warning(s)%s\n", d.unitId, d.file.c_str(),
                  p.vendor.c_str(), p.model.c_str(), unsigned(p.tags.size()), unsigned(p.mappedCount()),
                  profile::toString(p.addressMode), unsigned(p.warnings.size()), d.enabled ? "" : " [disabled]");
    for (const auto& w : p.warnings) Serial.printf("    - %s\n", w.c_str());
    if (d.simMessage.length()) Serial.printf("    sim: %s\n", d.simMessage.c_str());
    else if (d.simFile.length())
      Serial.printf("    sim: %s, %u entries, %u warning(s)\n", d.simFile.c_str(), unsigned(d.sim.entries.size()),
                    unsigned(d.sim.warnings.size()));
    for (const auto& w : d.sim.warnings) Serial.printf("    - %s\n", w.c_str());
  }
  return ok;
}

const std::vector<LoadedDevice>& devices() { return loaded; }
}  // namespace profiles
