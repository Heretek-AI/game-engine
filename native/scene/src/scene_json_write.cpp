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
  std::array<char, 32> buf{};
  auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (res.ec == std::errc()) {
    out.append(buf.data(), res.ptr);
  } else {
    out += "0";
  }
}

auto AppendFloatArray(std::string& out, const float* values,
                      std::size_t count) -> void {
  out.push_back('[');
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0U) {
      out.push_back(',');
    }
    AppendFloat(out, values[i]);
  }
  out.push_back(']');
}

// --- entity walk ----------------------------------------------------------
// Snapshot-style row: id + parent + name (editor_bridge walks the same way:
// ecs_get_target(EcsChildOf, 0) for parents, ecs_get_name for names).
struct Row {
  ecs_entity_t entity{0};
  ecs_entity_t parent{0};
  std::string name;
};

auto CollectRows(ecs_world_t* world, ecs_entity_t local_id)
    -> std::vector<Row> {
  std::vector<Row> rows;
  ecs_query_desc_t qd{};
  qd.terms[0].id = local_id;
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* query = ecs_query_init(world, &qd);
  if (query == nullptr) {
    return rows;
  }
  ecs_iter_t it = ecs_query_iter(world, query);
  while (ecs_query_next(&it)) {
    for (int i = 0; i < it.count; ++i) {
      const ecs_entity_t e = it.entities[i];
      Row row;
      row.entity = e;
      row.parent = ecs_get_target(world, e, EcsChildOf, 0);
      const char* name = ecs_get_name(world, e);
      row.name = (name != nullptr) ? name : "";
      rows.push_back(std::move(row));
    }
  }
  ecs_query_fini(query);
  return rows;
}

// Deterministic order: roots first in creation order, then BFS by parent
// (creation order within each sibling set). Creation order == entity id
// order on a fresh world; on a reused world, relative order still matches
// between the two saves because both walks see the same live set.
auto OrderRows(std::vector<Row> rows) -> std::vector<Row> {
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return static_cast<std::uint64_t>(a.entity) <
           static_cast<std::uint64_t>(b.entity);
  });
  std::vector<Row> ordered;
  ordered.reserve(rows.size());
  std::vector<ecs_entity_t> frontier{0};
  // Iterative deepening over depths: depth 0 = roots, depth d = children
  // of depth d-1. Each level keeps global creation order.
  std::vector<Row> rest = rows;
  while (!rest.empty()) {
    std::vector<Row> next;
    next.reserve(rest.size());
    bool progressed = false;
    for (Row& row : rest) {
      bool ready = false;
      for (ecs_entity_t done : frontier) {
        if (row.parent == done) {
          ready = true;
          break;
        }
      }
      if (ready) {
        ordered.push_back(std::move(row));
        progressed = true;
      } else {
        next.push_back(std::move(row));
      }
    }
    if (!progressed) {
      // Orphaned parent ids (parent without LocalTransform, or dead):
      // emit the rest in creation order as roots would.
      for (Row& row : next) {
        ordered.push_back(std::move(row));
      }
      break;
    }
    frontier.clear();
    for (const Row& row : ordered) {
      frontier.push_back(row.entity);
    }
    rest = std::move(next);
    if (rest.size() == rows.size()) {
      break;
    }
  }
  return ordered;
}

auto MeshRefFor(std::uint32_t mesh_id) -> std::string {
  for (const MeshSource& src : Registry()) {
    if (src.id == mesh_id) {
      return src.ref;
    }
  }
  std::string fallback = "mesh_id:";
  AppendU32(fallback, mesh_id);
  return fallback;
}

auto AppendHex(std::string& out, const std::uint8_t* bytes,
               std::size_t count) -> void {
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kHex[(bytes[i] >> 4U) & 0xFU]);
    out.push_back(kHex[bytes[i] & 0xFU]);
  }
}

// Component id lookup that tolerates missing components (returns 0).
auto Lookup(ecs_world_t* world, const char* name) noexcept -> ecs_entity_t {
  return ecs_lookup_path_w_sep(world, 0, name, "", "", false);
}

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
  if (world == nullptr || path == nullptr || path[0] == '\0') {
    return kSceneNullArg;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return kSceneNoLocal;
  }
  const ecs_entity_t world_tf_id = ecs_lookup(world, "WorldTransform");
  const ecs_entity_t cam_id = Lookup(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id = Lookup(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      Lookup(world, "Engine.Runtime.DirectionalLight");

  const std::vector<Row> ordered = OrderRows(CollectRows(world, local_id));
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
  for (const Row& row : ordered) {
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
    const auto* lt = static_cast<const engine::ecs::LocalTransform*>(
        ecs_get_id(world, row.entity, local_id));
    if (lt != nullptr) {
      comma();
      out += "\"LocalTransform\":{\"position\":";
      const float pos[3] = {lt->px, lt->py, lt->pz};
      AppendFloatArray(out, pos, 3);
      out += ",\"rotation\":";
      const float rot[4] = {lt->qx, lt->qy, lt->qz, lt->qw};
      AppendFloatArray(out, rot, 4);
      out += ",\"scale\":";
      const float scl[3] = {lt->sx, lt->sy, lt->sz};
      AppendFloatArray(out, scl, 3);
      out.push_back('}');
    }
    // Camera.
    if (cam_id != 0) {
      const auto* cam = static_cast<const engine::ecs::Camera*>(
          ecs_get_id(world, row.entity, cam_id));
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
          ecs_get_id(world, row.entity, mesh_id));
      if (mr != nullptr) {
        comma();
        out += "\"MeshRenderer\":{\"mesh\":";
        AppendJsonString(out, MeshRefFor(mr->mesh_id));
        out += ",\"base_color\":";
        AppendFloatArray(out, mr->base_color, 4);
        out.push_back('}');
      }
    }
    // DirectionalLight.
    if (light_id != 0) {
      const auto* li = static_cast<const engine::ecs::DirectionalLight*>(
          ecs_get_id(world, row.entity, light_id));
      if (li != nullptr) {
        comma();
        out += "\"DirectionalLight\":{\"direction\":";
        AppendFloatArray(out, li->direction, 3);
        out += ",\"intensity\":";
        AppendFloat(out, li->shadow);
        out.push_back('}');
      }
    }
    // User components: every other component id with type info, as a hex
    // blob + size + schema (P3-03 snapshot bucket model). Tags/pairs (no
    // type info) and the ChildOf pair are hierarchy, not data — skipped.
    // WorldTransform is derived — skipped.
    ecs_type_t const* type = ecs_get_type(world, row.entity);
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
            ecs_get_id(world, row.entity, comp));
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
        comma();
        AppendJsonString(out, comp_name);
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

  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return kSceneIoError;
  }
  const std::size_t written =
      std::fwrite(out.data(), 1, out.size(), file);
  std::fclose(file);
  if (written != out.size()) {
    return kSceneIoError;
  }
  return kSceneOk;
}

}  // namespace engine::scene
