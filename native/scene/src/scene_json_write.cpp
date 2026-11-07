// scene_json_write.cpp — P2-06: JSON v1 writer + entity walk.
//
// save_scene walks every LocalTransform-carrying entity (the scene
// membership marker, same rule as P3-03 editor snapshots), orders them
// parents-before-children (creation order within each sibling set, so the
// output is deterministic), and emits the exact §P2-06 format. Numbers use
// std::to_chars shortest round-trip (Sonar rule: no snprintf in new code).
#include "engine/scene/scene_io.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/scene/scene_entities.hpp"
#include "engine/scene/scene_file_util.hpp"
#include "flecs.h"

namespace engine::scene {

namespace {

// --- FNV-1a 32 ------------------------------------------------------------
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

auto AppendU32(std::string& out, std::uint32_t value) -> void {
  std::array<char, 16> buf{};
  auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  out.append(buf.data(), res.ptr);
}

// --- mesh source registry -------------------------------------------------
struct MeshSource {
  std::uint32_t id{0};
  std::string ref;
};

auto Registry() noexcept -> std::vector<MeshSource>& {
  static std::vector<MeshSource> reg;
  return reg;
}

// --- JSON string escaping -------------------------------------------------
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

auto AppendFloat(std::string& out, float value) -> void {
  // NaN/inf would serialize as `nan`/`inf` tokens — not valid JSON, and the
  // reader accepts them back into transform/camera math. Clamp to a defined
  // substitute (0) so output is always spec-parseable. Writer is void (no
  // error channel); a silent clamp keeps save->load->save byte-identical
  // where the alternative (a save error) would leave no file at all.
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

auto AppendFloatArray(std::string& out, std::span<const float> values)
    -> void {
  out.push_back('[');
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out.push_back(',');
    }
    AppendFloat(out, values[i]);
  }
  out.push_back(']');
}

// --- P3-04 named-field writer ------------------------------------------------
// Lane size table indexed by SceneFieldDesc::type (0..10). Must match the
// ABI ENGINE_FIELD_* table (generic_systems.cpp kFieldLaneSizes).
constexpr std::uint32_t kLaneSizes[11] = {4, 4, 4, 1, 8, 12, 16, 16, 16, 8, 8};

constexpr std::uint32_t kFieldsPerVec[11] = {1, 1, 1, 1, 2, 3, 4, 4, 4, 1, 1};

// Write one described user component as named JSON fields (writer rule,
// plan §8.2). Returns false when the lane data is unusable (caller falls
// back to the $bytes blob). entity lanes need the file-local id map:
// write 0 when the runtime id maps to nothing (null/invalid).
auto AppendNamedComponent(
    std::string& out, const SceneFieldDesc* fields, std::uint32_t count,
    const std::uint8_t* bytes, std::uint32_t comp_size,
    const std::function<std::uint32_t(std::uint64_t)>& file_id) -> bool {
  for (std::uint32_t i = 0; i < count; ++i) {
    const SceneFieldDesc& f = fields[i];
    // Validate against live memory before emitting anything: callers
    // check Atomic-per-component (skip whole component on any failure),
    // but the writer buffer is already open — validate here and bail to
    // the $bytes fallback so output never half-writes a component.
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
      case 0: {  // f32
        float v = 0;
        std::memcpy(&v, lane_ptr, 4);
        AppendFloat(out, v);
        break;
      }
      case 1: {  // i32
        std::int32_t v = 0;
        std::memcpy(&v, lane_ptr, 4);
        std::array<char, 16> num{};
        auto res = std::to_chars(num.data(), num.data() + num.size(), v);
        out.append(num.data(), res.ptr);
        break;
      }
      case 2: {  // u32
        std::uint32_t v = 0;
        std::memcpy(&v, lane_ptr, 4);
        AppendU32(out, v);
        break;
      }
      case 3: {  // bool8
        out += (*lane_ptr != 0) ? "true" : "false";
        break;
      }
      case 4:   // vec2
      case 5:   // vec3
      case 6:   // vec4
      case 7:   // quat
      case 8: {  // color4
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
      case 9: {  // entity: file-local id (0 = null/invalid)
        std::uint64_t runtime = 0;
        std::memcpy(&runtime, lane_ptr, 8);
        AppendU32(out, file_id(runtime));
        break;
      }
      case 10: {  // asset-ref: raw u64 cookie (documented limit)
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

// --- entity walk ----------------------------------------------------------
// P6-09: rows come from the shared scene_entities TU (collect_scene_rows:
// same walk, same order). The local Row/CollectRows/OrderRows/Lookup copies
// were removed; MeshRefFor/AppendHex stay (writer-local serialization).

auto MeshRefFor(std::uint32_t mesh_id) -> std::string {
  // Issue #84: meshes serialize as "asset path + index, never runtime
  // mesh_id". An id with no registry entry has no asset path to write, so
  // emit an empty mesh string (the loader resolves unknown/empty refs to
  // mesh_id 0, i.e. "no mesh") instead of the old "mesh_id:<n>" fallback
  // which leaked runtime ids into the file.
  for (const MeshSource& src : Registry()) {
    if (src.id == mesh_id) {
      return src.ref;
    }
  }
  return {};
}

auto AppendHex(std::string& out, const std::uint8_t* bytes,
               std::size_t count) -> void {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kHex[(bytes[i] >> 4U) & 0xFU]);
    out.push_back(kHex[bytes[i] & 0xFU]);
  }
}

// Component id lookup: shared helper (scene_entities). The local copy was
// removed in P6-09; call sites use lookup_component.

}  // namespace

auto schema_hash(const char* name, std::uint32_t size,
                 std::uint32_t align) noexcept -> std::uint32_t {
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

auto register_mesh_source(std::uint32_t mesh_id, const char* ref) noexcept
    -> void {
  if (ref == nullptr) {
    return;
  }
  for (MeshSource& src : Registry()) {
    if (src.id == mesh_id) {
      src.ref = ref;
      return;
    }
  }
  MeshSource src;
  src.id = mesh_id;
  src.ref = ref;
  Registry().push_back(std::move(src));
}

auto clear_mesh_sources() noexcept -> void { Registry().clear(); }

// Reverse lookup (registry lives in this TU; the reader calls through).
auto mesh_id_for_ref(const char* ref) noexcept -> std::uint32_t {
  if (ref == nullptr) {
    return 0;
  }
  const std::string_view want(ref);
  for (const MeshSource& src : Registry()) {
    if (src.ref == want) {
      return src.id;
    }
  }
  return 0;
}

auto save_scene(ecs_world_t* world, const char* path) noexcept -> int32_t {
  return save_scene_with_descriptions(world, path, nullptr, nullptr);
}

auto save_scene_with_descriptions(ecs_world_t* world, const char* path,
                                  SceneDescribeFn describe,
                                  void* user) noexcept -> int32_t {
  if (world == nullptr || path == nullptr || path[0] == '\0') {
    return kSceneNullArg;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return kSceneNoLocal;
  }
  const ecs_entity_t world_tf_id = ecs_lookup(world, "WorldTransform");
  const ecs_entity_t cam_id =
      lookup_component(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id =
      lookup_component(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      lookup_component(world, "Engine.Runtime.DirectionalLight");

  const std::vector<SceneRow> ordered = collect_scene_rows(world);
  // File-local ids: entity -> 1..N in output order.
  std::vector<std::pair<ecs_entity_t, std::uint32_t>> id_map;
  id_map.reserve(ordered.size());
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    id_map.emplace_back(ordered[i].entity,
                        static_cast<std::uint32_t>(i + 1U));
  }
  auto file_id = [&id_map](ecs_entity_t e) -> std::uint32_t {
    for (const auto& kv : id_map) {
      if (kv.first == e) {
        return kv.second;
      }
    }
    return 0;
  };

  std::string out = "{\"format\":\"engine-scene\",\"version\":1,\"entities\":[";
  bool first_entity = true;
  for (const SceneRow& row : ordered) {
    if (!first_entity) {
      out.push_back(',');
    }
    first_entity = false;
    out += "{\"id\":";
    AppendU32(out, file_id(row.entity));
    out += ",\"name\":";
    AppendJsonString(out, row.name);
    out += ",\"parent\":";
    AppendU32(out, file_id(row.parent));
    out += ",\"components\":{";

    bool first_comp = true;
    auto comma = [&out, &first_comp]() {
      if (!first_comp) {
        out.push_back(',');
      }
      first_comp = false;
    };

    // LocalTransform (named fields).
    float pose[10]{};
    const bool has_local =
        read_local_pose(world, row.entity, pose);
    if (has_local) {
      comma();
      out += "\"LocalTransform\":{\"position\":";
      std::array<float, 3> pos{pose[0], pose[1], pose[2]};
      AppendFloatArray(out, std::span<const float>(pos));
      out += ",\"rotation\":";
      std::array<float, 4> rot{pose[3], pose[4], pose[5], pose[6]};
      AppendFloatArray(out, std::span<const float>(rot));
      out += ",\"scale\":";
      std::array<float, 3> scl{pose[7], pose[8], pose[9]};
      AppendFloatArray(out, std::span<const float>(scl));
      out.push_back('}');
    }
    const auto row_ent = static_cast<ecs_entity_t>(row.entity);
    // Camera.
    if (cam_id != 0) {
      const auto* cam = static_cast<const engine::ecs::Camera*>(
          ecs_get_id(world, row_ent, cam_id));
      if (cam != nullptr) {
        comma();
        out += "\"Camera\":{\"fov_y_rad\":";
        AppendFloat(out, cam->fov_y_rad);
        out += ",\"near_z\":";
        AppendFloat(out, cam->near_z);
        out += ",\"far_z\":";
        AppendFloat(out, cam->far_z);
        out += ",\"is_main\":";
        AppendU32(out, cam->is_main);
        out.push_back('}');
      }
    }
    // MeshRenderer (mesh by asset ref, never runtime id).
    if (mesh_id != 0) {
      const auto* mr = static_cast<const engine::ecs::MeshRenderer*>(
          ecs_get_id(world, row_ent, mesh_id));
      if (mr != nullptr) {
        comma();
        out += "\"MeshRenderer\":{\"mesh\":";
        AppendJsonString(out, MeshRefFor(mr->mesh_id));
        out += ",\"base_color\":";
        AppendFloatArray(out, std::span<const float>(mr->base_color, 4));
        out.push_back('}');
      }
    }
    // DirectionalLight.
    if (light_id != 0) {
      const auto* li = static_cast<const engine::ecs::DirectionalLight*>(
          ecs_get_id(world, row_ent, light_id));
      if (li != nullptr) {
        comma();
        out += "\"DirectionalLight\":{\"direction\":";
        AppendFloatArray(out, std::span<const float>(li->direction, 3));
        out += ",\"intensity\":";
        AppendFloat(out, li->shadow);
        out.push_back('}');
      }
    }
    // User components: every other component id with type info, as a hex
    // blob + size + schema (P3-03 snapshot bucket model). Tags/pairs (no
    // type info) and the ChildOf pair are hierarchy, not data — skipped.
    // WorldTransform is derived — skipped.
    ecs_type_t const* type = ecs_get_type(world, row_ent);
    if (type != nullptr) {
      for (int32_t i = 0; i < type->count; ++i) {
        const ecs_id_t raw = type->array[i];
        const auto comp = static_cast<ecs_entity_t>(raw);
        if (!ecs_is_valid(world, comp) || ECS_IS_PAIR(raw) ||
            comp == local_id || comp == world_tf_id || comp == cam_id ||
            comp == mesh_id || comp == light_id) {
          continue;
        }
        const ecs_type_info_t* ti = ecs_get_type_info(world, raw);
        if (ti == nullptr || ti->size <= 0) {
          continue;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(
            ecs_get_id(world, row_ent, comp));
        if (bytes == nullptr) {
          continue;
        }
        const char* cname = ecs_get_name(world, comp);
        std::string comp_name =
            (cname != nullptr) ? cname : std::string("unknown");
        // Skip the engine's internal named components that carry their own
        // writer above but were looked up under a different id (defensive:
        // ids are cached per world, names are the contract).
        if (comp_name == "LocalTransform" || comp_name == "WorldTransform") {
          continue;
        }
        const auto size = static_cast<std::uint32_t>(ti->size);
        const auto align = static_cast<std::uint32_t>(ti->alignment);
        // P3-04: named fields when a live description exists (count > 0);
        // else the legacy $bytes blob, byte-for-byte unchanged.
        bool use_named = false;
        const SceneFieldDesc* desc_fields = nullptr;
        std::uint32_t desc_count = 0;
        if (describe != nullptr) {
          desc_fields = describe(comp_name.c_str(), &desc_count, user);
          use_named = (desc_fields != nullptr && desc_count > 0);
        }
        comma();
        AppendJsonString(out, comp_name);
        if (use_named) {
          // Buffer the named payload first: atomic per component (never
          // half-write); fall back to $bytes on any lane failure.
          std::string named;
          if (AppendNamedComponent(named, desc_fields, desc_count, bytes,
                                   size, file_id)) {
            out += ":{" + named + "}";
            continue;
          }
        }
        out += ":{\"$bytes\":\"";
        AppendHex(out, bytes, size);
        out += "\",\"$size\":";
        AppendU32(out, size);
        out += ",\"$schema\":\"";
        std::array<char, 9> hex{};
        const std::uint32_t hash = schema_hash(comp_name.c_str(), size, align);
        auto res = std::to_chars(hex.data(), hex.data() + 8,
                                 hash, 16);
        // Zero-pad to exactly 8 lowercase hex digits.
        std::size_t digits =
            static_cast<std::size_t>(res.ptr - hex.data());
        for (std::size_t p = digits; p < 8U; ++p) {
          out.push_back('0');
        }
        out.append(hex.data(), res.ptr);
        out += "\"}";
      }
    }
    out += "}}";
  }
  out += "]}";

  // Atomic write via the shared helper: bytes land in "<path>.tmp" and the
  // tmp is renamed over the target (MoveFileExW REPLACE_EXISTING on
  // Windows, where std::rename fails on existing targets). Short writes and
  // fclose flush failures fail the call and remove the tmp — a failed save
  // never truncates the user's previous scene.
  if (!file_util::AtomicWrite(path, out.data(), out.size())) {
    return kSceneIoError;
  }
  return kSceneOk;
}

}  // namespace engine::scene
