#include "toml_chunks.h"

namespace tomlx {
namespace {
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Number of `"""` or `'''` delimiters on a line (each one toggles multi-line string state).
int tripleQuotes(std::string_view line) {
  int n = 0;
  for (size_t i = 0; i + 2 < line.size(); ++i) {
    if ((line[i] == '"' || line[i] == '\'') && line[i + 1] == line[i] && line[i + 2] == line[i]) {
      ++n;
      i += 2;
    }
  }
  return n;
}

// "[name]" or "[[name]]" with an optional trailing comment. Returns false if `line`
// is not a header; sets `bad` when it starts like one but is malformed.
bool header(std::string_view line, std::string& name, bool& array, bool& bad) {
  bad = false;
  line = trim(line);
  if (line.empty() || line[0] != '[') return false;
  array = line.size() > 1 && line[1] == '[';
  const size_t open = array ? 2 : 1;
  const std::string_view close = array ? "]]" : "]";
  const size_t end = line.find(close, open);
  if (end == std::string_view::npos) { bad = true; return true; }
  std::string_view rest = trim(line.substr(end + close.size()));
  if (!rest.empty() && rest[0] != '#') { bad = true; return true; }
  name = std::string(trim(line.substr(open, end - open)));
  if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
  if (name.empty()) bad = true;
  return true;
}
}  // namespace

bool split(std::string_view text, std::vector<Chunk>& out, std::string& error) {
  out.clear();
  out.push_back(Chunk{});
  size_t pos = 0, line = 1, bodyStart = 0;
  bool inMultiline = false;
  while (pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    const size_t end = eol == std::string_view::npos ? text.size() : eol;
    const std::string_view l = text.substr(pos, end - pos);
    std::string name;
    bool array = false, bad = false;
    if (!inMultiline && header(l, name, array, bad)) {
      if (bad) {
        error = "TOML syntax error at line " + std::to_string(line) + ": malformed table header";
        return false;
      }
      out.back().body = text.substr(bodyStart, pos - bodyStart);
      Chunk c;
      c.name = name;
      c.array = array;
      c.line = line + 1;
      out.push_back(c);
      bodyStart = eol == std::string_view::npos ? text.size() : eol + 1;
    } else if (tripleQuotes(l) % 2) {
      inMultiline = !inMultiline;
    }
    if (eol == std::string_view::npos) break;
    pos = eol + 1;
    ++line;
  }
  out.back().body = text.substr(bodyStart);
  return true;
}

bool parse(const Chunk& c, toml::table& out, std::string& error) {
  auto r = toml::parse(c.body);
  if (!r) {
    const auto& e = r.error();
    error = "TOML syntax error at line " + std::to_string(c.line + e.source().begin.line - 1) + ": " +
            std::string(e.description());
    return false;
  }
  out = std::move(r.table());
  return true;
}
}  // namespace tomlx
