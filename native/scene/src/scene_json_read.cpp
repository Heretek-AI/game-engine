// scene_json_read.cpp — P2-06: JSON v1 reader.
//
// Minimal recursive-descent parser over the exact writer output (objects,
// arrays, strings with escapes, numbers, true/false/null skipped). No new
// dependency. Load is two-pass: create every entity + replay components,
// then replay parent links (parents may be defined after children).
#include "engine/scene/scene_io.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/scene/scene_file_util.hpp"
#include "flecs.h"

namespace engine::scene {

// Hex digit: -1 for non-hex (callers fail closed). Used by \u escapes
// (SceneParser::ParseHex4 above) and blob hex below — one copy on purpose.
auto HexVal(char c) -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

namespace {

// Local JSON value model for scene files (P2-06 contract): Str unescaped,
// Num raw text with true/false normalized to "1"/"0" by parse_literal below,
// parallel keys + values (insertion order; linear lookup is fine — objects
// are small: components per entity, fields per component). Deliberately NOT
// shared with engine/assets/json_minimal.hpp: the animator graph needs
// distinct True/False literal kinds and astral-pair string combining, and
// routing nested scene values through that parser silently broke nested
// conversions (CodeRabbit #206: nested is_main stayed 0, round-trip failed).
struct SceneValue {
  enum class Kind : std::uint8_t { Null, Num, Str, Arr, Obj };
  Kind kind{Kind::Null};
  std::string str;  // Str: unescaped; Num: raw text
  std::vector<SceneValue> items;  // Arr
  std::vector<std::string> keys;
  std::vector<SceneValue> values;

  [[nodiscard]] auto lookup(std::string_view key) const -> const SceneValue* {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == key) {
        return &values[i];
      }
    }
    return nullptr;
  }
};

struct SceneParser {
  // Scene parser: local value model + recursive descent with scene-specific
  // literal/string rules (true/false normalize to Num("1"/"0") per the
  // writer contract).

  // Valid scene files nest ~4 levels (entities[] -> entity -> components ->
  // field arrays). Cap recursion so a hostile file of 100k '[' cannot
  // overflow the stack — "never crash" rule. Depth counts only nested
  // containers (objects/arrays); sibling elements reuse the slot.
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

  auto parse_value(SceneValue& out) -> void {
    skip_ws();
    if (cur >= end) {
      ok = false;
      return;
    }
    switch (*cur) {
      case '{': parse_object(out); break;
      case '[': parse_array(out); break;
      case '"': {
        out.kind = SceneValue::Kind::Str;
        parse_string(out.str);
        break;
      }
      default: parse_literal(out); break;
    }
  }

  // Parse exactly 4 hex digits at cur[1..4] (cur points at 'u').
  // Reuses the TU's HexVal helper (blob hex below uses it too); leaves
  // cur on the last hex digit (the shared ++cur in parse_string advances
  // past it).
  auto ParseHex4(unsigned& unit) -> bool {
    if (end - cur < 5) {
      ok = false;
      return false;
    }
    unsigned v = 0;
    for (int i = 1; i <= 4; ++i) {
      const int h = HexVal(cur[i]);
      if (h < 0) {
        ok = false;
        return false;
      }
      v = (v << 4U) | static_cast<unsigned>(h);
    }
    cur += 4;
    unit = v;
    return true;
  }

  auto parse_string(std::string& out) -> void {
    // *cur == '"'.
    ++cur;
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
        case 'u': {
          // \uXXXX — decode as UTF-8 (combining valid surrogate pairs).
          // Writer escapes only ASCII control chars, but hand-written scene
          // files may carry any BMP escape; '?' replacement would corrupt
          // entity names / asset paths silently.
          unsigned code = 0;
          if (!ParseHex4(code)) {
            return;
          }
          // High surrogate: require a trailing low surrogate \uDC00-DFFF.
          if (code >= 0xD800U && code <= 0xDBFFU) {
            if (end - cur < 6 || cur[1] != '\\' || cur[2] != 'u') {
              ok = false;
              return;
            }
            cur += 2;  // position at low 'u'
            unsigned lo = 0;
            if (!ParseHex4(lo)) {
              return;
            }
            if (lo < 0xDC00U || lo > 0xDFFFU) {
              ok = false;
              return;
            }
            code = 0x10000U + ((code - 0xD800U) << 10U) + (lo - 0xDC00U);
          } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            ok = false;  // lone low surrogate.
            return;
          }
          // Encode code point as UTF-8.
          if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
          } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
          } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(
                static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
          } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(
                static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(
                static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
          }
          break;
        }
        default: ok = false; return;
      }
      ++cur;
    }
    if (cur >= end) {
      ok = false;
      return;
    }
    ++cur;  // closing quote
  }

  auto parse_literal(SceneValue& out) -> void {
    const char* begin = cur;
    while (cur < end && *cur != ',' && *cur != ']' && *cur != '}' &&
           *cur != ' ' && *cur != '\t' && *cur != '\n' && *cur != '\r') {
      ++cur;
    }
    if (cur == begin) {
      ok = false;
      return;
    }
    std::string_view tok(begin,
                         static_cast<std::size_t>(cur - begin));
    if (tok == "null") {
      out.kind = SceneValue::Kind::Null;
    } else if (tok == "true" || tok == "false") {
      out.kind = SceneValue::Kind::Num;
      out.str = (tok == "true") ? "1" : "0";
    } else {
      out.kind = SceneValue::Kind::Num;
      out.str.assign(tok);
    }
  }

  auto parse_array(SceneValue& out) -> void {
    if (!Enter()) {
      return;
    }
    out.kind = SceneValue::Kind::Arr;
    ++cur;  // [
    skip_ws();
    if (cur < end && *cur == ']') {
      ++cur;
      --depth;
      return;
    }
    while (cur < end) {
      SceneValue item;
      parse_value(item);
      if (!ok) {
        return;
      }
      out.items.push_back(std::move(item));
      skip_ws();
      if (cur >= end) {
        ok = false;
        return;
      }
      if (*cur == ',') {
        ++cur;
        continue;
      }
      if (*cur == ']') {
        ++cur;
        --depth;
        return;
      }
      ok = false;
      return;
    }
    ok = false;
  }

  auto parse_object(SceneValue& out) -> void {
    if (!Enter()) {
      return;
    }
    out.kind = SceneValue::Kind::Obj;
    ++cur;  // {
    skip_ws();
    if (cur < end && *cur == '}') {
      ++cur;
      --depth;
      return;
    }
    while (cur < end) {
      skip_ws();
      if (cur >= end || *cur != '"') {
        ok = false;
        return;
      }
      std::string key;
      parse_string(key);
      if (!ok) {
        return;
      }
      skip_ws();
      if (cur >= end || *cur != ':') {
        ok = false;
        return;
      }
      ++cur;
      SceneValue val;
      parse_value(val);
      if (!ok) {
        return;
      }
      out.keys.push_back(std::move(key));
      out.values.push_back(std::move(val));
      skip_ws();
      if (cur >= end) {
        ok = false;
        return;
      }
      if (*cur == ',') {
        ++cur;
        continue;
      }
      if (*cur == '}') {
        ++cur;
        --depth;
        return;
      }
      ok = false;
      return;
    }
    ok = false;
  }
};

auto ParseNum(std::string_view text, double& out) -> bool {
  // Writer emits via std::to_chars shortest round-trip (always '.' decimal).
  // libc++ (Apple) still lacks floating-point std::from_chars, and strtod
  // follows the process locale (',' decimal in some locales), so parse with a
  // locale-independent manual parser: [+-]? digits [. digits] [e[+-]digits].
  // Rejects leading whitespace and inf/nan so only exact JSON-number text
  // parses. Finite double accumulation overflows to HUGE_VAL through the
  // inf-range guard below (still a valid parse); exponent scale saturates so
  // e.g. 1e999 is rejected rather than looping or wrapping.
  if (text.empty() || text.size() >= 64) {
    return false;
  }
  std::size_t i = 0;
  if (text[i] == '+' || text[i] == '-') {
    ++i;
  }
  bool any_digits = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
    any_digits = true;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
      any_digits = true;
    }
  }
  if (!any_digits) {
    return false;
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
      ++i;
    }
    bool any_exp = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
      any_exp = true;
    }
    if (!any_exp) {
      return false;
    }
  }
  if (i != text.size()) {
    return false;
  }
  double val = 0;
  double frac_div = 1;
  bool neg = text[0] == '-';
  bool overflow = false;
  std::size_t p = (text[0] == '+' || text[0] == '-') ? 1 : 0;
  while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
    val = val * 10.0 + static_cast<double>(text[p] - '0');
    if (!std::isfinite(val)) {
      overflow = true;
      while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
        ++p;
      }
      break;
    }
    ++p;
  }
  if (!overflow && p < text.size() && text[p] == '.') {
    ++p;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
      val = val * 10.0 + static_cast<double>(text[p] - '0');
      frac_div *= 10.0;
      if (!std::isfinite(val) || !std::isfinite(frac_div)) {
        overflow = true;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
          ++p;
        }
        break;
      }
      ++p;
    }
  }
  if (!overflow) {
    val /= frac_div;
    if (!std::isfinite(val)) {
      overflow = true;
    }
  }
  if (!overflow && p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
    ++p;
    bool exp_neg = false;
    if (text[p] == '+' || text[p] == '-') {
      exp_neg = text[p] == '-';
      ++p;
    }
    int exp = 0;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
      // Saturate: beyond ~999 the result is +-inf or 0 regardless of digits.
      if (exp < 1000) {
        exp = exp * 10 + (text[p] - '0');
      }
      ++p;
    }
    if (exp > 308) {
      return false;  // beyond double range — reject, stay finite.
    }
    double scale = 1;
    for (int k = 0; k < exp; ++k) {
      scale *= exp_neg ? 0.1 : 10.0;
      if (!std::isfinite(scale)) {
        break;
      }
    }
    val *= scale;
    if (!std::isfinite(val)) {
      overflow = true;
    }
  }
  if (overflow) {
    return false;  // out of finite-double range — reject, stay finite.
  }
  out = neg ? -val : val;
  return true;
}

auto ParseU32(std::string_view text, std::uint32_t& out) -> bool {
  const char* begin = text.data();
  const char* last = text.data() + text.size();
  auto res = std::from_chars(begin, last, out);
  return res.ec == std::errc() && res.ptr == last;
}

auto NumOf(const SceneValue* v, double& out) -> bool {
  return v != nullptr && v->kind == SceneValue::Kind::Num &&
         ParseNum(v->str, out);
}

// Checked double -> float conversion for file input. ParseNum already
// rejects non-finite text, but a finite double outside the float range
// (e.g. 1e300) would make static_cast<float> undefined behavior —
// clamp to the float extremes (a defined value) instead.
auto NumFloat(const SceneValue* v, float& out) -> bool {
  double d = 0;
  if (!NumOf(v, d)) {
    return false;
  }
  constexpr double kMax =
      static_cast<double>(std::numeric_limits<float>::max());
  if (d > kMax) {
    out = std::numeric_limits<float>::max();
  } else if (d < -kMax) {
    out = -std::numeric_limits<float>::max();
  } else {
    out = static_cast<float>(d);
  }
  return true;
}

auto U32Of(const SceneValue* v, std::uint32_t& out) -> bool {
  return v != nullptr && v->kind == SceneValue::Kind::Num &&
         ParseU32(v->str, out);
}

template <std::size_t N>
auto GetFloatArray(const SceneValue& comp, std::string_view field,
                   std::array<float, N>& out) -> bool {
  const SceneValue* arr = comp.lookup(field);
  if (arr == nullptr || arr->kind != SceneValue::Kind::Arr ||
      arr->items.size() != N) {
    return false;
  }
  for (std::size_t i = 0; i < N; ++i) {
    float f = 0;
    if (!NumFloat(&arr->items[i], f)) {
      return false;
    }
    out[i] = f;
  }
  return true;
}

auto Lookup(ecs_world_t* world, const char* name) noexcept -> ecs_entity_t {
  return ecs_lookup_path_w_sep(world, 0, name, "", "", false);
}

// Register-or-fetch a user component by name for blob replay. Missing ->
// create with the recorded (size, align); present with a different layout
// -> 0 (caller warns + skips, never aborts — gotcha 20).
auto UserComponent(ecs_world_t* world, const char* name, std::uint32_t size,
                   std::uint32_t align) noexcept -> ecs_entity_t {
  return engine::ecs::RegisterRenderComponent(world, name, size, align);
}


// Resolve a "path#index" mesh ref to a mesh_id. Known refs come from the
// registry. "mesh_id:<n>" still parses (files written by older builds) but
// the writer no longer emits it — see MeshRefFor above.
auto MeshIdFor(std::string_view ref) -> std::uint32_t {
  // Direct fallback form first (no registry needed).
  constexpr std::string_view kPrefix = "mesh_id:";
  if (ref.size() > kPrefix.size() &&
      ref.substr(0, kPrefix.size()) == kPrefix) {
    std::uint32_t id = 0;
    if (ParseU32(ref.substr(kPrefix.size()), id)) {
      return id;
    }
  }
  std::string want(ref);
  return mesh_id_for_ref(want.c_str());
}

}  // namespace

auto load_scene(ecs_world_t* world, const char* path) noexcept -> int32_t {
  if (world == nullptr || path == nullptr || path[0] == '\0') {
    return kSceneNullArg;
  }
  std::string text;
  if (!file_util::ReadAll(path, text)) {
    return kSceneIoError;
  }

  SceneParser parser;
  parser.cur = text.data();
  parser.end = text.data() + text.size();
  SceneValue root;
  parser.parse_value(root);
  if (!parser.ok || root.kind != SceneValue::Kind::Obj) {
    return kSceneBadFormat;
  }
  const SceneValue* format = root.lookup("format");
  const SceneValue* version = root.lookup("version");
  const SceneValue* entities = root.lookup("entities");
  if (format == nullptr || format->kind != SceneValue::Kind::Str ||
      format->str != "engine-scene" || version == nullptr ||
      version->kind != SceneValue::Kind::Num || version->str != "1" ||
      entities == nullptr || entities->kind != SceneValue::Kind::Arr) {
    return kSceneBadFormat;
  }

  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return kSceneNoLocal;
  }
  const ecs_entity_t cam_id = Lookup(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id = Lookup(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      Lookup(world, "Engine.Runtime.DirectionalLight");
  // Built-ins must resolve in the target world before load: the writer only
  // emits named payloads, so a world without register_render() would
  // silently drop cameras/meshes (round-trip gate). Register on demand.
  if (cam_id == 0 || mesh_id == 0 || light_id == 0) {
    engine::ecs::RegisterRenderComponent(world, "Engine.Runtime.Camera",
                                         sizeof(engine::ecs::Camera),
                                         alignof(engine::ecs::Camera));
    engine::ecs::RegisterRenderComponent(world, "Engine.Runtime.MeshRenderer",
                                         sizeof(engine::ecs::MeshRenderer),
                                         alignof(engine::ecs::MeshRenderer));
    engine::ecs::RegisterRenderComponent(
        world, "Engine.Runtime.DirectionalLight",
        sizeof(engine::ecs::DirectionalLight),
        alignof(engine::ecs::DirectionalLight));
  }
  const ecs_entity_t cam_id2 = Lookup(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id2 = Lookup(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id2 =
      Lookup(world, "Engine.Runtime.DirectionalLight");

  int32_t skipped = 0;
  std::vector<std::pair<std::uint32_t, ecs_entity_t>> id_map;
  id_map.reserve(entities->items.size());
  // Pass 0: validate every entity header before creating anything. Without
  // this a bad entity mid-file leaves earlier entities already created (no
  // parent links yet) and the caller cannot roll back — a retry would
  // duplicate them. Reject duplicate file-local ids too: pass 2 resolves
  // parents through id_map, so a duplicate makes the lookup ambiguous.
  {
    std::vector<std::uint32_t> seen;
    seen.reserve(entities->items.size());
    for (const SceneValue& ent : entities->items) {
      if (ent.kind != SceneValue::Kind::Obj) {
        return kSceneBadFormat;
      }
      const SceneValue* fid = ent.lookup("id");
      const SceneValue* fname = ent.lookup("name");
      const SceneValue* fcomps = ent.lookup("components");
      std::uint32_t file_id = 0;
      if (!U32Of(fid, file_id) || file_id == 0 || fname == nullptr ||
          fname->kind != SceneValue::Kind::Str || fcomps == nullptr ||
          fcomps->kind != SceneValue::Kind::Obj ||
          std::find(seen.begin(), seen.end(), file_id) != seen.end()) {
        return kSceneBadFormat;
      }
      seen.push_back(file_id);
    }
  }
  // Pass 1: create entities + components (parents replayed in pass 2).
  for (const SceneValue& ent : entities->items) {
    if (ent.kind != SceneValue::Kind::Obj) {
      return kSceneBadFormat;
    }
    const SceneValue* fid = ent.lookup("id");
    const SceneValue* fname = ent.lookup("name");
    const SceneValue* fcomps = ent.lookup("components");
    std::uint32_t file_id = 0;
    if (!U32Of(fid, file_id) || file_id == 0 || fname == nullptr ||
        fname->kind != SceneValue::Kind::Str || fcomps == nullptr ||
        fcomps->kind != SceneValue::Kind::Obj) {
      return kSceneBadFormat;
    }
    const ecs_entity_t e = ecs_new(world);
    if (e == 0) {
      return kSceneIoError;
    }
    ecs_set_name(world, e, fname->str.c_str());
    id_map.emplace_back(file_id, e);

    for (std::size_t c = 0; c < fcomps->keys.size(); ++c) {
      const std::string& cname = fcomps->keys[c];
      const SceneValue& cval = fcomps->values[c];
      if (cval.kind != SceneValue::Kind::Obj) {
        continue;
      }
      if (cname == "LocalTransform") {
        // ecs_get_mut_id does not add: emplace first (same rule as the
        // test's SetPose — a null mutate pointer segfaults on write).
        ecs_add_id(world, e, local_id);
        auto* lt = static_cast<engine::ecs::LocalTransform*>(
            ecs_get_mut_id(world, e, local_id));
        if (lt == nullptr) {
          continue;
        }
        std::array<float, 3> pos{};
        std::array<float, 4> rot{0, 0, 0, 1};
        std::array<float, 3> scl{1, 1, 1};
        GetFloatArray(cval, "position", pos);
        GetFloatArray(cval, "rotation", rot);
        GetFloatArray(cval, "scale", scl);
        lt->px = pos[0];
        lt->py = pos[1];
        lt->pz = pos[2];
        lt->qx = rot[0];
        lt->qy = rot[1];
        lt->qz = rot[2];
        lt->qw = rot[3];
        lt->sx = scl[0];
        lt->sy = scl[1];
        lt->sz = scl[2];
      } else if (cname == "Camera" && cam_id2 != 0) {
        ecs_add_id(world, e, cam_id2);
        auto* cam = static_cast<engine::ecs::Camera*>(
            ecs_get_mut_id(world, e, cam_id2));
        if (cam == nullptr) {
          continue;
        }
        std::uint32_t is_main = 0;
        float fov_f = 0;
        float near_f = 0;
        float far_f = 0;
        NumFloat(cval.lookup("fov_y_rad"), fov_f);
        NumFloat(cval.lookup("near_z"), near_f);
        NumFloat(cval.lookup("far_z"), far_f);
        U32Of(cval.lookup("is_main"), is_main);
        cam->fov_y_rad = fov_f;
        cam->near_z = near_f;
        cam->far_z = far_f;
        cam->is_main = is_main;
      } else if (cname == "MeshRenderer" && mesh_id2 != 0) {
        ecs_add_id(world, e, mesh_id2);
        auto* mr = static_cast<engine::ecs::MeshRenderer*>(
            ecs_get_mut_id(world, e, mesh_id2));
        if (mr == nullptr) {
          continue;
        }
        const SceneValue* mesh = cval.lookup("mesh");
        mr->mesh_id =
            (mesh != nullptr && mesh->kind == SceneValue::Kind::Str)
                ? MeshIdFor(mesh->str)
                : 0;
        std::array<float, 4> color{1, 1, 1, 1};
        GetFloatArray(cval, "base_color", color);
        mr->base_color[0] = color[0];
        mr->base_color[1] = color[1];
        mr->base_color[2] = color[2];
        mr->base_color[3] = color[3];
      } else if (cname == "DirectionalLight" && light_id2 != 0) {
        ecs_add_id(world, e, light_id2);
        auto* li = static_cast<engine::ecs::DirectionalLight*>(
            ecs_get_mut_id(world, e, light_id2));
        if (li == nullptr) {
          continue;
        }
        std::array<float, 3> dir{};
        float shadow_f = 0;
        GetFloatArray(cval, "direction", dir);
        // Writer emits the lane as "intensity" (spec format); the native
        // field is `shadow` (P4-03 renamed the lane, same offset).
        if (!NumFloat(cval.lookup("intensity"), shadow_f)) {
          NumFloat(cval.lookup("shadow"), shadow_f);
        }
        li->direction[0] = dir[0];
        li->direction[1] = dir[1];
        li->direction[2] = dir[2];
        li->shadow = shadow_f;
      } else if (cname == "WorldTransform") {
        // Derived — never serialized by the writer; ignore when present.
        continue;
      } else if (cname == "Camera" || cname == "MeshRenderer" ||
                 cname == "DirectionalLight" || cname == "LocalTransform") {
        // Built-in id missing in this world but present in the file (e.g. a
        // world that never called register_render): skip the named payload
        // instead of warning as a user blob.
        continue;
      } else {
        // User blob: $bytes + $size + $schema.
        const SceneValue* bytes = cval.lookup("$bytes");
        const SceneValue* size_v = cval.lookup("$size");
        const SceneValue* schema_v = cval.lookup("$schema");
        std::uint32_t size = 0;
        if (bytes == nullptr || bytes->kind != SceneValue::Kind::Str ||
            !U32Of(size_v, size) || size == 0 || size > 65536U ||
            schema_v == nullptr || schema_v->kind != SceneValue::Kind::Str) {
          std::printf(
              "[WARN] scene: bad user component '%s' (malformed blob) "
              "skipped\n",
              cname.c_str());
          ++skipped;
          continue;
        }
        if (bytes->str.size() != static_cast<std::size_t>(size) * 2U) {
          std::printf(
              "[WARN] scene: bad user component '%s' (size %u vs %zu hex "
              "chars) skipped\n",
              cname.c_str(), size, bytes->str.size());
          ++skipped;
          continue;
        }
        // Schema check against the CURRENT registration (size:align) when
        // the component already exists; for unknown names recover the
        // alignment by brute-forcing powers of two against the file's
        // $schema (FNV-1a of "name:size:align"). A file schema that matches
        // no power-of-two alignment is rejected with a warning — never
        // registered with a guessed align (gotcha: wrong align poisons a
        // later real-type registration via RegisterRenderComponent).
        const ecs_entity_t existing = Lookup(world, cname.c_str());
        std::uint32_t align = 0;
        std::uint32_t file_schema = 0;
        {
          const char* hex = schema_v->str.c_str();
          bool hex_ok = schema_v->str.size() == 8;
          for (std::size_t i = 0; hex_ok && i < 8U; ++i) {
            const int v = HexVal(hex[i]);
            if (v < 0) {
              hex_ok = false;
              break;
            }
            file_schema =
                (file_schema << 4U) | static_cast<std::uint32_t>(v);
          }
          if (!hex_ok) {
            std::printf(
                "[WARN] scene: bad user component '%s' (malformed $schema) "
                "skipped\n",
                cname.c_str());
            ++skipped;
            continue;
          }
        }
        if (existing != 0) {
          const ecs_type_info_t* ti = ecs_get_type_info(world, existing);
          if (ti == nullptr || ti->size <= 0) {
            std::printf(
                "[WARN] scene: bad user component '%s' (present but not a "
                "data component) skipped\n",
                cname.c_str());
            ++skipped;
            continue;
          }
          align = static_cast<std::uint32_t>(ti->alignment);
          if (static_cast<std::uint32_t>(ti->size) != size) {
            std::printf(
                "[WARN] scene: bad user component '%s' (size %u vs live "
                "%d) skipped\n",
                cname.c_str(), size, ti->size);
            ++skipped;
            continue;
          }
          // Known alignment: verify the schema hash too.
          if (file_schema != schema_hash(cname.c_str(), size, align)) {
            std::printf(
                "[WARN] scene: bad user component '%s' (schema mismatch) "
                "skipped\n",
                cname.c_str());
            ++skipped;
            continue;
          }
        } else {
          // Unknown name: recover the alignment from the file schema by
          // trying every power of two up to `size` (align never exceeds
          // size for a sane layout). No match -> skip with a warning;
          // registering with a guessed align would poison a later
          // real-type registration (RegisterRenderComponent rejects
          // layout mismatch, so the managed ComponentId<T> setup would
          // fail after a load that "succeeded").
          std::uint32_t recovered = 0;
          for (std::uint32_t a = 1; a <= size && a <= 64U; a <<= 1U) {
            if (schema_hash(cname.c_str(), size, a) == file_schema) {
              recovered = a;
              break;
            }
          }
          if (recovered == 0) {
            std::printf(
                "[WARN] scene: bad user component '%s' (schema mismatch) "
                "skipped\n",
                cname.c_str());
            ++skipped;
            continue;
          }
          align = recovered;
        }
        // Decode hex.
        std::vector<std::uint8_t> decoded;
        decoded.reserve(size);
        bool hex_ok = true;
        for (std::uint32_t i = 0; i < size; ++i) {
          const int hi = HexVal(bytes->str[2U * i]);
          const int lo = HexVal(bytes->str[2U * i + 1U]);
          if (hi < 0 || lo < 0) {
            hex_ok = false;
            break;
          }
          decoded.push_back(
              static_cast<std::uint8_t>((hi << 4) | lo));
        }
        if (!hex_ok) {
          std::printf(
              "[WARN] scene: bad user component '%s' (non-hex bytes) "
              "skipped\n",
              cname.c_str());
          ++skipped;
          continue;
        }
        // Register-on-demand for unknown names with the recovered alignment
        // (probed from the file $schema above); for known names the live
        // registration wins (already validated).
        const ecs_entity_t comp =
            (existing != 0)
                ? existing
                : UserComponent(world, cname.c_str(), size, align);
        if (comp == 0) {
          std::printf(
              "[WARN] scene: bad user component '%s' (registration "
              "rejected) skipped\n",
              cname.c_str());
          ++skipped;
          continue;
        }
        ecs_set_id(world, e, comp, size, decoded.data());
      }
    }
    // WorldTransform for the new entity (derived state, like construct).
    ecs_add_id(world, e, ecs_lookup(world, "WorldTransform"));
  }
  // Pass 2: parent links (file-local id -> live entity). The file parent
  // map is walked (not the live world) so longer cycles (A->B->A) are
  // detected too. A link that resolves to self, to an unknown id, or that
  // closes a cycle is skipped with a warning — adding ChildOf(e, e) is a
  // Flecs cycle, and pass 1 deliberately created no links yet.
  std::vector<std::uint32_t> file_parents(entities->items.size(), 0);
  for (std::size_t i = 0; i < entities->items.size(); ++i) {
    const SceneValue* fparent = entities->items[i].lookup("parent");
    std::uint32_t parent_file = 0;
    if (fparent != nullptr && fparent->kind == SceneValue::Kind::Num) {
      ParseU32(fparent->str, parent_file);
    }
    file_parents[i] = parent_file;
  }
  std::size_t idx = 0;
  for (const SceneValue& ent : entities->items) {
    const std::uint32_t parent_file =
        (idx < file_parents.size()) ? file_parents[idx] : 0;
    if (parent_file != 0 && idx < id_map.size()) {
      ecs_entity_t parent_live = 0;
      for (const auto& kv : id_map) {
        if (kv.first == parent_file) {
          parent_live = kv.second;
          break;
        }
      }
      const ecs_entity_t self_live = id_map[idx].second;
      bool cycle = (parent_live == self_live) || (parent_live == 0);
      if (!cycle) {
        // Walk the file parent chain from the parent: if it reaches this
        // entity's file id, the link would close a cycle.
        std::uint32_t cursor = parent_file;
        for (std::size_t steps = 0; steps <= file_parents.size(); ++steps) {
          if (cursor == id_map[idx].first) {
            cycle = true;
            break;
          }
          std::uint32_t next = 0;
          for (std::size_t j = 0; j < id_map.size(); ++j) {
            if (id_map[j].first == cursor && j < file_parents.size()) {
              next = file_parents[j];
              break;
            }
          }
          if (next == 0) {
            break;
          }
          cursor = next;
        }
      }
      if (cycle) {
        std::printf(
            "[WARN] scene: parent link for '%s' skipped (cycle or unknown "
            "parent %u)\n",
            ent.lookup("name")->str.c_str(), parent_file);
      } else {
        ecs_add_pair(world, self_live, EcsChildOf, parent_live);
      }
    }
    ++idx;
  }
  return skipped;
}

}  // namespace engine::scene
