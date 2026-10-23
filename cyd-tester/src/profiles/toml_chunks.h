#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <toml++/toml.hpp>

// Parses a TOML document one table at a time, so peak memory is one table's DOM
// rather than the whole file's. A 79-tag profile needs ~138 KB as one toml++
// document but only a few KB per [[tags]] entry, which matters on a board without PSRAM.
// Splits at [table] and [[array]] header lines, ignoring lines inside multi-line strings.
namespace tomlx {
struct Chunk {
  std::string name;       // header name, "" for the keys before the first header
  bool array = false;     // [[name]]
  std::string_view body;  // text between this header and the next
  size_t line = 1;        // 1-based line of the body's first line in the whole file
};

// False (with `error`) for a malformed header line.
bool split(std::string_view text, std::vector<Chunk>& out, std::string& error);

// Parses one chunk's body. On a syntax error returns false with the error
// reported against the line number in the whole file.
bool parse(const Chunk& c, toml::table& out, std::string& error);
}  // namespace tomlx
