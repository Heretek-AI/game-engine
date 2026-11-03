// scene_json_read.cpp — P2-06: JSON v1 reader.
//
// Minimal recursive-descent parser over the exact writer output (objects,
// arrays, strings with escapes, numbers, true/false/null skipped). No new
// dependency. Load is two-pass: create every entity + replay components,
// then replay parent links (parents may be defined after children).
#include "engine/scene/scene_io.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace engine::scene {

namespace {

// --- JSON value model -----------------------------------------------------
struct Value {
  enum class Kind : std::uint8_t { Null, Num, Str, Arr, Obj };
  Kind kind{Kind::Null};
  std::string str;  // Str: unescaped; Num: raw text
  std::vector<Value> items;  // Arr
  // Obj: parallel keys + values (insertion order; linear lookup is fine —
  // objects are small: components per entity, fields per component).
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
  const char* cur{nullptr};
  const char* end{nullptr};
  bool ok{true};

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
      case '"': {
        out.kind = Value::Kind::Str;
        parse_string(out.str);
        break;
      }
      default: parse_literal(out); break;
    }
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
          // \uXXXX — only the ASCII range is decoded; anything else
          // becomes '?'. Scene strings are paths/names (ASCII).
          if (end - cur < 5) {
            ok = false;
            return;
          }
          unsigned code = 0;
          for (int i = 1; i <= 4; ++i) {
            const char c = cur[i];
            code <<= 4U;
            if (c >= '0' && c <= '9') {
              code |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
              code |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
              code |= static_cast<unsigned>(c - 'A' + 10);
            } else {
              ok = false;
              return;
            }
          }
          out.push_back(code < 0x80U ? static_cast<char>(code) : '?');
          cur += 4;
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

  auto parse_literal(Value& out) -> void {
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
      out.kind = Value::Kind::Null;
    } else if (tok == "true" || tok == "false") {
      out.kind = Value::Kind::Num;
      out.str = (tok == "true") ? "1" : "0";
    } else {
      out.kind = Value::Kind::Num;
      out.str.assign(tok);
    }
  }

  auto parse_array(Value& out) -> void {
    out.kind = Value::Kind::Arr;
    ++cur;  // [
    skip_ws();
    if (cur < end && *cur == ']') {
      ++cur;
      return;
    }
    while (cur < end) {
      Value item;
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
        return;
      }
      ok = false;
      return;
    }
    ok = false;
  }

  auto parse_object(Value& out) -> void {
    out.kind = Value::Kind::Obj;
    ++cur;  // {
    skip_ws();
    if (cur < end && *cur == '}') {
      ++cur;
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
      Value val;
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
        return;
      }
      ok = false;
      return;
    }
    ok = false;
  }
};

auto ParseNum(std::string_view text, double& out) -> bool {
  // std::from_chars for double (shortest round-trip matches to_chars).
  const char* begin = text.data();
  const char* last = text.data() + text.size();
  auto res = std::from_chars(begin, last, out);
  return res.ec == std::errc() && res.ptr == last;
}

auto ParseU32(std::string_view text, std::uint32_t& out) -> bool {
  const char* begin = text.data();
  const char* last = text.data() + text.size();
  auto res = std::from_chars(begin, last, out);
  return res.ec == std::errc() && res.ptr == last;
}

auto NumOf(const Value* v, double& out) -> bool {
  return v != nullptr && v->kind == Value::Kind::Num &&
         ParseNum(v->str, out);
}

auto U32Of(const Value* v, std::uint32_t& out) -> bool {
  return v != nullptr && v->kind == Value::Kind::Num &&
         ParseU32(v->str, out);
}

auto GetFloatArray(const Value& comp, std::string_view field, float* out,
                   std::size_t count) -> bool {
  const Value* arr = comp.find(field);
  if (arr == nullptr || arr->kind != Value::Kind::Arr ||
      arr->items.size() != count) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    double d = 0;
    if (!NumOf(&arr->items[i], d)) {
      return false;
    }
    out[i] = static_cast<float>(d);
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

// Resolve a "path#index" mesh ref to a mesh_id. Known refs come from the
// registry; "mesh_id:<n>" parses directly (the save fallback for
// unregistered ids, so those round-trip without a registry entry).
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
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return kSceneIoError;
  }
  std::string text;
  std::array<char, 4096> chunk{};
  while (true) {
    const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
    text.append(chunk.data(), got);
    if (got < chunk.size()) {
      break;
    }
  }
  std::fclose(file);

  Parser parser{text.data(), text.data() + text.size(), true};
  Value root;
  parser.parse_value(root);
  if (!parser.ok || root.kind != Value::Kind::Obj) {
    return kSceneBadFormat;
  }
  const Value* format = root.find("format");
  const Value* version = root.find("version");
  const Value* entities = root.find("entities");
  if (format == nullptr || format->kind != Value::Kind::Str ||
      format->str != "engine-scene" || version == nullptr ||
      version->kind != Value::Kind::Num || version->str != "1" ||
      entities == nullptr || entities->kind != Value::Kind::Arr) {
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
  // Pass 1: create entities + components (parents replayed in pass 2).
  for (const Value& ent : entities->items) {
    if (ent.kind != Value::Kind::Obj) {
      return kSceneBadFormat;
    }
    const Value* fid = ent.find("id");
    const Value* fname = ent.find("name");
    const Value* fcomps = ent.find("components");
    std::uint32_t file_id = 0;
    if (!U32Of(fid, file_id) || file_id == 0 || fname == nullptr ||
        fname->kind != Value::Kind::Str || fcomps == nullptr ||
        fcomps->kind != Value::Kind::Obj) {
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
      const Value& cval = fcomps->values[c];
      if (cval.kind != Value::Kind::Obj) {
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
        float pos[3]{};
        float rot[4]{0, 0, 0, 1};
        float scl[3]{1, 1, 1};
        GetFloatArray(cval, "position", pos, 3);
        GetFloatArray(cval, "rotation", rot, 4);
        GetFloatArray(cval, "scale", scl, 3);
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
        double fov = 0;
        double near_z = 0;
        double far_z = 0;
        std::uint32_t is_main = 0;
        NumOf(cval.find("fov_y_rad"), fov);
        NumOf(cval.find("near_z"), near_z);
        NumOf(cval.find("far_z"), far_z);
        U32Of(cval.find("is_main"), is_main);
        cam->fov_y_rad = static_cast<float>(fov);
        cam->near_z = static_cast<float>(near_z);
        cam->far_z = static_cast<float>(far_z);
        cam->is_main = is_main;
      } else if (cname == "MeshRenderer" && mesh_id2 != 0) {
        ecs_add_id(world, e, mesh_id2);
        auto* mr = static_cast<engine::ecs::MeshRenderer*>(
            ecs_get_mut_id(world, e, mesh_id2));
        if (mr == nullptr) {
          continue;
        }
        const Value* mesh = cval.find("mesh");
        mr->mesh_id =
            (mesh != nullptr && mesh->kind == Value::Kind::Str)
                ? MeshIdFor(mesh->str)
                : 0;
        float color[4]{1, 1, 1, 1};
        GetFloatArray(cval, "base_color", color, 4);
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
        float dir[3]{};
        double shadow = 0;
        GetFloatArray(cval, "direction", dir, 3);
        // Writer emits the lane as "intensity" (spec format); the native
        // field is `shadow` (P4-03 renamed the lane, same offset).
        if (!NumOf(cval.find("intensity"), shadow)) {
          NumOf(cval.find("shadow"), shadow);
        }
        li->direction[0] = dir[0];
        li->direction[1] = dir[1];
        li->direction[2] = dir[2];
        li->shadow = static_cast<float>(shadow);
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
        const Value* bytes = cval.find("$bytes");
        const Value* size_v = cval.find("$size");
        const Value* schema_v = cval.find("$schema");
        std::uint32_t size = 0;
        if (bytes == nullptr || bytes->kind != Value::Kind::Str ||
            !U32Of(size_v, size) || size == 0 || size > 65536U ||
            schema_v == nullptr || schema_v->kind != Value::Kind::Str) {
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
        // the component already exists; unknown names register fresh.
        const ecs_entity_t existing = Lookup(world, cname.c_str());
        std::uint32_t align = 0;
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
          std::uint32_t file_schema = 0;
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
          if (!hex_ok ||
              file_schema != schema_hash(cname.c_str(), size, align)) {
            std::printf(
                "[WARN] scene: bad user component '%s' (schema mismatch) "
                "skipped\n",
                cname.c_str());
            ++skipped;
            continue;
          }
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
        // Register-on-demand for unknown names (size from file, align 4
        // unless the name already exists — then the live alignment won).
        // NOTE: align-4 default matches every test user component; a
        // file with a wider align replays under 4 and the schema check
        // on the NEXT load would warn (documented, never a crash).
        const ecs_entity_t comp =
            (existing != 0)
                ? existing
                : UserComponent(world, cname.c_str(), size,
                                (align != 0) ? align : 4);
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
  // Pass 2: parent links (file-local id -> live entity).
  std::size_t idx = 0;
  for (const Value& ent : entities->items) {
    const Value* fparent = ent.find("parent");
    std::uint32_t parent_file = 0;
    if (fparent != nullptr && fparent->kind == Value::Kind::Num) {
      ParseU32(fparent->str, parent_file);
    }
    if (parent_file != 0 && idx < id_map.size()) {
      ecs_entity_t parent_live = 0;
      for (const auto& kv : id_map) {
        if (kv.first == parent_file) {
          parent_live = kv.second;
          break;
        }
      }
      if (parent_live != 0) {
        ecs_add_pair(world, id_map[idx].second, EcsChildOf, parent_live);
      }
    }
    ++idx;
  }
  return skipped;
}

}  // namespace engine::scene
