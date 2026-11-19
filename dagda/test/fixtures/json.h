//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// A small JSON reader for known-answer files, written by hand rather than adding a
// library: what they hold is objects, arrays, plain strings, whole numbers, booleans
// and null. A number keeps its text, so a u64 survives. Throws std::runtime_error on
// anything else. Test only.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dagda::test {

class Json {
 public:
  enum class Kind : uint8_t { null, boolean, number, string, array, object };

  static Json parse(std::string_view text) {
    size_t at = 0;
    auto out = value(text, at);
    space(text, at);
    if (at != text.size()) throw std::runtime_error("json: text after the value");
    return out;
  }
  static Json read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("json: cannot read " + path.string());
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    return parse(text);
  }

  Kind kind() const noexcept { return _kind; }
  bool is_null() const noexcept { return _kind == Kind::null; }
  bool boolean() const { return need(Kind::boolean)._text == "true"; }
  uint64_t u64() const { return std::strtoull(need(Kind::number)._text.c_str(), nullptr, 10); }
  const std::string& string() const { return need(Kind::string)._text; }
  const std::vector<Json>& array() const { return need(Kind::array)._items; }
  const Json& operator[](const std::string& key) const {
    const auto found = need(Kind::object)._fields.find(key);
    if (found == _fields.end()) throw std::runtime_error("json: no field " + key);
    return found->second;
  }
  bool has(const std::string& key) const { return need(Kind::object)._fields.contains(key); }

 private:
  const Json& need(Kind kind) const {
    if (_kind != kind) throw std::runtime_error("json: not of the kind asked for");
    return *this;
  }
  static void space(std::string_view text, size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
  }
  static void expect(std::string_view text, size_t& at, char what) {
    if (at >= text.size() || text[at] != what) throw std::runtime_error(std::string("json: expected ") + what);
    ++at;
  }
  static std::string string_of(std::string_view text, size_t& at) {
    expect(text, at, '"');
    std::string out;
    while (at < text.size() && text[at] != '"') {
      if (text[at] != '\\') {
        out += text[at++];
        continue;
      }
      ++at;
      if (at >= text.size()) break;
      const char escaped = text[at++];
      switch (escaped) {
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          out += '\r';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'u': {
          // Kept as one byte: known-answer files name bytes this way, for reading
          // only.
          if (at + 4 > text.size()) throw std::runtime_error("json: short escape");
          out += static_cast<char>(std::strtoul(std::string(text.substr(at, 4)).c_str(), nullptr, 16));
          at += 4;
          break;
        }
        default:
          out += escaped;
      }
    }
    expect(text, at, '"');
    return out;
  }
  static Json value(std::string_view text, size_t& at) {
    space(text, at);
    if (at >= text.size()) throw std::runtime_error("json: ran out");
    Json out;
    const char first = text[at];
    if (first == '{') {
      out._kind = Kind::object;
      ++at;
      space(text, at);
      if (at < text.size() && text[at] == '}') {
        ++at;
        return out;
      }
      for (;;) {
        space(text, at);
        auto key = string_of(text, at);
        space(text, at);
        expect(text, at, ':');
        out._fields.emplace(std::move(key), value(text, at));
        space(text, at);
        if (at < text.size() && text[at] == ',') {
          ++at;
          continue;
        }
        expect(text, at, '}');
        return out;
      }
    }
    if (first == '[') {
      out._kind = Kind::array;
      ++at;
      space(text, at);
      if (at < text.size() && text[at] == ']') {
        ++at;
        return out;
      }
      for (;;) {
        out._items.push_back(value(text, at));
        space(text, at);
        if (at < text.size() && text[at] == ',') {
          ++at;
          continue;
        }
        expect(text, at, ']');
        return out;
      }
    }
    if (first == '"') {
      out._kind = Kind::string;
      out._text = string_of(text, at);
      return out;
    }
    const auto start = at;
    while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ']' && text[at] != ' ' && text[at] != '\n' && text[at] != '\r') ++at;
    out._text = std::string(text.substr(start, at - start));
    if (out._text == "null") {
      out._kind = Kind::null;
    } else if (out._text == "true" || out._text == "false") {
      out._kind = Kind::boolean;
    } else if (!out._text.empty() && (out._text[0] == '-' || (out._text[0] >= '0' && out._text[0] <= '9'))) {
      out._kind = Kind::number;
    } else {
      throw std::runtime_error("json: bad value " + out._text);
    }
    return out;
  }

  Kind _kind = Kind::null;
  std::string _text;
  std::vector<Json> _items;
  std::map<std::string, Json> _fields;
};

}  // namespace dagda::test
