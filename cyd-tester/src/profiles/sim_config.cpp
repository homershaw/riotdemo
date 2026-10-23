#include "sim_config.h"
#include <algorithm>
#include "toml_chunks.h"
#include <cstdio>
#include <toml++/toml.hpp>

namespace sim {
const char* toString(Mode m) {
  switch (m) {
    case Mode::Static: return "static";
    case Mode::Ramp: return "ramp";
    case Mode::Sine: return "sine";
    case Mode::Random: return "random";
    case Mode::Step: return "step";
    case Mode::Toggle: return "toggle";
    case Mode::Counter: return "counter";
    case Mode::Echo: return "echo";
    case Mode::Ldr: return "ldr";
  }
  return "?";
}

const Entry* Config::find(std::string_view tag) const {
  for (const auto& e : entries)
    if (e.tag == tag) return &e;
  return nullptr;
}

namespace {
bool parseMode(const std::string& s, Mode& m) {
  static const std::pair<const char*, Mode> names[] = {
      {"static", Mode::Static}, {"ramp", Mode::Ramp}, {"sine", Mode::Sine}, {"random", Mode::Random}, {"step", Mode::Step},
      {"toggle", Mode::Toggle}, {"counter", Mode::Counter}, {"echo", Mode::Echo}, {"ldr", Mode::Ldr}};
  for (const auto& [n, v] : names)
    if (s == n) { m = v; return true; }
  return false;
}

bool number(const toml::table& t, const char* key, double& out) {
  if (auto v = t[key].value<double>()) { out = *v; return true; }  // toml++ converts integers too
  return false;
}
}  // namespace

bool parse(std::string_view text, const profile::Profile& p, Config& out, std::string& error) {
  out = Config{};
  // One table at a time (see toml_chunks.h): a generated sim has one [[sim]] per tag.
  std::vector<tomlx::Chunk> chunks;
  if (!tomlx::split(text, chunks, error)) return false;
  toml::table root;
  if (!tomlx::parse(chunks[0], root, error)) return false;
  for (const auto& [key, node] : root) {
    const std::string_view k = key.str();
    if (k != "schema_version" && k != "profile_id") out.warnings.push_back("unknown key '" + std::string(k) + "' ignored");
  }
  for (size_t i = 1; i < chunks.size(); ++i)
    if (!(chunks[i].array && chunks[i].name == "sim"))
      out.warnings.push_back("unknown table " + std::string(chunks[i].array ? "[[" : "[") + chunks[i].name +
                             (chunks[i].array ? "]]" : "]") + " ignored");
  out.profileId = root["profile_id"].value_or<std::string>("");
  if (out.profileId.empty()) { error = "profile_id is missing"; return false; }
  if (out.profileId != p.profileId)
    out.warnings.push_back("profile_id \"" + out.profileId + "\" does not match the profile (\"" + p.profileId + "\")");

  size_t index = 0;
  for (size_t ci = 1; ci < chunks.size(); ++ci) {
    if (!chunks[ci].array || chunks[ci].name != "sim") continue;
    std::string where = "sim[" + std::to_string(index++) + "]";
    toml::table simTable;  // one entry's DOM at a time
    if (!tomlx::parse(chunks[ci], simTable, error)) return false;
    const toml::table* t = &simTable;
    Entry e;
    e.tag = (*t)["tag"].value_or<std::string>("");
    if (e.tag.empty()) { out.warnings.push_back(where + " has no tag; skipped"); continue; }
    where = "sim '" + e.tag + "': ";
    const profile::Tag* tag = p.find(e.tag);
    if (!tag) { out.warnings.push_back(where + "no such tag in the profile; skipped"); continue; }
    if (out.find(e.tag)) { out.warnings.push_back(where + "duplicate entry; skipped"); continue; }
    for (const auto& [key, n] : *t) {
      const std::string_view k = key.str();
      static const char* known[] = {"tag", "mode", "initial", "min", "max", "period_s", "noise", "update_ms"};
      if (std::none_of(std::begin(known), std::end(known), [&](const char* x) { return k == x; }))
        out.warnings.push_back(where + "unknown key '" + std::string(k) + "' ignored");
    }
    const std::string mode = (*t)["mode"].value_or<std::string>("static");
    if (!parseMode(mode, e.mode)) { out.warnings.push_back(where + "unknown mode \"" + mode + "\"; using static"); e.mode = Mode::Static; }
    number(*t, "initial", e.initial);
    number(*t, "min", e.min);
    number(*t, "max", e.max);
    if (!std::isnan(e.min) && !std::isnan(e.max) && e.min > e.max) {
      std::swap(e.min, e.max);
      out.warnings.push_back(where + "min is above max; swapped");
    }
    double v;
    if (number(*t, "period_s", v)) {
      if (v > 0) e.periodS = v;
      else out.warnings.push_back(where + "period_s must be above 0; using 60");
    }
    if (number(*t, "noise", v)) e.noise = v < 0 ? -v : v;
    if (number(*t, "update_ms", v)) {
      if (v >= 50) e.updateMs = uint32_t(v);
      else out.warnings.push_back(where + "update_ms below 50; using 50");
      if (v < 50) e.updateMs = 50;
    }
    const bool needsRange = e.mode == Mode::Ramp || e.mode == Mode::Sine || e.mode == Mode::Random;
    if (needsRange && (std::isnan(e.min) || std::isnan(e.max)))
      out.warnings.push_back(where + std::string(toString(e.mode)) + " needs min and max; the value stays static");
    out.entries.push_back(e);
  }
  return true;
}

namespace {
std::string quote(const std::string& s) {
  std::string q = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { q += '\\'; q += c; }
    else if (c == '\n') q += "\\n";
    else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); q += b; }
    else q += c;
  }
  return q + "\"";
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.9g", v);
  std::string s = b;
  if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // keep it a TOML float
  return s;
}

// An unscaled integer with no units holds a code or setting, not a measurement.
bool looksLikeCode(const profile::Tag& t) {
  return t.type != profile::DataType::Float32 && t.units.empty() && t.scale == 1.0f;
}
}  // namespace

std::string toToml(const Config& c) {
  std::string o = "schema_version = 1\nprofile_id = " + quote(c.profileId) + "\n";
  for (const auto& e : c.entries) {
    o += "\n[[sim]]\ntag = " + quote(e.tag) + "\nmode = " + quote(toString(e.mode)) + "\n";
    if (!std::isnan(e.initial)) o += "initial = " + num(e.initial) + "\n";
    if (!std::isnan(e.min)) o += "min = " + num(e.min) + "\n";
    if (!std::isnan(e.max)) o += "max = " + num(e.max) + "\n";
    if (e.mode != Mode::Static && e.mode != Mode::Echo && e.mode != Mode::Counter && e.mode != Mode::Random && e.mode != Mode::Ldr)
      o += "period_s = " + num(e.periodS) + "\n";
    if (e.noise > 0) o += "noise = " + num(e.noise) + "\n";
    if (e.mode != Mode::Echo) o += "update_ms = " + std::to_string(e.updateMs) + "\n";
  }
  return o;
}

std::string templateToml(const profile::Profile& p) {
  std::string o = "# Simulation sidecar for " + p.profileId + " (" + p.vendor + " " + p.model + ")\n";
  o += "# Generated template. Values are in ENGINEERING units; the unit applies scale and encoding.\n";
  o += "# Ranges marked PLACEHOLDER are not in the profile: replace them with realistic limits.\n";
  o += "schema_version = 1\nprofile_id = " + quote(p.profileId) + "\n";
  for (const auto& t : p.tags) {
    if (!t.mapped) continue;
    o += "\n[[sim]]\n";
    if (!t.name.empty() && t.name != t.id) o += "# " + t.name + (t.units.empty() ? "" : " [" + t.units + "]") + "\n";
    o += "tag = " + quote(t.id) + "\n";
    if (t.writable) {
      o += "mode = \"echo\"        # setpoint: changes only when the RIOT writes it\ninitial = 0\n";
    } else if (t.type == profile::DataType::Bool) {
      o += "mode = \"static\"\ninitial = 0\nupdate_ms = 1000\n";
    } else if (looksLikeCode(t)) {
      o += "mode = \"static\"      # code / configuration register\ninitial = 0\nupdate_ms = 1000\n";
    } else {
      o += "mode = \"sine\"\nmin = 0.0           # PLACEHOLDER range" + (t.units.empty() ? std::string() : " in " + t.units) +
           "\nmax = 100.0         # PLACEHOLDER\nperiod_s = 60\nupdate_ms = 1000\n";
    }
  }
  return o;
}

double startValue(const profile::Tag& t, const Entry* e) {
  double v = 0;
  if (e && !std::isnan(e->initial)) v = e->initial;
  else if (e && !std::isnan(e->min) && !std::isnan(e->max)) v = (e->min + e->max) / 2;
  if (t.type == profile::DataType::Bool) v = v != 0 ? 1 : 0;
  return v;
}
}  // namespace sim
