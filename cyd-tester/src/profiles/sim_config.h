#pragma once
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include "profile.h"

// Simulation sidecar <profile_id>.sim.toml (PROMPT.md section 6). Portable C++17.
namespace sim {
enum class Mode : uint8_t { Static, Ramp, Sine, Random, Step, Toggle, Counter, Echo, Ldr };

struct Entry {
  std::string tag;
  Mode mode = Mode::Static;
  double initial = NAN;  // NAN = not set
  double min = NAN, max = NAN;
  double periodS = 60;
  double noise = 0;
  uint32_t updateMs = 1000;
};

struct Config {
  std::string profileId;
  std::vector<Entry> entries;
  std::vector<std::string> warnings;
  const Entry* find(std::string_view tag) const;
};

const char* toString(Mode m);
// Returns false (with `error`) only for TOML syntax errors or a missing profile_id.
// Entries naming tags the profile doesn't have are dropped with a warning.
bool parse(std::string_view text, const profile::Profile& p, Config& out, std::string& error);
// Starting value: `initial`, else the middle of min/max, else 0 (bools: 0/1).
double startValue(const profile::Tag& t, const Entry* e);

// Clean TOML for a sim config (the web UI's Save button).
std::string toToml(const Config& c);
// "Generate sim template": an entry for every mapped tag. Writable tags echo,
// bits and code/config registers stay static, and measurements get a sine over a
// placeholder range that is clearly marked for the user to replace.
std::string templateToml(const profile::Profile& p);
}  // namespace sim
