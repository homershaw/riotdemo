#pragma once
#include <Arduino.h>

// Minimal, stable TOML emitter for files the unit writes (config, calibration,
// sims). Parsing uses toml++; writing stays hand-formatted so output is clean.
class TomlWriter {
 public:
  TomlWriter& comment(const char* text);
  TomlWriter& blank();
  TomlWriter& table(const char* name);
  TomlWriter& arrayTable(const char* name);
  TomlWriter& kv(const char* key, const char* value);
  TomlWriter& kv(const char* key, const String& value) { return kv(key, value.c_str()); }
  TomlWriter& kv(const char* key, long value);
  TomlWriter& kv(const char* key, int value) { return kv(key, long(value)); }
  TomlWriter& kv(const char* key, double value);
  TomlWriter& kv(const char* key, bool value);
  const String& str() const { return out_; }

  static String quote(const char* value);

 private:
  String out_;
};
