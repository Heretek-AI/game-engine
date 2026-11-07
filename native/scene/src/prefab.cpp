// prefab.cpp — P6-09: prefab assets, instantiation, overrides, nesting.
//
// Format: {"format":"engine-prefab","version":1,"entities":[...]} where each
// entity mirrors the P2-06 scene payload (id/name/parent/components with
// the same named built-ins + $bytes blobs + P3-04 named user fields).
// A PrefabRef sibling key mounts a nested prefab by reference:
//
//   {"id":3,"name":"LampMount","parent":1,
//    "PrefabRef":{"asset":"props/lamp.engine-prefab"}}
//
// Parsing reuses the scene reader's JSON model? No — the reader's
// SceneValue/SceneParser live in an anonymous namespace inside
// scene_json_read.cpp, so this TU carries a small profiled JSON reader for
// the envelope only (objects/arrays/strings/numbers/true/false/null, 64
// deep). Component payloads are applied through a shared per-component
// applier that speaks the same payload dialect as the reader (named
// built-ins, $bytes blobs with $schema validation, named user fields via
// the describe callback), so a prefab written by save_prefab always reads
// back through instantiate/update/apply.
//
// noexcept throughout (bridge boundary rule); std::string/vector growth
// uses the throwing form only where OOM is unrecoverable at editor scale
// (same rule as the editor_bridge TU) and is caught at the ABI boundary.
#include "engine/scene/prefab.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/scene/scene_entities.hpp"
#include "engine/scene/scene_file_util.hpp"
#include "engine/scene/scene_io.hpp"
#include "flecs.h"

namespace engine::prefab {

namespace {

// --- JSON envelope reader -------------------------------------------------
// Profiled to the prefab envelope (mirrors the scene reader's rules:
// true/false normalize to Num "1"/"0", \u escapes decoded, 64-deep cap).

struct PVal {
  enum class Kind : std::uint8_t { Null, Num, Str, Arr, Obj };
  Kind kind{Kind::Null};
  std::string str;
  std::vector<PVal> items;
  std::vector<std::string> keys;
  std::vector<PVal> values;

  [[nodiscard]] auto lookup(std::string_view key) const -> const PVal* {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == key) {
        return &values[i];
      }
    }
    return nullptr;
  }
};

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

struct PParser {
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

  auto parse_value(PVal& out) -> void {
    skip_ws();
    if (cur >= end) {
      ok = false;
      return;
    }
    switch (*cur) {
      case '{': parse_object(out); break;
      case '[': parse_array(out); break;
      case '"':
        out.kind = PVal::Kind::Str;
        parse_string(out.str);
        break;
      default: parse_literal(out); break;
    }
  }

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
        case 'u': {
          unsigned code = 0;
          if (!ParseHex4(code)) {
            return;
          }
          if (code >= 0xD800U && code <= 0xDBFFU) {
            if (end - cur < 6 || cur[1] != '\\' || cur[2] != 'u') {
              ok = false;
              return;
            }
            cur += 2;
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
            ok = false;
            return;
          }
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

  auto parse_literal(PVal& out) -> void {
    const char* begin = cur;
    while (cur < end && *cur != ',' && *cur != ']' && *cur != '}' &&
           *cur != ' ' && *cur != '\t' && *cur != '\n' && *cur != '\r') {
      ++cur;
    }
    if (cur == begin) {
      ok = false;
      return;
    }
    std::string_view tok(begin, static_cast<std::size_t>(cur - begin));
    if (tok == "null") {
      out.kind = PVal::Kind::Null;
    } else if (tok == "true" || tok == "false") {
      out.kind = PVal::Kind::Num;
      out.str = (tok == "true") ? "1" : "0";
    } else {
      out.kind = PVal::Kind::Num;
      out.str.assign(tok);
    }
  }

  auto parse_array(PVal& out) -> void {
    if (!Enter()) {
      return;
    }
    out.kind = PVal::Kind::Arr;
    ++cur;
    skip_ws();
    if (cur < end && *cur == ']') {
      ++cur;
      --depth;
      return;
    }
    while (cur < end) {
      PVal item;
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

  auto parse_object(PVal& out) -> void {
    if (!Enter()) {
      return;
    }
    out.kind = PVal::Kind::Obj;
    ++cur;
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
      PVal val;
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

// --- number conversions (same rules as the scene reader) -------------------

auto ParseNum(std::string_view text, double& out) -> bool {
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
      if (exp < 1000) {
        exp = exp * 10 + (text[p] - '0');
      }
      ++p;
    }
    if (exp > 308) {
      return false;
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
    return false;
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

auto NumOf(const PVal* v, double& out) -> bool {
  return v != nullptr && v->kind == PVal::Kind::Num && ParseNum(v->str, out);
}

auto NumFloat(const PVal* v, float& out) -> bool {
  double d = 0;
  if (!NumOf(v, d)) {
    return false;
  }
  constexpr double kMax = static_cast<double>(std::numeric_limits<float>::max());
  if (d > kMax) {
    out = std::numeric_limits<float>::max();
  } else if (d < -kMax) {
    out = -std::numeric_limits<float>::max();
  } else {
    out = static_cast<float>(d);
  }
  return true;
}

auto U32Of(const PVal* v, std::uint32_t& out) -> bool {
  return v != nullptr && v->kind == PVal::Kind::Num && ParseU32(v->str, out);
}

template <std::size_t N>
auto GetFloatArray(const PVal& comp, std::string_view field,
                   std::array<float, N>& out) -> bool {
  const PVal* arr = comp.lookup(field);
  if (arr == nullptr || arr->kind != PVal::Kind::Arr ||
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

// --- JSON writer ------------------------------------------------------------

auto AppendJsonString(std::string& out, std::string_view text) -> void {
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(static_cast<unsigned>(c) >> 4U) & 0xFU]);
          out.push_back(kHex[static_cast<unsigned>(c) & 0xFU]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

auto AppendU32(std::string& out, std::uint32_t value) -> void {
  std::array<char, 16> buf{};
  auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  out.append(buf.data(), res.ptr);
}

auto AppendFloat(std::string& out, float value) -> void {
  if (!std::isfinite(value)) {
    out += "0";
    return;
  }
  std::array<char, 32> buf{};
  auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (res.ec == std::errc()) {
    out.append(buf.data(), res.ptr);
  } else {
    out += "0";
  }
}

auto AppendFloatArray(std::string& out, const float* values, std::size_t n)
    -> void {
  out.push_back('[');
  for (std::size_t i = 0; i < n; ++i) {
    if (i != 0U) {
      out.push_back(',');
    }
    AppendFloat(out, values[i]);
  }
  out.push_back(']');
}

auto AppendHex(std::string& out, const std::uint8_t* bytes, std::size_t count)
    -> void {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kHex[(bytes[i] >> 4U) & 0xFU]);
    out.push_back(kHex[bytes[i] & 0xFU]);
  }
}

constexpr std::uint32_t kLaneSizes[11] = {4, 4, 4, 1, 8, 12, 16,
                                          16, 16, 8, 8};
constexpr std::uint32_t kFieldsPerVec[11] = {1, 1, 1, 1, 2, 3, 4, 4, 4, 1, 1};

// Serialize one component's bytes as named fields (describe-driven).
// Returns false when the lanes don't validate (caller falls back to the
// $bytes blob). Entity-lane file ids resolve through `file_id`.
auto AppendNamedComponent(
    std::string& out, const scene::SceneFieldDesc* fields, std::uint32_t count,
    const std::uint8_t* bytes, std::uint32_t comp_size,
    const std::function<std::uint32_t(std::uint64_t)>& file_id) -> bool {
  for (std::uint32_t i = 0; i < count; ++i) {
    const scene::SceneFieldDesc& f = fields[i];
    if (f.type > 10) {
      return false;
    }
    const std::uint32_t lane = kLaneSizes[f.type];
    if (f.offset > comp_size || lane > comp_size - f.offset) {
      return false;
    }
    if (i != 0U) {
      out.push_back(',');
    }
    AppendJsonString(out, f.name);
    out.push_back(':');
    const std::uint8_t* lane_ptr = bytes + f.offset;
    switch (f.type) {
      case 0: {
        float v = 0;
        std::memcpy(&v, lane_ptr, 4);
        AppendFloat(out, v);
        break;
      }
      case 1: {
        std::int32_t v = 0;
        std::memcpy(&v, lane_ptr, 4);
        std::array<char, 16> num{};
        auto res = std::to_chars(num.data(), num.data() + num.size(), v);
        out.append(num.data(), res.ptr);
        break;
      }
      case 2: {
        std::uint32_t v = 0;
        std::memcpy(&v, lane_ptr, 4);
        AppendU32(out, v);
        break;
      }
      case 3: {
        out += (*lane_ptr != 0) ? "true" : "false";
        break;
      }
      case 4:
      case 5:
      case 6:
      case 7:
      case 8: {
        const std::uint32_t n = kFieldsPerVec[f.type];
        out.push_back('[');
        for (std::uint32_t k = 0; k < n; ++k) {
          if (k != 0U) {
            out.push_back(',');
          }
          float v = 0;
          std::memcpy(&v, lane_ptr + k * 4, 4);
          AppendFloat(out, v);
        }
        out.push_back(']');
        break;
      }
      case 9: {
        std::uint64_t runtime = 0;
        std::memcpy(&runtime, lane_ptr, 8);
        AppendU32(out, file_id(runtime));
        break;
      }
      case 10: {
        std::uint64_t v = 0;
        std::memcpy(&v, lane_ptr, 8);
        std::array<char, 24> num{};
        auto res = std::to_chars(num.data(), num.data() + num.size(), v);
        out.append(num.data(), res.ptr);
        break;
      }
      default: return false;
    }
  }
  return true;
}

// --- mesh ref + schema hash (same rules as the scene writer) -----------------
// Meshes serialize as asset path + "#" + submesh index (never runtime ids).

struct MeshSource {
  std::uint32_t id{0};
  std::string ref;
};

auto MeshRegistry() noexcept -> std::vector<MeshSource>& {
  static std::vector<MeshSource> reg;
  return reg;
}

auto MeshRefFor(std::uint32_t mesh_id) -> std::string {
  for (const MeshSource& src : MeshRegistry()) {
    if (src.id == mesh_id) {
      return src.ref;
    }
  }
  // Fall back to the scene module's registry (populated by scene saves
  // and register_mesh_source in the same process).
  return {};
}

auto MeshIdForRef(std::string_view ref) -> std::uint32_t {
  constexpr std::string_view kPrefix = "mesh_id:";
  if (ref.size() > kPrefix.size() && ref.substr(0, kPrefix.size()) == kPrefix) {
    std::uint32_t id = 0;
    if (ParseU32(ref.substr(kPrefix.size()), id)) {
      return id;
    }
  }
  for (const MeshSource& src : MeshRegistry()) {
    if (src.ref == ref) {
      return src.id;
    }
  }
  std::string want(ref);
  return scene::mesh_id_for_ref(want.c_str());
}

constexpr std::uint32_t kFnvOffset = 2166136261U;
constexpr std::uint32_t kFnvPrime = 16777619U;

auto Fnv1a(std::string_view text) noexcept -> std::uint32_t {
  std::uint32_t hash = kFnvOffset;
  for (char c : text) {
    hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

auto SchemaHash(const char* name, std::uint32_t size, std::uint32_t align)
    -> std::uint32_t {
  if (name == nullptr) {
    return 0;
  }
  std::string key(name);
  key.push_back(':');
  std::array<char, 16> num{};
  auto r0 = std::to_chars(num.data(), num.data() + num.size(), size);
  key.append(num.data(), r0.ptr);
  key.push_back(':');
  auto r1 = std::to_chars(num.data(), num.data() + num.size(), align);
  key.append(num.data(), r1.ptr);
  return Fnv1a(key);
}

// ParsePrefabText forward declaration removed in P6-09 cleanup: the
// envelope parse lives in ParsePrefabTextImpl (below) and FillRowComps;

// Component payload -> raw bytes. `comp` is the "components" object entry
// for one named component. Named built-ins decode field-wise; user blobs
// decode $bytes hex (schema/align recorded for the writer round-trip).
// Returns false when the payload is malformed (caller skips with warning).
struct DecodedComp {
  std::string name;
  std::vector<std::uint8_t> bytes;  // empty for built-ins (see kind)
  // Built-in payload, decoded to native structs when kind != kUser:
  int kind{0};  // 0=user blob, 1=pose, 2=camera, 3=mesh, 4=light
  std::array<float, 10> pose{};
  engine::ecs::Camera cam{};
  engine::ecs::MeshRenderer mesh{};
  engine::ecs::DirectionalLight light{};
  std::uint32_t blob_size{0};
  std::uint32_t blob_schema{0};
};

auto DecodePayload(const std::string& cname, const PVal& cval,
                   scene::SceneDescribeFn describe, void* user,
                   DecodedComp& out) -> bool {
  out.name = cname;
  if (cname == "LocalTransform") {
    std::array<float, 3> pos{};
    std::array<float, 4> rot{0, 0, 0, 1};
    std::array<float, 3> scl{1, 1, 1};
    GetFloatArray(cval, "position", pos);
    GetFloatArray(cval, "rotation", rot);
    GetFloatArray(cval, "scale", scl);
    out.kind = 1;
    out.pose = {pos[0], pos[1], pos[2], rot[0], rot[1],
                rot[2], rot[3], scl[0], scl[1], scl[2]};
    return true;
  }
  if (cname == "Camera") {
    float fov = 0;
    float nz = 0;
    float fz = 0;
    std::uint32_t is_main = 0;
    NumFloat(cval.lookup("fov_y_rad"), fov);
    NumFloat(cval.lookup("near_z"), nz);
    NumFloat(cval.lookup("far_z"), fz);
    U32Of(cval.lookup("is_main"), is_main);
    out.kind = 2;
    out.cam = {fov, nz, fz, is_main};
    return true;
  }
  if (cname == "MeshRenderer") {
    const PVal* mesh = cval.lookup("mesh");
    std::uint32_t mid = 0;
    if (mesh != nullptr && mesh->kind == PVal::Kind::Str) {
      mid = MeshIdForRef(mesh->str);
    }
    std::array<float, 4> color{1, 1, 1, 1};
    GetFloatArray(cval, "base_color", color);
    out.kind = 3;
    out.mesh.mesh_id = mid;
    out.mesh.base_color[0] = color[0];
    out.mesh.base_color[1] = color[1];
    out.mesh.base_color[2] = color[2];
    out.mesh.base_color[3] = color[3];
    return true;
  }
  if (cname == "DirectionalLight") {
    std::array<float, 3> dir{};
    float shadow_f = 0;
    GetFloatArray(cval, "direction", dir);
    if (!NumFloat(cval.lookup("intensity"), shadow_f)) {
      NumFloat(cval.lookup("shadow"), shadow_f);
    }
    out.kind = 4;
    out.light.direction[0] = dir[0];
    out.light.direction[1] = dir[1];
    out.light.direction[2] = dir[2];
    out.light.shadow = shadow_f;
    return true;
  }
  if (cname == "WorldTransform") {
    return false;  // derived — never stored.
  }
  // User payload: $bytes legacy first, then named fields via describe.
  const PVal* bytes = cval.lookup("$bytes");
  if (bytes == nullptr && describe != nullptr) {
    std::uint32_t desc_count = 0;
    const scene::SceneFieldDesc* desc_fields =
        describe(cname.c_str(), &desc_count, user);
    if (desc_fields == nullptr || desc_count == 0) {
      return false;
    }
    // Stage into a zeroed buffer sized by the largest lane end.
    std::uint32_t comp_size = 0;
    for (std::uint32_t fi = 0; fi < desc_count; ++fi) {
      const scene::SceneFieldDesc& f = desc_fields[fi];
      if (f.type > 10) {
        return false;
      }
      const std::uint32_t end = f.offset + kLaneSizes[f.type];
      if (end > comp_size) {
        comp_size = end;
      }
    }
    if (comp_size == 0 || comp_size > 65536U) {
      return false;
    }
    std::vector<std::uint8_t> staged(comp_size, 0);
    constexpr std::uint32_t kN[11] = {1, 1, 1, 1, 2, 3, 4, 4, 4, 1, 1};
    for (std::uint32_t fi = 0; fi < desc_count; ++fi) {
      const scene::SceneFieldDesc& f = desc_fields[fi];
      const PVal* fv = cval.lookup(f.name);
      if (fv == nullptr) {
        continue;
      }
      if (f.type > 10 || f.offset >= comp_size ||
          kLaneSizes[f.type] > comp_size - f.offset) {
        return false;
      }
      std::uint8_t* dst = staged.data() + f.offset;
      switch (f.type) {
        case 0: {
          float v = 0;
          if (!NumFloat(fv, v)) {
            return false;
          }
          std::memcpy(dst, &v, 4);
          break;
        }
        case 1: {
          double d = 0;
          if (!NumOf(fv, d) || d < -2147483648.0 || d > 2147483647.0 ||
              std::floor(d) != d) {
            return false;
          }
          const auto v = static_cast<std::int32_t>(d);
          std::memcpy(dst, &v, 4);
          break;
        }
        case 2: {
          std::uint32_t v = 0;
          if (!U32Of(fv, v)) {
            return false;
          }
          std::memcpy(dst, &v, 4);
          break;
        }
        case 3: {
          double d = 0;
          if (!NumOf(fv, d) || (d != 0 && d != 1)) {
            return false;
          }
          *dst = (d == 1) ? 1 : 0;
          break;
        }
        case 4:
        case 5:
        case 6:
        case 7:
        case 8: {
          const std::uint32_t n = kN[f.type];
          if (fv->kind != PVal::Kind::Arr || fv->items.size() != n) {
            return false;
          }
          for (std::uint32_t lane = 0; lane < n; ++lane) {
            float v = 0;
            if (!NumFloat(&fv->items[lane], v)) {
              return false;
            }
            std::memcpy(dst + lane * 4, &v, 4);
          }
          break;
        }
        case 9: {
          // Entity lane: file-local id kept as-is (remap at materialize).
          std::uint32_t ref_id = 0;
          if (fv->kind == PVal::Kind::Num && ParseU32(fv->str, ref_id)) {
            const auto raw = static_cast<std::uint64_t>(ref_id);
            std::memcpy(dst, &raw, 8);
          } else {
            return false;
          }
          break;
        }
        case 10: {
          std::uint32_t v32 = 0;
          double d = 0;
          if (U32Of(fv, v32)) {
            const auto v = static_cast<std::uint64_t>(v32);
            std::memcpy(dst, &v, 8);
          } else if (NumOf(fv, d) && d >= 0 &&
                     d <= 18446744073709551615.0 && std::floor(d) == d) {
            const auto v = static_cast<std::uint64_t>(d);
            std::memcpy(dst, &v, 8);
          } else {
            return false;
          }
          break;
        }
        default: return false;
      }
    }
    out.kind = 0;
    out.bytes = std::move(staged);
    out.blob_size = comp_size;
    out.blob_schema = 0;  // named payloads carry no schema; writer re-derives
    return true;
  }
  const PVal* size_v = cval.lookup("$size");
  const PVal* schema_v = cval.lookup("$schema");
  std::uint32_t size = 0;
  if (bytes == nullptr || bytes->kind != PVal::Kind::Str ||
      !U32Of(size_v, size) || size == 0 || size > 65536U ||
      schema_v == nullptr || schema_v->kind != PVal::Kind::Str ||
      schema_v->str.size() != 8) {
    return false;
  }
  if (bytes->str.size() != static_cast<std::size_t>(size) * 2U) {
    return false;
  }
  std::uint32_t file_schema = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    const int v = HexVal(schema_v->str[i]);
    if (v < 0) {
      return false;
    }
    file_schema = (file_schema << 4U) | static_cast<std::uint32_t>(v);
  }
  std::vector<std::uint8_t> decoded;
  decoded.reserve(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    const int hi = HexVal(bytes->str[2U * i]);
    const int lo = HexVal(bytes->str[2U * i + 1U]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    decoded.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  out.kind = 0;
  out.bytes = std::move(decoded);
  out.blob_size = size;
  out.blob_schema = file_schema;
  return true;
}

}  // namespace

// --- live-world component capture --------------------------------------------
// Reads one entity's full component image in asset-row form (forward
// declarations above; definitions here at namespace scope).

namespace {

// Envelope -> rows. Validates headers (format/version/entities), entity
// headers (id/name/components-or-PrefabRef), duplicate file ids.
auto ParsePrefabTextImpl(const std::string& text,
                         std::vector<PrefabBaseRow>& rows,
                         std::vector<std::string>& mounts) -> int32_t {
  PParser parser;
  parser.cur = text.data();
  parser.end = text.data() + text.size();
  PVal root;
  parser.parse_value(root);
  if (!parser.ok || root.kind != PVal::Kind::Obj) {
    return kPrefabBadFormat;
  }
  const PVal* format = root.lookup("format");
  const PVal* version = root.lookup("version");
  const PVal* entities = root.lookup("entities");
  if (format == nullptr || format->kind != PVal::Kind::Str ||
      format->str != "engine-prefab" || version == nullptr ||
      version->kind != PVal::Kind::Num || version->str != "1" ||
      entities == nullptr || entities->kind != PVal::Kind::Arr) {
    return kPrefabBadFormat;
  }
  std::vector<std::uint32_t> seen;
  seen.reserve(entities->items.size());
  for (const PVal& ent : entities->items) {
    if (ent.kind != PVal::Kind::Obj) {
      return kPrefabBadFormat;
    }
    const PVal* fid = ent.lookup("id");
    const PVal* fname = ent.lookup("name");
    std::uint32_t file_id = 0;
    if (!U32Of(fid, file_id) || file_id == 0 || fname == nullptr ||
        fname->kind != PVal::Kind::Str ||
        std::find(seen.begin(), seen.end(), file_id) != seen.end()) {
      return kPrefabBadFormat;
    }
    seen.push_back(file_id);
  }
  for (const PVal& ent : entities->items) {
    std::uint32_t file_id = 0;
    U32Of(ent.lookup("id"), file_id);
    PrefabBaseRow row;
    row.file_id = file_id;
    row.name = ent.lookup("name")->str;
    std::uint32_t parent = 0;
    U32Of(ent.lookup("parent"), parent);
    row.parent_file_id = parent;
    std::string mount;
    const PVal* ref = ent.lookup("PrefabRef");
    if (ref != nullptr && ref->kind == PVal::Kind::Obj) {
      const PVal* asset = ref->lookup("asset");
      if (asset != nullptr && asset->kind == PVal::Kind::Str &&
          !asset->str.empty()) {
        mount = asset->str;
      }
    }
    mounts.push_back(mount);
    rows.push_back(std::move(row));
  }
  return kPrefabOk;
}

// Attach decoded component payloads to rows: fills rows[i].comps from the
// parsed entities (needs describe for named user fields). Warns + skips
// bad components (never aborts — same rule as the scene reader).
auto FillRowComps(const PVal& entities, std::vector<PrefabBaseRow>& rows,
                  scene::SceneDescribeFn describe, void* user) -> void {
  for (std::size_t i = 0; i < entities.items.size() && i < rows.size();
       ++i) {
    const PVal& ent = entities.items[i];
    const PVal* fcomps = ent.lookup("components");
    if (fcomps == nullptr || fcomps->kind != PVal::Kind::Obj) {
      continue;
    }
    for (std::size_t c = 0; c < fcomps->keys.size(); ++c) {
      const std::string& cname = fcomps->keys[c];
      const PVal& cval = fcomps->values[c];
      if (cval.kind != PVal::Kind::Obj) {
        continue;
      }
      DecodedComp dc;
      if (!DecodePayload(cname, cval, describe, user, dc)) {
        std::printf("[WARN] prefab: bad component '%s' skipped\n",
                    cname.c_str());
        continue;
      }
      PrefabBaseComp bc;
      bc.name = cname;
      switch (dc.kind) {
        case 1:
          bc.bytes.assign(
              reinterpret_cast<const std::uint8_t*>(dc.pose.data()),
              reinterpret_cast<const std::uint8_t*>(dc.pose.data()) +
                  dc.pose.size() * sizeof(float));
          break;
        case 2:
          bc.bytes.assign(
              reinterpret_cast<const std::uint8_t*>(&dc.cam),
              reinterpret_cast<const std::uint8_t*>(&dc.cam) +
                  sizeof(dc.cam));
          break;
        case 3:
          bc.bytes.assign(
              reinterpret_cast<const std::uint8_t*>(&dc.mesh),
              reinterpret_cast<const std::uint8_t*>(&dc.mesh) +
                  sizeof(dc.mesh));
          break;
        case 4:
          bc.bytes.assign(
              reinterpret_cast<const std::uint8_t*>(&dc.light),
              reinterpret_cast<const std::uint8_t*>(&dc.light) +
                  sizeof(dc.light));
          break;
        default: bc.bytes = std::move(dc.bytes); break;
      }
      rows[i].comps.push_back(std::move(bc));
    }
  }
}

}  // namespace

auto parse_prefab_file(const char* path, scene::SceneDescribeFn describe, void* user,
                       std::vector<PrefabBaseRow>& out_rows,
                       std::vector<std::string>& out_mounts) noexcept
    -> int32_t {
  if (path == nullptr || path[0] == '\0') {
    return kPrefabNullArg;
  }
  std::string text;
  if (!scene::file_util::ReadAll(path, text)) {
    return kPrefabIoError;
  }
  // Parse once: envelope + headers validated here so FillRowComps below
  // can reuse the same tree (no second parse).
  PParser parser;
  parser.cur = text.data();
  parser.end = text.data() + text.size();
  PVal root;
  parser.parse_value(root);
  if (!parser.ok || root.kind != PVal::Kind::Obj) {
    return kPrefabBadFormat;
  }
  const PVal* entities = root.lookup("entities");
  const PVal* format = root.lookup("format");
  const PVal* version = root.lookup("version");
  if (format == nullptr || format->kind != PVal::Kind::Str ||
      format->str != "engine-prefab" || version == nullptr ||
      version->kind != PVal::Kind::Num || version->str != "1" ||
      entities == nullptr || entities->kind != PVal::Kind::Arr) {
    return kPrefabBadFormat;
  }
  std::vector<PrefabBaseRow> rows;
  std::vector<std::string> mounts;
  const int32_t rc = ParsePrefabTextImpl(text, rows, mounts);
  if (rc != kPrefabOk) {
    return rc;
  }
  FillRowComps(*entities, rows, describe, user);
  out_rows = std::move(rows);
  out_mounts = std::move(mounts);
  return kPrefabOk;
}

// --- envelope serializer ------------------------------------------------------
// Rows in asset id space -> {"format":"engine-prefab",...}. Component bytes
// re-encode through the same payload dialect the parser reads: pose /
// camera / mesh / light named payloads, user blobs as $bytes hex (schema
// re-derived from the live registration when available, else the stored
// $schema preserved through parse).

namespace {

struct SerializeCtx {
  ecs_world_t* world{nullptr};
  scene::SceneDescribeFn describe{nullptr};
  void* user{nullptr};
  // Live file-local id map for entity lanes (empty when serializing parsed
  // rows without a world: entity lanes keep their stored file ids).
  const std::vector<std::pair<std::uint64_t, std::uint32_t>>* id_map{nullptr};
};

// Encode one component's bytes (row image) as a JSON payload object.
auto AppendCompPayload(std::string& out, const std::string& cname,
                       const std::uint8_t* bytes, std::size_t size,
                       std::uint32_t schema, SerializeCtx& ctx) -> void {
  AppendJsonString(out, cname);
  out += ":{";
  if (cname == "LocalTransform" && size >= 40) {
    float pose[10]{};
    std::memcpy(pose, bytes, 40);
    out += "\"position\":";
    AppendFloatArray(out, pose, 3);
    out += ",\"rotation\":";
    AppendFloatArray(out, pose + 3, 4);
    out += ",\"scale\":";
    AppendFloatArray(out, pose + 7, 3);
    out.push_back('}');
    return;
  }
  if (cname == "Camera" && size >= sizeof(engine::ecs::Camera)) {
    engine::ecs::Camera cam{};
    std::memcpy(&cam, bytes, sizeof(cam));
    out += "\"fov_y_rad\":";
    AppendFloat(out, cam.fov_y_rad);
    out += ",\"near_z\":";
    AppendFloat(out, cam.near_z);
    out += ",\"far_z\":";
    AppendFloat(out, cam.far_z);
    out += ",\"is_main\":";
    AppendU32(out, cam.is_main);
    out.push_back('}');
    return;
  }
  if (cname == "MeshRenderer" && size >= sizeof(engine::ecs::MeshRenderer)) {
    engine::ecs::MeshRenderer mr{};
    std::memcpy(&mr, bytes, sizeof(mr));
    out += "\"mesh\":";
    AppendJsonString(out, MeshRefFor(mr.mesh_id));
    out += ",\"base_color\":";
    AppendFloatArray(out, mr.base_color, 4);
    out.push_back('}');
    return;
  }
  if (cname == "DirectionalLight" &&
      size >= sizeof(engine::ecs::DirectionalLight)) {
    engine::ecs::DirectionalLight li{};
    std::memcpy(&li, bytes, sizeof(li));
    out += "\"direction\":";
    AppendFloatArray(out, li.direction, 3);
    out += ",\"intensity\":";
    AppendFloat(out, li.shadow);
    out.push_back('}');
    return;
  }
  // User component: named fields when a live description exists, else blob.
  bool use_named = false;
  const scene::SceneFieldDesc* desc_fields = nullptr;
  std::uint32_t desc_count = 0;
  if (ctx.describe != nullptr) {
    desc_fields = ctx.describe(cname.c_str(), &desc_count, ctx.user);
    use_named = (desc_fields != nullptr && desc_count > 0);
  }
  if (use_named) {
    std::function<std::uint32_t(std::uint64_t)> file_id =
        [&](std::uint64_t runtime) -> std::uint32_t {
      if (ctx.id_map != nullptr) {
        for (const auto& kv : *ctx.id_map) {
          if (kv.first == runtime) {
            return kv.second;
          }
        }
        return 0;
      }
      // No world: stored image keeps file ids inline (u64 lane value).
      return static_cast<std::uint32_t>(runtime & 0xFFFFFFFFU);
    };
    std::string named;
    if (AppendNamedComponent(named, desc_fields, desc_count, bytes,
                             static_cast<std::uint32_t>(size), file_id)) {
      out += named;
      out.push_back('}');
      return;
    }
  }
  // $bytes blob: size + schema. Schema re-derives from the live
  // registration when the world knows the component; otherwise the
  // stored schema (parsed files) round-trips verbatim.
  std::uint32_t out_schema = schema;
  if (ctx.world != nullptr) {
    const ecs_entity_t comp =
        scene::lookup_component(ctx.world, cname.c_str());
    if (comp != 0) {
      const ecs_type_info_t* ti = ecs_get_type_info(ctx.world, comp);
      if (ti != nullptr && ti->size > 0 &&
          static_cast<std::size_t>(ti->size) == size) {
        out_schema = SchemaHash(cname.c_str(), static_cast<std::uint32_t>(size),
                                static_cast<std::uint32_t>(ti->alignment));
      }
    }
  }
  out += "\"$bytes\":\"";
  AppendHex(out, bytes, size);
  out += "\",\"$size\":";
  AppendU32(out, static_cast<std::uint32_t>(size));
  out += ",\"$schema\":\"";
  std::array<char, 9> hex{};
  auto res = std::to_chars(hex.data(), hex.data() + 8, out_schema, 16);
  std::size_t digits = static_cast<std::size_t>(res.ptr - hex.data());
  for (std::size_t p = digits; p < 8U; ++p) {
    out.push_back('0');
  }
  out.append(hex.data(), res.ptr);
  out += "\"}";
}

}  // namespace

auto serialize_prefab_rows(const std::vector<PrefabBaseRow>& rows,
                           const std::vector<std::string>& mounts,
                           std::string& out) noexcept -> int32_t {
  SerializeCtx ctx;
  out = "{\"format\":\"engine-prefab\",\"version\":1,\"entities\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const PrefabBaseRow& row = rows[i];
    if (i != 0U) {
      out.push_back(',');
    }
    out += "{\"id\":";
    AppendU32(out, row.file_id);
    out += ",\"name\":";
    AppendJsonString(out, row.name);
    out += ",\"parent\":";
    AppendU32(out, row.parent_file_id);
    const std::string mount =
        (i < mounts.size()) ? mounts[i] : std::string();
    if (!mount.empty()) {
      out += ",\"PrefabRef\":{\"asset\":";
      AppendJsonString(out, mount);
      out += "}";
    }
    out += ",\"components\":{";
    bool first = true;
    for (const PrefabBaseComp& comp : row.comps) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      std::uint32_t schema = SchemaHash(comp.name.c_str(),
                                        static_cast<std::uint32_t>(comp.bytes.size()), 4);
      AppendCompPayload(out, comp.name, comp.bytes.data(), comp.bytes.size(),
                        schema, ctx);
    }
    out += "}}";
  }
  out += "]}";
  return kPrefabOk;
}

// --- registry -------------------------------------------------------------------

auto PrefabRegistry::anchor_for(ecs_world_t* world,
                                const std::string& asset) noexcept
    -> std::uint64_t {
  if (world == nullptr || asset.empty()) {
    return 0;
  }
  for (const auto& kv : anchors_) {
    if (kv.first == asset) {
      return kv.second;
    }
  }
  ecs_entity_desc_t desc{};
  // Anonymous prefab anchor (unnamed: never collides, never serialized —
  // the scene writer only walks LocalTransform carriers).
  desc.name = nullptr;
  const ecs_entity_t e = ecs_entity_init(world, &desc);
  if (e == 0) {
    return 0;
  }
  ecs_add_id(world, e, EcsPrefab);
  const auto anchor = static_cast<std::uint64_t>(e);
  anchors_.emplace_back(asset, anchor);
  return anchor;
}

auto PrefabRegistry::asset_for_anchor(std::uint64_t anchor) const noexcept
    -> std::string {
  for (const auto& kv : anchors_) {
    if (kv.second == anchor) {
      return kv.first;
    }
  }
  return {};
}

auto PrefabRegistry::is_instance_root(ecs_world_t* world,
                                      std::uint64_t entity) const noexcept
    -> bool {
  if (world == nullptr || entity == 0) {
    return false;
  }
  const auto e = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, e)) {
    return false;
  }
  const ecs_entity_t base = ecs_get_target(world, e, EcsIsA, 0);
  if (base == 0) {
    return false;
  }
  for (const auto& kv : anchors_) {
    if (kv.second == static_cast<std::uint64_t>(base)) {
      return true;
    }
  }
  return false;
}

auto PrefabRegistry::asset_for_instance(std::uint64_t root) const noexcept
    -> std::string {
  for (const auto& inst : instances_) {
    if (inst.root == root) {
      return inst.asset;
    }
  }
  return {};
}

auto PrefabRegistry::register_instance(std::uint64_t root,
                                       const std::string& asset,
                                       PrefabBaseImage image) noexcept
    -> void {
  for (auto& inst : instances_) {
    if (inst.root == root) {
      inst.asset = asset;
      inst.base = std::move(image);
      return;
    }
  }
  Instance inst;
  inst.root = root;
  inst.asset = asset;
  inst.base = std::move(image);
  instances_.push_back(std::move(inst));
}

auto PrefabRegistry::base_for(std::uint64_t root) const noexcept
    -> const PrefabBaseImage* {
  for (const auto& inst : instances_) {
    if (inst.root == root) {
      return &inst.base;
    }
  }
  return nullptr;
}

auto PrefabRegistry::base_for_mut(std::uint64_t root) noexcept
    -> PrefabBaseImage* {
  for (auto& inst : instances_) {
    if (inst.root == root) {
      return &inst.base;
    }
  }
  return nullptr;
}

auto PrefabRegistry::unregister(std::uint64_t root) noexcept -> void {
  for (std::size_t i = 0; i < instances_.size(); ++i) {
    if (instances_[i].root == root) {
      instances_.erase(instances_.begin() +
                       static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

auto PrefabRegistry::instances_of(ecs_world_t* world,
                                  const std::string& asset) noexcept
    -> std::vector<std::uint64_t> {
  std::vector<std::uint64_t> out;
  for (std::size_t i = 0; i < instances_.size();) {
    if (instances_[i].asset != asset) {
      ++i;
      continue;
    }
    const auto e = static_cast<ecs_entity_t>(instances_[i].root);
    if (world != nullptr && !ecs_is_alive(world, e)) {
      instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    out.push_back(instances_[i].root);
    ++i;
  }
  return out;
}

// --- materialize / capture / merge ------------------------------------------------
// Rows (asset id space) <-> live entities. The materializer replays decoded
// rows into fresh entities: pass 1 creates entities + components, pass 2
// replays parent links (same two-pass shape as the scene reader, including
// its cycle guard). Entity-lane fixups inside user blobs resolve through
// the file-local id map at materialize time.

namespace {

// Ensure the built-ins resolve (same register-on-demand rule as the scene
// reader: a world without register_render() would silently drop them).
auto EnsureBuiltins(ecs_world_t* world) noexcept -> bool {
  const ecs_entity_t cam_id = scene::lookup_component(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id =
      scene::lookup_component(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      scene::lookup_component(world, "Engine.Runtime.DirectionalLight");
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
  return true;
}

// Register-or-fetch a user component for blob replay (same rule as the
// scene reader: existing layout wins, unknown names recover alignment
// from the file $schema over powers of two, mismatch -> 0).
auto UserComponent(ecs_world_t* world, const char* name, std::uint32_t size,
                   std::uint32_t file_schema) noexcept -> ecs_entity_t {
  const ecs_entity_t existing = scene::lookup_component(world, name);
  if (existing != 0) {
    const ecs_type_info_t* ti = ecs_get_type_info(world, existing);
    if (ti == nullptr || ti->size <= 0 ||
        static_cast<std::uint32_t>(ti->size) != size) {
      return 0;
    }
    if (file_schema != 0 &&
        file_schema !=
            SchemaHash(name, size, static_cast<std::uint32_t>(ti->alignment))) {
      return 0;
    }
    return existing;
  }
  std::uint32_t align = 0;
  for (std::uint32_t a = 1; a <= size && a <= 64U; a <<= 1U) {
    if (SchemaHash(name, size, a) == file_schema) {
      align = a;
      break;
    }
  }
  if (align == 0 && file_schema == 0) {
    align = 4;  // named payloads carry no schema: 4-byte lanes by contract
  }
  if (align == 0) {
    return 0;
  }
  return engine::ecs::RegisterRenderComponent(world, name, size, align);
}

struct Fixup {
  ecs_entity_t e;
  ecs_entity_t comp;
  std::uint32_t offset;
  std::uint32_t file_id;  // file-local target (0 = null, stays zero)
};

// Apply one row's components to a fresh entity. Blob schemas come from the
// parsed payload (stored alongside bytes at parse: blob_size/schema are
// re-derived here from the row's byte length — see note below).
auto ApplyRowComps(ecs_world_t* world, ecs_entity_t e, const PrefabBaseRow& row,
                   scene::SceneDescribeFn describe, void* user,
                   std::vector<Fixup>& fixups) -> void {
  EnsureBuiltins(world);
  const ecs_entity_t cam_id = scene::lookup_component(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id =
      scene::lookup_component(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      scene::lookup_component(world, "Engine.Runtime.DirectionalLight");
  for (const PrefabBaseComp& comp : row.comps) {
    if (comp.name == "LocalTransform" && comp.bytes.size() >= 40) {
      float pose[10]{};
      std::memcpy(pose, comp.bytes.data(), 40);
      scene::write_local_pose(world, e, pose);
    } else if (comp.name == "Camera" && cam_id != 0 &&
               comp.bytes.size() >= sizeof(engine::ecs::Camera)) {
      engine::ecs::Camera cam{};
      std::memcpy(&cam, comp.bytes.data(), sizeof(cam));
      ecs_set_id(world, e, cam_id, sizeof(cam), &cam);
    } else if (comp.name == "MeshRenderer" && mesh_id != 0 &&
               comp.bytes.size() >= sizeof(engine::ecs::MeshRenderer)) {
      engine::ecs::MeshRenderer mr{};
      std::memcpy(&mr, comp.bytes.data(), sizeof(mr));
      ecs_set_id(world, e, mesh_id, sizeof(mr), &mr);
    } else if (comp.name == "DirectionalLight" && light_id != 0 &&
               comp.bytes.size() >= sizeof(engine::ecs::DirectionalLight)) {
      engine::ecs::DirectionalLight li{};
      std::memcpy(&li, comp.bytes.data(), sizeof(li));
      ecs_set_id(world, e, light_id, sizeof(li), &li);
    } else if (comp.name == "WorldTransform") {
      continue;
    } else {
      // User blob: needs describe for entity-lane fixups. The stored row
      // keeps raw bytes; entity lanes (type 9) hold file-local ids that
      // remap through the materialize id map below.
      const auto size = static_cast<std::uint32_t>(comp.bytes.size());
      if (size == 0 || size > 65536U) {
        continue;
      }
      std::uint32_t desc_count = 0;
      const scene::SceneFieldDesc* desc_fields = nullptr;
      if (describe != nullptr) {
        desc_fields = describe(comp.name.c_str(), &desc_count, user);
      }
      ecs_entity_t target = 0;
      if (desc_fields != nullptr && desc_count > 0) {
        const ecs_entity_t existing = scene::lookup_component(world, comp.name.c_str());
        if (existing != 0) {
          const ecs_type_info_t* ti = ecs_get_type_info(world, existing);
          if (ti != nullptr && ti->size > 0 &&
              static_cast<std::uint32_t>(ti->size) == size) {
            target = existing;
            // Record entity-lane fixups before writing.
            for (std::uint32_t fi = 0; fi < desc_count; ++fi) {
              const scene::SceneFieldDesc& f = desc_fields[fi];
              if (f.type == 9 && f.offset + 8 <= size &&
                  f.offset < comp.bytes.size()) {
                std::uint64_t stored = 0;
                std::memcpy(&stored, comp.bytes.data() + f.offset, 8);
                const auto ref = static_cast<std::uint32_t>(stored & 0xFFFFFFFFU);
                if (ref != 0) {
                  Fixup fx;
                  fx.e = e;
                  fx.comp = existing;
                  fx.offset = f.offset;
                  fx.file_id = ref;
                  fixups.push_back(fx);
                }
              }
            }
          }
        }
      }
      if (target == 0) {
        // Blob path: schema from the stored hash when available. Rows
        // parsed from files carry blob bytes verbatim; alignment recovers
        // from the payload schema (0 for named payloads -> 4).
        target = UserComponent(world, comp.name.c_str(), size, 0);
        if (target == 0) {
          std::printf("[WARN] prefab: bad user component '%s' skipped\n",
                      comp.name.c_str());
          continue;
        }
      }
      ecs_set_id(world, e, target, size, comp.bytes.data());
    }
  }
  ecs_add_id(world, e, ecs_lookup(world, "WorldTransform"));
}

// Capture one live entity's component image into row form.
auto CaptureRow(ecs_world_t* world, ecs_entity_t e, std::uint32_t file_id,
                std::uint32_t parent_file_id) -> PrefabBaseRow {
  PrefabBaseRow row;
  row.file_id = file_id;
  const char* n = ecs_get_name(world, e);
  row.name = (n != nullptr) ? n : "";
  row.parent_file_id = parent_file_id;
  float pose[10]{};
  if (scene::read_local_pose(world, e, pose)) {
    PrefabBaseComp bc;
    bc.name = "LocalTransform";
    bc.bytes.assign(reinterpret_cast<const std::uint8_t*>(pose),
                    reinterpret_cast<const std::uint8_t*>(pose) + 40);
    row.comps.push_back(std::move(bc));
  }
  const ecs_entity_t cam_id = scene::lookup_component(world, "Engine.Runtime.Camera");
  if (cam_id != 0) {
    const auto* cam = static_cast<const engine::ecs::Camera*>(
        ecs_get_id(world, e, cam_id));
    if (cam != nullptr) {
      PrefabBaseComp bc;
      bc.name = "Camera";
      bc.bytes.assign(reinterpret_cast<const std::uint8_t*>(cam),
                      reinterpret_cast<const std::uint8_t*>(cam) + sizeof(*cam));
      row.comps.push_back(std::move(bc));
    }
  }
  const ecs_entity_t mesh_id =
      scene::lookup_component(world, "Engine.Runtime.MeshRenderer");
  if (mesh_id != 0) {
    const auto* mr = static_cast<const engine::ecs::MeshRenderer*>(
        ecs_get_id(world, e, mesh_id));
    if (mr != nullptr) {
      PrefabBaseComp bc;
      bc.name = "MeshRenderer";
      bc.bytes.assign(reinterpret_cast<const std::uint8_t*>(mr),
                      reinterpret_cast<const std::uint8_t*>(mr) + sizeof(*mr));
      row.comps.push_back(std::move(bc));
    }
  }
  const ecs_entity_t light_id =
      scene::lookup_component(world, "Engine.Runtime.DirectionalLight");
  if (light_id != 0) {
    const auto* li = static_cast<const engine::ecs::DirectionalLight*>(
        ecs_get_id(world, e, light_id));
    if (li != nullptr) {
      PrefabBaseComp bc;
      bc.name = "DirectionalLight";
      bc.bytes.assign(reinterpret_cast<const std::uint8_t*>(li),
                      reinterpret_cast<const std::uint8_t*>(li) + sizeof(*li));
      row.comps.push_back(std::move(bc));
    }
  }
  for (scene::SceneComp sc : scene::enum_data_components(world, e)) {
    PrefabBaseComp bc;
    bc.name = std::move(sc.name);
    bc.bytes = std::move(sc.bytes);
    row.comps.push_back(std::move(bc));
  }
  return row;
}

}  // namespace

// --- instantiate ------------------------------------------------------------------
// Recursive materialize with a cycle guard: `stack` holds asset paths on the
// current instantiate chain; a mount whose asset is already stacked
// materializes as an empty mount point (warns, kPrefabOk overall — one bad
// mount never aborts the whole instantiate).

namespace {

struct MaterializeCtx {
  scene::SceneDescribeFn describe{nullptr};
  void* user{nullptr};
  PrefabRegistry* registry{nullptr};
  std::vector<std::string> stack;  // asset paths on the current chain
  std::uint32_t next_file_id{1};
};

// Forward: materialize rows into `world` under `parent_live`.
auto MaterializeRows(ecs_world_t* world,
                     const std::vector<PrefabBaseRow>& rows,
                     const std::vector<std::string>& mounts,
                     ecs_entity_t parent_live, MaterializeCtx& ctx,
                     std::vector<std::pair<std::uint32_t, ecs_entity_t>>& id_map,
                     std::vector<Fixup>& fixups, ecs_entity_t& first_root)
    -> int32_t;

auto MaterializeRows(ecs_world_t* world,
                     const std::vector<PrefabBaseRow>& rows,
                     const std::vector<std::string>& mounts,
                     ecs_entity_t parent_live, MaterializeCtx& ctx,
                     std::vector<std::pair<std::uint32_t, ecs_entity_t>>& id_map,
                     std::vector<Fixup>& fixups, ecs_entity_t& first_root)
    -> int32_t {
  // Pass 1: create entities + components (parents in pass 2).
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const PrefabBaseRow& row = rows[i];
    const std::string mount =
        (i < mounts.size()) ? mounts[i] : std::string();
    const ecs_entity_t e = ecs_new(world);
    if (e == 0) {
      return kPrefabIoError;
    }
    ecs_set_name(world, e, row.name.c_str());
    id_map.emplace_back(row.file_id, e);
    if (first_root == 0 && row.parent_file_id == 0) {
      first_root = e;
    }
    if (!mount.empty()) {
      // Nested mount: recurse into the nested asset, then attach its root
      // under this mount point. The mount entity itself stays a plain
      // named node (carries the mount's own components, if any).
      ApplyRowComps(world, e, row, ctx.describe, ctx.user, fixups);
      bool cycled = false;
      for (const std::string& s : ctx.stack) {
        if (s == mount) {
          cycled = true;
          break;
        }
      }
      if (cycled) {
        std::printf("[WARN] prefab: nested mount cycle at '%s' skipped\n",
                    mount.c_str());
        continue;
      }
      std::vector<PrefabBaseRow> nrows;
      std::vector<std::string> nmounts;
      const int32_t prc =
          parse_prefab_file(mount.c_str(), ctx.describe, ctx.user, nrows, nmounts);
      if (prc != kPrefabOk) {
        std::printf("[WARN] prefab: nested asset '%s' unreadable, mount skipped\n",
                    mount.c_str());
        continue;
      }
      const std::uint64_t anchor =
          ctx.registry->anchor_for(world, mount);
      ctx.stack.push_back(mount);
      ecs_entity_t nested_root = 0;
      std::vector<std::pair<std::uint32_t, ecs_entity_t>> sub_map;
      const int32_t mrc = MaterializeRows(world, nrows, nmounts, e, ctx,
                                          sub_map, fixups, nested_root);
      ctx.stack.pop_back();
      if (mrc != kPrefabOk || nested_root == 0 || anchor == 0) {
        continue;
      }
      // Merge the nested id map into ours (file ids are asset-local; the
      // nested asset shares the same numbering — remap is per-asset so no
      // collision escapes this call).
      for (const auto& kv : sub_map) {
        id_map.push_back(kv);
      }
      ecs_add_pair(world, nested_root, EcsIsA,
                   static_cast<ecs_entity_t>(anchor));
      // Base image for the nested instance (asset id space).
      PrefabBaseImage image;
      image.rows = nrows;
      ctx.registry->register_instance(nested_root, mount, std::move(image));
      continue;
    }
    ApplyRowComps(world, e, row, ctx.describe, ctx.user, fixups);
  }
  // Pass 2: parent links (file-local -> live), with the reader's cycle
  // guard (self/unknown/cycle -> skip with warning).
  std::vector<std::uint32_t> file_parents(rows.size(), 0);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    file_parents[i] = rows[i].parent_file_id;
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::uint32_t parent_file = file_parents[i];
    ecs_entity_t self_live = 0;
    for (const auto& kv : id_map) {
      if (kv.first == rows[i].file_id) {
        self_live = kv.second;
        break;
      }
    }
    if (self_live == 0) {
      continue;
    }
    if (parent_file == 0) {
      if (parent_live != 0) {
        ecs_add_pair(world, self_live, EcsChildOf, parent_live);
      }
      continue;
    }
    ecs_entity_t parent_live_link = 0;
    for (const auto& kv : id_map) {
      if (kv.first == parent_file) {
        parent_live_link = kv.second;
        break;
      }
    }
    bool cycle = (parent_live_link == self_live) || (parent_live_link == 0);
    if (!cycle) {
      std::uint32_t cursor = parent_file;
      for (std::size_t steps = 0; steps <= file_parents.size(); ++steps) {
        if (cursor == rows[i].file_id) {
          cycle = true;
          break;
        }
        std::uint32_t next = 0;
        for (std::size_t j = 0; j < rows.size(); ++j) {
          if (rows[j].file_id == cursor && j < file_parents.size()) {
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
      std::printf("[WARN] prefab: parent link skipped (cycle or unknown parent %u)\n",
                  parent_file);
    } else {
      ecs_add_pair(world, self_live, EcsChildOf, parent_live_link);
    }
  }
  return kPrefabOk;
}

}  // namespace

auto instantiate_prefab(ecs_world_t* world, const char* prefab_path,
                        scene::SceneDescribeFn describe, void* describe_user,
                        PrefabRegistry& registry, std::uint64_t* out_root) noexcept
    -> int32_t {
  if (world == nullptr || prefab_path == nullptr || prefab_path[0] == '\0' ||
      out_root == nullptr) {
    return kPrefabNullArg;
  }
  if (ecs_lookup(world, "LocalTransform") == 0) {
    return kPrefabNoLocal;
  }
  std::vector<PrefabBaseRow> rows;
  std::vector<std::string> mounts;
  const int32_t prc =
      parse_prefab_file(prefab_path, describe, describe_user, rows, mounts);
  if (prc != kPrefabOk) {
    return prc;
  }
  if (rows.empty()) {
    return kPrefabBadFormat;
  }
  const std::uint64_t anchor = registry.anchor_for(world, prefab_path);
  if (anchor == 0) {
    return kPrefabIoError;
  }
  MaterializeCtx ctx;
  ctx.describe = describe;
  ctx.user = describe_user;
  ctx.registry = &registry;
  ctx.stack.push_back(prefab_path);
  std::vector<std::pair<std::uint32_t, ecs_entity_t>> id_map;
  std::vector<Fixup> fixups;
  ecs_entity_t root = 0;
  const int32_t mrc =
      MaterializeRows(world, rows, mounts, 0, ctx, id_map, fixups, root);
  ctx.stack.pop_back();
  if (mrc != kPrefabOk || root == 0) {
    return (mrc != kPrefabOk) ? mrc : kPrefabIoError;
  }
  // Entity-lane fixups now that every entity exists.
  for (const Fixup& fx : fixups) {
    ecs_entity_t target = 0;
    for (const auto& kv : id_map) {
      if (kv.first == fx.file_id) {
        target = kv.second;
        break;
      }
    }
    if (target == 0) {
      continue;
    }
    auto* mut =
        static_cast<std::uint8_t*>(ecs_get_mut_id(world, fx.e, fx.comp));
    if (mut == nullptr) {
      continue;
    }
    const auto raw = static_cast<std::uint64_t>(target);
    std::memcpy(mut + fx.offset, &raw, 8);
  }
  ecs_add_pair(world, root, EcsIsA, static_cast<ecs_entity_t>(anchor));
  PrefabBaseImage image;
  image.rows = rows;
  registry.register_instance(root, prefab_path, std::move(image));
  *out_root = root;
  return kPrefabOk;
}

// --- update / apply / revert / save ------------------------------------------------
// Live-instance <-> asset-file sync. Instance members are found by walking
// the live subtree of each registered root; members match base rows by
// (name, sibling order) — the same stable key the writer's ordering gives.
// Nested-instance subtrees are owned by their own asset: the parent merge
// skips them (they update when their own asset updates).

namespace {

// Collect live member entities under `root` (root first, then
// parents-before-children), EXCLUDING nested-instance subtrees (a child
// that is itself a registered instance root starts its own tree).
auto CollectMembers(ecs_world_t* world, std::uint64_t root,
                    PrefabRegistry& registry) -> std::vector<ecs_entity_t> {
  std::vector<ecs_entity_t> out;
  const auto root_ent = static_cast<ecs_entity_t>(root);
  if (!ecs_is_alive(world, root_ent)) {
    return out;
  }
  std::vector<scene::SceneRow> rows = scene::collect_subtree_rows(world, root);
  for (const scene::SceneRow& row : rows) {
    const auto e = static_cast<ecs_entity_t>(row.entity);
    if (row.entity != root && registry.asset_for_instance(row.entity).empty() == false) {
      continue;  // nested instance root: owned by its own asset
    }
    // Also skip everything UNDER a nested root: walk up to check.
    bool under_nested = false;
    ecs_entity_t cursor = e;
    for (std::size_t steps = 0; steps <= rows.size(); ++steps) {
      if (cursor == root_ent) {
        break;
      }
      const ecs_entity_t parent = ecs_get_target(world, cursor, EcsChildOf, 0);
      if (parent == 0) {
        break;
      }
      if (parent != root_ent &&
          registry.asset_for_instance(static_cast<std::uint64_t>(parent))
                  .empty() == false) {
        under_nested = true;
        break;
      }
      cursor = parent;
    }
    if (!under_nested) {
      out.push_back(e);
    }
  }
  return out;
}

// Match live members to base rows by (name, sibling order): group base
// rows by parent file id, group live members by live parent, order each
// group by creation order (entity id), match by index with a name check.
// Returns live entity -> row index (rows.size() = unmatched/new).
auto MatchMembers(const std::vector<ecs_entity_t>& members,
                  const std::vector<PrefabBaseRow>& rows,
                  ecs_world_t* world) -> std::vector<std::size_t> {
  std::vector<std::size_t> match(members.size(), rows.size());
  // Base rows grouped by parent file id (creation order = file order).
  // Live members grouped by parent entity.
  for (std::size_t m = 0; m < members.size(); ++m) {
    const ecs_entity_t parent =
        ecs_get_target(world, members[m], EcsChildOf, 0);
    // Sibling index of this member among live siblings (by entity id).
    std::size_t sib_idx = 0;
    const char* mname = ecs_get_name(world, members[m]);
    std::string mns = (mname != nullptr) ? mname : "";
    // Count live siblings with the same parent created before this one.
    for (std::size_t k = 0; k < members.size(); ++k) {
      if (k == m) {
        continue;
      }
      if (ecs_get_target(world, members[k], EcsChildOf, 0) == parent &&
          static_cast<std::uint64_t>(members[k]) <
              static_cast<std::uint64_t>(members[m])) {
        // Only count same-name siblings (name check below is per index).
        const char* kn = ecs_get_name(world, members[k]);
        if (mns == ((kn != nullptr) ? kn : "")) {
          ++sib_idx;
        }
      }
    }
    // Find the sib_idx-th base row with this name whose parent matches.
    std::size_t seen = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
      if (rows[r].name != mns) {
        continue;
      }
      // Parent-name check: base parent row's name vs live parent's name.
      // Roots match by name alone: a nested instance root's live parent
      // is its mount point (outside the nested asset), and a top-level
      // instance root's live parent is whatever the scene holds — the
      // asset always records parent 0 for its root. Non-root rows still
      // require the parent-name check so same-named siblings in different
      // subtrees don't cross-match.
      std::string bpar;
      for (const PrefabBaseRow& pr : rows) {
        if (pr.file_id == rows[r].parent_file_id) {
          bpar = pr.name;
          break;
        }
      }
      const bool row_is_root = (rows[r].parent_file_id == 0);
      if (!row_is_root) {
        std::string lpar;
        if (parent != 0) {
          const char* pn = ecs_get_name(world, parent);
          lpar = (pn != nullptr) ? pn : "";
        }
        if (bpar != lpar) {
          continue;
        }
      }
      if (seen == sib_idx) {
        // Claim check: one row per member.
        bool taken = false;
        for (std::size_t mm = 0; mm < m; ++mm) {
          if (match[mm] == r) {
            taken = true;
            break;
          }
        }
        if (!taken) {
          match[m] = r;
        }
        break;
      }
      ++seen;
    }
  }
  return match;
}

// Read one live entity's component bytes by name (pose under
// "LocalTransform", named built-ins, user blobs). Empty when absent.
auto LiveCompBytes(ecs_world_t* world, ecs_entity_t e, const std::string& name)
    -> std::vector<std::uint8_t> {
  if (name == "LocalTransform") {
    float pose[10]{};
    if (!scene::read_local_pose(world, e, pose)) {
      return {};
    }
    std::vector<std::uint8_t> b(40);
    std::memcpy(b.data(), pose, 40);
    return b;
  }
  ecs_entity_t comp = 0;
  std::size_t expect = 0;
  if (name == "Camera") {
    comp = scene::lookup_component(world, "Engine.Runtime.Camera");
    expect = sizeof(engine::ecs::Camera);
  } else if (name == "MeshRenderer") {
    comp = scene::lookup_component(world, "Engine.Runtime.MeshRenderer");
    expect = sizeof(engine::ecs::MeshRenderer);
  } else if (name == "DirectionalLight") {
    comp = scene::lookup_component(world, "Engine.Runtime.DirectionalLight");
    expect = sizeof(engine::ecs::DirectionalLight);
  } else {
    comp = scene::lookup_component(world, name.c_str());
  }
  if (comp == 0) {
    return {};
  }
  const ecs_type_info_t* ti = ecs_get_type_info(world, comp);
  if (ti == nullptr || ti->size <= 0) {
    return {};
  }
  if (expect != 0 && static_cast<std::size_t>(ti->size) != expect) {
    return {};
  }
  const auto* src =
      static_cast<const std::uint8_t*>(ecs_get_id(world, e, comp));
  if (src == nullptr) {
    return {};
  }
  const auto size = static_cast<std::size_t>(ti->size);
  return std::vector<std::uint8_t>(src, src + size);
}

// Write raw bytes to one live entity's component (creates the component
// when missing: register-on-demand for user blobs via the live layout).
auto WriteCompBytes(ecs_world_t* world, ecs_entity_t e, const std::string& name,
                    const std::vector<std::uint8_t>& bytes) -> bool {
  if (bytes.empty()) {
    return false;
  }
  if (name == "LocalTransform" && bytes.size() >= 40) {
    float pose[10]{};
    std::memcpy(pose, bytes.data(), 40);
    return scene::write_local_pose(world, e, pose);
  }
  ecs_entity_t comp = 0;
  if (name == "Camera") {
    comp = scene::lookup_component(world, "Engine.Runtime.Camera");
  } else if (name == "MeshRenderer") {
    comp = scene::lookup_component(world, "Engine.Runtime.MeshRenderer");
  } else if (name == "DirectionalLight") {
    comp = scene::lookup_component(world, "Engine.Runtime.DirectionalLight");
  } else {
    comp = scene::lookup_component(world, name.c_str());
    if (comp == 0) {
      comp = engine::ecs::RegisterRenderComponent(
          world, name.c_str(),
          static_cast<std::uint32_t>(bytes.size()), 4);
    }
  }
  if (comp == 0) {
    return false;
  }
  const ecs_type_info_t* ti = ecs_get_type_info(world, comp);
  if (ti == nullptr || ti->size <= 0 ||
      static_cast<std::size_t>(ti->size) != bytes.size()) {
    return false;
  }
  ecs_set_id(world, e, comp, bytes.size(), bytes.data());
  return true;
}

}  // namespace

auto update_prefab_instances(ecs_world_t* world, const char* prefab_path,
                             scene::SceneDescribeFn describe, void* describe_user,
                             PrefabRegistry& registry,
                             std::uint64_t* out_updated) noexcept -> int32_t {
  if (world == nullptr || prefab_path == nullptr || prefab_path[0] == '\0' ||
      out_updated == nullptr) {
    return kPrefabNullArg;
  }
  // Parse the new asset BEFORE touching any instance (file errors abort
  // with instances untouched).
  std::vector<PrefabBaseRow> new_rows;
  std::vector<std::string> new_mounts;
  const int32_t prc = parse_prefab_file(prefab_path, describe, describe_user,
                                        new_rows, new_mounts);
  if (prc != kPrefabOk) {
    return prc;
  }
  std::vector<std::uint64_t> roots = registry.instances_of(world, prefab_path);
  std::uint64_t updated = 0;
  for (std::uint64_t root : roots) {
    const auto root_ent = static_cast<ecs_entity_t>(root);
    if (!ecs_is_alive(world, root_ent)) {
      continue;
    }
    PrefabBaseImage* base = registry.base_for_mut(root);
    if (base == nullptr) {
      continue;
    }
    std::vector<ecs_entity_t> members = CollectMembers(world, root, registry);
    std::vector<std::size_t> match = MatchMembers(members, base->rows, world);
    // Index new rows by file id for the merge target lookup.
    for (std::size_t m = 0; m < members.size(); ++m) {
      if (match[m] >= base->rows.size()) {
        continue;  // user-added member: not tracked, left alone
      }
      const PrefabBaseRow& Brow = base->rows[match[m]];
      // New row with the same file id (structure preserved) — else the
      // member's base row is gone (asset deleted it): keep live as-is.
      const PrefabBaseRow* Nrow = nullptr;
      for (const PrefabBaseRow& nr : new_rows) {
        if (nr.file_id == Brow.file_id) {
          Nrow = &nr;
          break;
        }
      }
      if (Nrow == nullptr) {
        continue;
      }
      // Per-field three-way merge. Field ranges come from the live
      // description when available (entity/asset-ref lanes merge whole —
      // ids are remapped at materialize, not merged); otherwise the
      // component merges in 4-byte lanes (float/int lanes by contract).
      // new_lane = (live_lane == base_lane) ? asset_lane : live_lane.
      for (const PrefabBaseComp& ncomp : Nrow->comps) {
        const PrefabBaseComp* bcomp = nullptr;
        for (const PrefabBaseComp& bc : Brow.comps) {
          if (bc.name == ncomp.name) {
            bcomp = &bc;
            break;
          }
        }
        std::vector<std::uint8_t> live =
            LiveCompBytes(world, members[m], ncomp.name);
        if (bcomp == nullptr) {
          // Component is new in the asset: take it when the live row
          // doesn't carry it (else the user added the same component —
          // keep live).
          if (live.empty()) {
            WriteCompBytes(world, members[m], ncomp.name, ncomp.bytes);
          }
          continue;
        }
        if (live.size() != bcomp->bytes.size() ||
            live.size() != ncomp.bytes.size()) {
          // Size drift (user swapped the component width): take the asset
          // image when live still equals base, else keep live.
          if (live == bcomp->bytes) {
            WriteCompBytes(world, members[m], ncomp.name, ncomp.bytes);
          }
          continue;
        }
        if (live == bcomp->bytes) {
          WriteCompBytes(world, members[m], ncomp.name, ncomp.bytes);
          continue;
        }
        // Field ranges for this component.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        std::uint32_t desc_count = 0;
        const scene::SceneFieldDesc* desc_fields = nullptr;
        if (describe != nullptr) {
          desc_fields =
              describe(ncomp.name.c_str(), &desc_count, describe_user);
        }
        if (desc_fields != nullptr && desc_count > 0) {
          for (std::uint32_t fi = 0; fi < desc_count; ++fi) {
            const scene::SceneFieldDesc& f = desc_fields[fi];
            if (f.type > 10) {
              continue;
            }
            const std::uint32_t lane = kLaneSizes[f.type];
            if (f.offset > static_cast<std::uint32_t>(live.size()) ||
                lane >
                    static_cast<std::uint32_t>(live.size()) - f.offset) {
              continue;
            }
            if (f.type == 9 || f.type == 10) {
              continue;  // id lanes: never merge, keep live
            }
            const std::uint32_t n =
                (f.type >= 4 && f.type <= 8) ? kFieldsPerVec[f.type] : 1;
            const std::uint32_t stride = (f.type >= 4 && f.type <= 8) ? 4 : lane;
            for (std::uint32_t k = 0; k < n; ++k) {
              ranges.emplace_back(f.offset + k * stride, stride);
            }
          }
        }
        if (ranges.empty()) {
          // No description: 4-byte lanes (float/int contract). Entity-id
          // lanes can't be told apart here — parsed-file rows keep
          // file-local ids while live holds runtime ids, so byte lanes
          // that differ in ALL instances would always read "overridden".
          // Blob components without a description therefore merge whole
          // (same as before); lane splitting applies to described
          // components and the fixed-size built-ins below.
          bool is_builtin = (ncomp.name == "LocalTransform" ||
                             ncomp.name == "Camera" ||
                             ncomp.name == "MeshRenderer" ||
                             ncomp.name == "DirectionalLight");
          if (!is_builtin) {
            continue;  // keep live (conservative: never corrupt id lanes)
          }
          for (std::uint32_t o = 0;
               o + 4 <= static_cast<std::uint32_t>(live.size()); o += 4) {
            ranges.emplace_back(o, 4);
          }
        }
        std::vector<std::uint8_t> merged = live;
        for (const auto& rg : ranges) {
          bool same = true;
          for (std::uint32_t k = 0; k < rg.second; ++k) {
            if (live[rg.first + k] != bcomp->bytes[rg.first + k]) {
              same = false;
              break;
            }
          }
          if (same) {
            for (std::uint32_t k = 0; k < rg.second; ++k) {
              merged[rg.first + k] = ncomp.bytes[rg.first + k];
            }
          }
        }
        if (merged != live) {
          WriteCompBytes(world, members[m], ncomp.name, merged);
        }
      }
    }
    // Rebase to the new asset image (structure of the new file; member
    // matching on the NEXT update uses the new base).
    base->rows = new_rows;
    ++updated;
  }
  *out_updated = updated;
  return kPrefabOk;
}

auto revert_prefab(ecs_world_t* world, std::uint64_t root,
                   PrefabRegistry& registry) noexcept -> int32_t {
  if (world == nullptr || root == 0) {
    return kPrefabNullArg;
  }
  const auto root_ent = static_cast<ecs_entity_t>(root);
  if (!ecs_is_alive(world, root_ent)) {
    return kPrefabNotInstance;
  }
  const PrefabBaseImage* base = registry.base_for(root);
  if (base == nullptr) {
    return kPrefabNotInstance;
  }
  std::vector<ecs_entity_t> members = CollectMembers(world, root, registry);
  std::vector<std::size_t> match = MatchMembers(members, base->rows, world);
  for (std::size_t m = 0; m < members.size(); ++m) {
    if (match[m] >= base->rows.size()) {
      continue;
    }
    const PrefabBaseRow& Brow = base->rows[match[m]];
    for (const PrefabBaseComp& bcomp : Brow.comps) {
      WriteCompBytes(world, members[m], bcomp.name, bcomp.bytes);
    }
    // Restore the snapshotted name.
    ecs_set_name(world, members[m], Brow.name.c_str());
  }
  return kPrefabOk;
}

auto save_prefab(ecs_world_t* world, std::uint64_t root, const char* path,
                 scene::SceneDescribeFn describe, void* describe_user,
                 LinkResolveFn resolve, void* resolve_user) noexcept
    -> int32_t {
  if (world == nullptr || root == 0 || path == nullptr || path[0] == '\0') {
    return kPrefabNullArg;
  }
  const auto root_ent = static_cast<ecs_entity_t>(root);
  if (!ecs_is_alive(world, root_ent)) {
    return kPrefabBadFormat;
  }
  if (ecs_lookup(world, "LocalTransform") == 0) {
    return kPrefabNoLocal;
  }
  std::vector<scene::SceneRow> rows = scene::collect_subtree_rows(world, root);
  if (rows.empty()) {
    return kPrefabBadFormat;
  }
  // File-local ids in output order.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> id_map;
  id_map.reserve(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    id_map.emplace_back(rows[i].entity, static_cast<std::uint32_t>(i + 1U));
  }
  auto file_id = [&id_map](std::uint64_t e) -> std::uint32_t {
    // NOTE: entity lanes inside user blobs need the file-local id of the
    // REFERENCED live entity. CaptureRow stores raw runtime ids; remap
    // them here through the same map.
    for (const auto& kv : id_map) {
      if (kv.first == e) {
        return kv.second;
      }
    }
    return 0;
  };
  (void)file_id;
  std::vector<PrefabBaseRow> out_rows;
  std::vector<std::string> out_mounts;
  out_rows.reserve(rows.size());
  out_mounts.reserve(rows.size());
  for (const scene::SceneRow& srow : rows) {
    // Nested instance root (below the saved root): emit a PrefabRef mount
    // with overrides diffed against its base image. The bridge passes a
    // registry-backed resolver; a null resolver inlines everything.
    std::string mount_asset;
    bool is_mount = false;
    if (srow.entity != root && resolve != nullptr) {
      is_mount = resolve(srow.entity, mount_asset, resolve_user);
    }
    PrefabBaseRow row;
    for (const auto& kv : id_map) {
      if (kv.first == srow.entity) {
        row.file_id = kv.second;
        break;
      }
    }
    row.name = srow.name;
    row.parent_file_id = 0;
    for (const auto& kv : id_map) {
      if (kv.first == srow.parent) {
        row.parent_file_id = kv.second;
        break;
      }
    }
    if (is_mount) {
      // Mount point: own pose/name only (nested content stays in its
      // asset). Overrides of the nested root itself fold into the mount
      // row's components.
      const auto e = static_cast<ecs_entity_t>(srow.entity);
      float pose[10]{};
      if (scene::read_local_pose(world, e, pose)) {
        PrefabBaseComp bc;
        bc.name = "LocalTransform";
        bc.bytes.assign(reinterpret_cast<const std::uint8_t*>(pose),
                        reinterpret_cast<const std::uint8_t*>(pose) + 40);
        row.comps.push_back(std::move(bc));
      }
      out_mounts.push_back(mount_asset);
      out_rows.push_back(std::move(row));
      // Skip the nested subtree rows (owned by the nested asset).
      continue;
    }
    const auto e = static_cast<ecs_entity_t>(srow.entity);
    PrefabBaseRow captured = CaptureRow(world, e, row.file_id, row.parent_file_id);
    captured.name = row.name;
    out_mounts.emplace_back();
    out_rows.push_back(std::move(captured));
  }
  // Drop rows under mount points (nested content owned by nested assets).
  // A row whose ancestor chain passes through a mount row is nested-owned.
  std::vector<bool> drop(out_rows.size(), false);
  for (std::size_t i = 0; i < out_rows.size(); ++i) {
    if (out_mounts[i].empty()) {
      continue;
    }
    const std::uint32_t mount_id = out_rows[i].file_id;
    for (std::size_t j = 0; j < out_rows.size(); ++j) {
      if (i == j) {
        continue;
      }
      std::uint32_t cursor = out_rows[j].parent_file_id;
      for (std::size_t steps = 0; steps <= out_rows.size(); ++steps) {
        if (cursor == mount_id) {
          drop[j] = true;
          break;
        }
        if (cursor == 0) {
          break;
        }
        std::uint32_t next = 0;
        for (const PrefabBaseRow& r : out_rows) {
          if (r.file_id == cursor) {
            next = r.parent_file_id;
            break;
          }
        }
        if (next == 0) {
          break;
        }
        cursor = next;
      }
    }
  }
  std::vector<PrefabBaseRow> kept_rows;
  std::vector<std::string> kept_mounts;
  for (std::size_t i = 0; i < out_rows.size(); ++i) {
    if (!drop[i]) {
      kept_rows.push_back(std::move(out_rows[i]));
      kept_mounts.push_back(std::move(out_mounts[i]));
    }
  }
  SerializeCtx sctx;
  sctx.world = world;
  sctx.describe = describe;
  sctx.user = describe_user;
  sctx.id_map = &id_map;
  // Re-serialize with entity-lane remap: user-blob entity lanes hold
  // runtime ids in the captured image; AppendCompPayload's named path
  // resolves through id_map via sctx. Blob-path lanes keep stored ids
  // (parsed-file round-trips); live captures remap below.
  std::string out = "{\"format\":\"engine-prefab\",\"version\":1,\"entities\":[";
  for (std::size_t i = 0; i < kept_rows.size(); ++i) {
    const PrefabBaseRow& row = kept_rows[i];
    if (i != 0U) {
      out.push_back(',');
    }
    out += "{\"id\":";
    AppendU32(out, row.file_id);
    out += ",\"name\":";
    AppendJsonString(out, row.name);
    out += ",\"parent\":";
    AppendU32(out, row.parent_file_id);
    if (!kept_mounts[i].empty()) {
      out += ",\"PrefabRef\":{\"asset\":";
      AppendJsonString(out, kept_mounts[i]);
      out += "}";
    }
    out += ",\"components\":{";
    bool first = true;
    for (const PrefabBaseComp& comp : row.comps) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      std::uint32_t schema = SchemaHash(comp.name.c_str(),
                                        static_cast<std::uint32_t>(comp.bytes.size()), 4);
      AppendCompPayload(out, comp.name, comp.bytes.data(), comp.bytes.size(),
                        schema, sctx);
    }
    out += "}}";
  }
  out += "]}";
  if (!scene::file_util::AtomicWrite(path, out.data(), out.size())) {
    return kPrefabIoError;
  }
  return kPrefabOk;
}

auto apply_prefab(ecs_world_t* world, std::uint64_t root,
                  scene::SceneDescribeFn describe, void* describe_user,
                  PrefabRegistry& registry) noexcept -> int32_t {
  if (world == nullptr || root == 0) {
    return kPrefabNullArg;
  }
  const auto root_ent = static_cast<ecs_entity_t>(root);
  if (!ecs_is_alive(world, root_ent)) {
    return kPrefabNotInstance;
  }
  const std::string asset = registry.asset_for_instance(root);
  if (asset.empty()) {
    return kPrefabNotInstance;
  }
  // Snapshot live member images keyed by CURRENT base rows (match by the
  // same member matcher the merge uses): new file rows = live images for
  // matched members, base rows for members gone live-deleted... simplest
  // sound rule: file rows = live capture of every member in base-row
  // order (matched), appended with nothing (user-added live members stay
  // scene-local, never leak into the asset).
  const PrefabBaseImage* base = registry.base_for(root);
  if (base == nullptr) {
    return kPrefabNotInstance;
  }
  std::vector<ecs_entity_t> members = CollectMembers(world, root, registry);
  std::vector<std::size_t> match = MatchMembers(members, base->rows, world);
  // live entity -> base row index (invert the match).
  std::vector<std::int64_t> row_to_member(base->rows.size(), -1);
  for (std::size_t m = 0; m < members.size(); ++m) {
    if (match[m] < base->rows.size()) {
      row_to_member[match[m]] = static_cast<std::int64_t>(m);
    }
  }
  std::vector<PrefabBaseRow> file_rows;
  file_rows.reserve(base->rows.size());
  for (std::size_t r = 0; r < base->rows.size(); ++r) {
    if (row_to_member[r] < 0) {
      // Member deleted live: drop the row from the asset too (apply
      // propagates deletions, matching Unity).
      continue;
    }
    const ecs_entity_t e = members[static_cast<std::size_t>(row_to_member[r])];
    PrefabBaseRow row = CaptureRow(world, e, base->rows[r].file_id,
                                   base->rows[r].parent_file_id);
    const char* n = ecs_get_name(world, e);
    row.name = (n != nullptr) ? n : "";
    file_rows.push_back(std::move(row));
  }
  // Mounts: none at this level change shape (nested links preserved from
  // the current asset file when ids still exist).
  std::vector<PrefabBaseRow> cur_rows;
  std::vector<std::string> cur_mounts;
  if (parse_prefab_file(asset.c_str(), describe, describe_user, cur_rows,
                        cur_mounts) == kPrefabOk) {
    // ok to be stale — mounts keyed by file id below
  }
  std::vector<std::string> file_mounts(file_rows.size());
  for (std::size_t i = 0; i < file_rows.size(); ++i) {
    for (std::size_t c = 0; c < cur_rows.size() && c < cur_mounts.size(); ++c) {
      if (cur_rows[c].file_id == file_rows[i].file_id && !cur_mounts[c].empty()) {
        file_mounts[i] = cur_mounts[c];
        break;
      }
    }
  }
  std::string out;
  if (serialize_prefab_rows(file_rows, file_mounts, out) != kPrefabOk) {
    return kPrefabIoError;
  }
  if (!scene::file_util::AtomicWrite(asset.c_str(), out.data(), out.size())) {
    return kPrefabIoError;
  }
  // Rebase: the asset now equals live; overrides cleared.
  PrefabBaseImage* mut = registry.base_for_mut(root);
  if (mut != nullptr) {
    // Re-parse the written file so the base image matches the file bytes
    // exactly (schema/hash canonical form).
    std::vector<PrefabBaseRow> rebased;
    std::vector<std::string> rebmounts;
    if (parse_prefab_file(asset.c_str(), describe, describe_user, rebased,
                          rebmounts) == kPrefabOk) {
      mut->rows = std::move(rebased);
    } else {
      mut->rows = file_rows;
    }
  }
  return kPrefabOk;
}

}  // namespace engine::prefab
