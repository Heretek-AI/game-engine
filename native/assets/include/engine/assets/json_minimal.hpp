// json_minimal.hpp — P5-03/P2-06 shared minimal JSON value model + parser.
//
// Two modules need a tiny dependency-free JSON reader over a closed schema:
//   * P2-06 scene files (native/scene/src/scene_json_read.cpp)
//   * P5-03 animator state-machine graphs
//     (native/assets/src/animator_state_machine.cpp)
//
// Both used to carry their own recursive-descent copy (Value + Parser with
// depth-capped containers, strings with escapes, numbers, true/false/null),
// which trips the Sonar duplication gate on every PR touching either copy.
// One shared header kills the mirror at the source: ~90 lines of parser
// skeleton (Value, Parser::Enter/skip_ws/parse_value/parse_string/
// parse_literal/parse_array/parse_object + ParseCommaList driver) live here
// exactly once; each TU keeps only its schema-specific conversion layer
// (ToDouble/ToFloatFinite/FindNamedIndex + graph/scene assembly).
//
// Design notes:
//   * Header-only + internal linkage. Everything is `inline` or a template
//     inside `engine::json_min` detail namespaces so both TUs can include
//     it without ODR or export-macro issues (no ENGINE_*_API — the parser
//     never crosses a DLL boundary; only each module's own API does).
//   * Same fail-closed contract as the P2-06 original: `ok=false` on any
//     malformed input (bad JSON, over depth cap, unterminated string,
//     trailing garbage); `Depth counts only nested containers`.
//   * Two Value flavors: scene files need True/False/Null distinctly and
//     treat bools as Num("1"/"0") via the writer contract; animator graphs
//     need True/False kinds + raw Num text for finite-float validation.
//     The shared model carries the union (Null, Num, True, False, Str, Arr,
//     Obj) — each consumer only reads the kinds its schema uses. Bool
//     normalization (true->Num("1")) stays in the scene TU, not here.
//   * Numbers stay raw text (`str`); validation/conversion is the
//     consumer's job (animator: ToDouble+finite+float-range; scene:
//     ParseNum locale-independent double parser).
//   * UTF-8 decoding keeps the simpler BMP-only form (P5-03 animator
//     variant): \uXXXX -> UTF-8, lone surrogates fail. The scene variant's
//     surrogate-pair combining is a scene-TU extension point, not shared.
//   * No malloc/new on any frame path: the parser allocates only at
//     load time (std::string/std::vector growth while parsing); consumers
//     reserve/fix capacities after parsing per AGENTS.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::json_min {

// --- JSON value model ------------------------------------------------------
// Str: unescaped; Num: raw text (validated at conversion by the consumer).
struct Value {
  enum class Kind : std::uint8_t { Null, Num, True, False, Str, Arr, Obj };
  Kind kind{Kind::Null};
  std::string str;
  std::vector<Value> items;  // Arr
  // Obj: parallel keys + values (insertion order; linear lookup is fine —
  // objects are small: components per entity, fields per component,
  // states/transitions/conditions per graph).
  std::vector<std::string> keys;
  std::vector<Value> values;

  [[nodiscard]] auto find(std::string_view key) const -> const Value* {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == key) {
        return &values[i];
      }
    }
    return nullptr;
  }
};

struct Parser {
  // Depth cap: hostile 100k-'[' input cannot overflow the stack
  // ("never crash" rule). Counts only nested containers; siblings reuse it.
  static constexpr int kMaxDepth = 64;
  const char* cur{nullptr};
  const char* end{nullptr};
  bool ok{true};
  int depth{0};

  auto Enter() -> bool {
    if (depth >= kMaxDepth) {
      ok = false;
      return false;
    }
    ++depth;
    return true;
  }

  auto skip_ws() -> void {
    while (cur < end &&
           (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) {
      ++cur;
    }
  }

  auto parse_value(Value& out) -> void {
    skip_ws();
    if (cur >= end) {
      ok = false;
      return;
    }
    switch (*cur) {
      case '{': parse_object(out); break;
      case '[': parse_array(out); break;
      case '"':
        out.kind = Value::Kind::Str;
        parse_string(out.str);
        break;
      default: parse_literal(out); break;
    }
  }

  auto parse_string(std::string& out) -> void {
    ++cur;  // opening quote
    while (cur < end && *cur != '"') {
      if (*cur != '\\') {
        out.push_back(*cur++);
        continue;
      }
      ++cur;
      if (cur >= end) {
        ok = false;
        return;
      }
      switch (*cur) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          // BMP escape -> UTF-8, combining valid surrogate pairs into the
          // astral code point; lone surrogates (or a high surrogate not
          // followed by a low one) fail instead of emitting invalid UTF-8.
          if (end - cur < 5) {
            ok = false;
            return;
          }
          unsigned cp = 0;
          for (int i = 1; i <= 4; ++i) {
            char c = cur[i];
            cp <<= 4;
            if (c >= '0' && c <= '9') {
              cp |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
              cp |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
              cp |= static_cast<unsigned>(c - 'A' + 10);
            } else {
              ok = false;
              return;
            }
          }
          cur += 5;
          if (cp >= 0xD800U && cp <= 0xDBFFU) {
            // High surrogate: require a trailing \uDC00-\uDFFF low half.
            if (end - cur < 6 || cur[0] != '\\' || cur[1] != 'u') {
              ok = false;
              return;
            }
            unsigned lo = 0;
            for (int i = 2; i <= 5; ++i) {
              char c = cur[i];
              lo <<= 4U;
              if (c >= '0' && c <= '9') {
                lo |= static_cast<unsigned>(c - '0');
              } else if (c >= 'a' && c <= 'f') {
                lo |= static_cast<unsigned>(c - 'a' + 10);
              } else if (c >= 'A' && c <= 'F') {
                lo |= static_cast<unsigned>(c - 'A' + 10);
              } else {
                ok = false;
                return;
              }
            }
            if (lo < 0xDC00U || lo > 0xDFFFU) {
              ok = false;
              return;
            }
            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
            cur += 6;
          } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
            ok = false;  // lone low surrogate.
            return;
          }
          if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(
                static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(
                static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          continue;
        }
        default:
          ok = false;
          return;
      }
      ++cur;
    }
    if (cur >= end) {
      ok = false;
      return;
    }
    ++cur;  // closing quote
  }

  auto parse_literal(Value& out) -> void {
    if (end - cur >= 4 && std::memcmp(cur, "true", 4) == 0) {
      out.kind = Value::Kind::True;
      cur += 4;
    } else if (end - cur >= 5 && std::memcmp(cur, "false", 5) == 0) {
      out.kind = Value::Kind::False;
      cur += 5;
    } else if (end - cur >= 4 && std::memcmp(cur, "null", 4) == 0) {
      out.kind = Value::Kind::Null;
      cur += 4;
    } else {
      // Number: raw text, validated at conversion by the consumer.
      const char* start = cur;
      if (cur < end && (*cur == '-' || *cur == '+')) {
        ++cur;
      }
      while (cur < end && ((*cur >= '0' && *cur <= '9') || *cur == '.' ||
                           *cur == 'e' || *cur == 'E' || *cur == '+' ||
                           *cur == '-')) {
        ++cur;
      }
      if (cur == start) {
        ok = false;
        return;
      }
      out.kind = Value::Kind::Num;
      out.str.assign(start, cur);
    }
  }

  // Shared bracket-list driver: parse_array and parse_object differ only
  // in closer/kind and per-item work. Sets out.kind, consumes the opener,
  // then loops items separated by ',' until closer. `item` runs per element
  // (parse + store); false aborts (ok already set). Depth is Enter()-ed by
  // the caller, released here on every exit path.
  template <typename F>
  auto ParseCommaList(char /*open*/, char close, Value::Kind kind, Value& out,
                      F&& item) -> void {
    out.kind = kind;
    ++cur;  // open bracket (caller peeked it via parse_value dispatch)
    skip_ws();
    if (cur < end && *cur == close) {
      ++cur;
      --depth;
      return;
    }
    while (true) {
      Value v;
      if (!item(v)) {
        return;
      }
      skip_ws();
      if (cur >= end) {
        ok = false;
        return;
      }
      if (*cur == close) {
        ++cur;
        --depth;
        return;
      }
      if (*cur != ',') {
        ok = false;
        return;
      }
      ++cur;
    }
  }

  auto parse_array(Value& out) -> void {
    if (!Enter()) {
      return;
    }
    ParseCommaList('[', ']', Value::Kind::Arr, out, [&](Value& item) {
      parse_value(item);
      if (!ok) {
        return false;
      }
      out.items.push_back(std::move(item));
      return true;
    });
  }

  auto parse_object(Value& out) -> void {
    if (!Enter()) {
      return;
    }
    ParseCommaList('{', '}', Value::Kind::Obj, out, [&](Value& /*slot*/) {
      skip_ws();
      if (cur >= end || *cur != '"') {
        ok = false;
        return false;
      }
      std::string key;
      parse_string(key);
      if (!ok) {
        return false;
      }
      skip_ws();
      if (cur >= end || *cur != ':') {
        ok = false;
        return false;
      }
      ++cur;
      Value val;
      parse_value(val);
      if (!ok) {
        return false;
      }
      out.keys.push_back(std::move(key));
      out.values.push_back(std::move(val));
      return true;
    });
  }
};

}  // namespace engine::json_min
