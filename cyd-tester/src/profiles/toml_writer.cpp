#include "toml_writer.h"
#include <math.h>

TomlWriter& TomlWriter::comment(const char* text) { out_ += "# "; out_ += text; out_ += '\n'; return *this; }
TomlWriter& TomlWriter::blank() { out_ += '\n'; return *this; }
TomlWriter& TomlWriter::table(const char* name) { out_ += "\n["; out_ += name; out_ += "]\n"; return *this; }
TomlWriter& TomlWriter::arrayTable(const char* name) { out_ += "\n[["; out_ += name; out_ += "]]\n"; return *this; }

TomlWriter& TomlWriter::kv(const char* key, const char* value) {
  out_ += key; out_ += " = "; out_ += quote(value); out_ += '\n'; return *this;
}
TomlWriter& TomlWriter::kv(const char* key, long value) {
  out_ += key; out_ += " = "; out_ += String(value); out_ += '\n'; return *this;
}
TomlWriter& TomlWriter::kv(const char* key, bool value) {
  out_ += key; out_ += value ? " = true\n" : " = false\n"; return *this;
}
TomlWriter& TomlWriter::kv(const char* key, double value) {
  out_ += key; out_ += " = ";
  if (isnan(value)) out_ += "nan";
  else if (isinf(value)) out_ += value > 0 ? "inf" : "-inf";
  else {
    char buf[32];
    snprintf(buf, sizeof buf, "%.9g", value);
    out_ += buf;
    // TOML needs a '.' or exponent to keep the value a float.
    if (!strpbrk(buf, ".eE")) out_ += ".0";
  }
  out_ += '\n';
  return *this;
}

String TomlWriter::quote(const char* value) {
  String q = "\"";
  for (const char* p = value; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '"': q += "\\\""; break;
      case '\\': q += "\\\\"; break;
      case '\n': q += "\\n"; break;
      case '\r': q += "\\r"; break;
      case '\t': q += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) { char esc[8]; snprintf(esc, sizeof esc, "\\u%04x", c); q += esc; }
        else q += c;
    }
  }
  return q + "\"";
}
