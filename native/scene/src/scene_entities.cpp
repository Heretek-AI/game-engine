// scene_entities.cpp — P6-09: shared traversal implementation.
//
// Moved verbatim from scene_json_write.cpp: CollectRows/OrderRows/Lookup
// plus the writer's component-bucket walk. The writer TU keeps calling
// these (same output, byte-for-byte); the prefab module reuses them for
// subtree save + base-image capture.
#include "engine/scene/scene_entities.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace engine::scene {

namespace {

// Snapshot-style row: id + parent + name.
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

auto ToSceneRow(const Row& row) -> SceneRow {
  SceneRow out;
  out.entity = row.entity;
  out.parent = row.parent;
  out.name = row.name;
  return out;
}

}  // namespace

auto lookup_component(ecs_world_t* world, const char* name) noexcept
    -> std::uint64_t {
  if (world == nullptr || name == nullptr) {
    return 0;
  }
  return ecs_lookup_path_w_sep(world, 0, name, "", "", false);
}

auto collect_scene_rows(ecs_world_t* world) noexcept
    -> std::vector<SceneRow> {
  std::vector<SceneRow> out;
  if (world == nullptr) {
    return out;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return out;
  }
  for (const Row& row : OrderRows(CollectRows(world, local_id))) {
    out.push_back(ToSceneRow(row));
  }
  return out;
}

auto collect_subtree_rows(ecs_world_t* world, std::uint64_t root) noexcept
    -> std::vector<SceneRow> {
  std::vector<SceneRow> out;
  if (world == nullptr || root == 0) {
    return out;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return out;
  }
  const auto root_ent = static_cast<ecs_entity_t>(root);
  if (!ecs_is_alive(world, root_ent) ||
      ecs_get_id(world, root_ent, local_id) == nullptr) {
    return out;
  }
  // Keep only rows under root: walk the parent chain of every row.
  std::vector<Row> kept;
  for (Row& row : CollectRows(world, local_id)) {
    ecs_entity_t cursor = row.entity;
    // Bound the climb (same hostile-parent-chain caution as the reader's
    // cycle walk): a world never holds more rows than collected.
    for (std::size_t steps = 0; steps <= CollectRows(world, local_id).size();
         ++steps) {
      if (cursor == root_ent) {
        kept.push_back(std::move(row));
        break;
      }
      const ecs_entity_t parent =
          (cursor == row.entity)
              ? row.parent
              : ecs_get_target(world, cursor, EcsChildOf, 0);
      if (parent == 0) {
        break;
      }
      cursor = parent;
    }
  }
  for (const Row& row : OrderRows(std::move(kept))) {
    SceneRow srow = ToSceneRow(row);
    // Re-root: the subtree root serializes as parent 0.
    if (srow.entity == root) {
      srow.parent = 0;
    }
    out.push_back(std::move(srow));
  }
  return out;
}

auto enum_data_components(ecs_world_t* world, std::uint64_t entity) noexcept
    -> std::vector<SceneComp> {
  std::vector<SceneComp> out;
  if (world == nullptr || entity == 0) {
    return out;
  }
  const auto e = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, e)) {
    return out;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  const ecs_entity_t world_tf_id = ecs_lookup(world, "WorldTransform");
  const ecs_entity_t cam_id = lookup_component(world, "Engine.Runtime.Camera");
  const ecs_entity_t mesh_id =
      lookup_component(world, "Engine.Runtime.MeshRenderer");
  const ecs_entity_t light_id =
      lookup_component(world, "Engine.Runtime.DirectionalLight");
  const ecs_type_t* type = ecs_get_type(world, e);
  if (type == nullptr) {
    return out;
  }
  for (int32_t i = 0; i < type->count; ++i) {
    const ecs_id_t raw = type->array[i];
    const auto comp = static_cast<ecs_entity_t>(raw);
    if (!ecs_is_valid(world, comp) || ECS_IS_PAIR(raw) || comp == local_id ||
        comp == world_tf_id || comp == cam_id || comp == mesh_id ||
        comp == light_id) {
      continue;
    }
    const ecs_type_info_t* ti = ecs_get_type_info(world, raw);
    if (ti == nullptr || ti->size <= 0) {
      continue;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(
        ecs_get_id(world, e, comp));
    if (bytes == nullptr) {
      continue;  // Inherited via IsA: no owned bytes on this row.
    }
    const char* cname = ecs_get_name(world, comp);
    std::string comp_name = (cname != nullptr) ? cname : std::string("unknown");
    if (comp_name == "LocalTransform" || comp_name == "WorldTransform") {
      continue;
    }
    SceneComp sc;
    sc.id = comp;
    sc.name = std::move(comp_name);
    sc.size = static_cast<std::uint32_t>(ti->size);
    sc.align = static_cast<std::uint32_t>(ti->alignment);
    sc.bytes.assign(bytes, bytes + sc.size);
    out.push_back(std::move(sc));
  }
  return out;
}

auto read_local_pose(ecs_world_t* world, std::uint64_t entity,
                     float* out_xyz_q_s) noexcept -> bool {
  if (world == nullptr || entity == 0 || out_xyz_q_s == nullptr) {
    return false;
  }
  const auto e = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, e)) {
    return false;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return false;
  }
  const auto* lt = static_cast<const engine::ecs::LocalTransform*>(
      ecs_get_id(world, e, local_id));
  if (lt == nullptr) {
    return false;
  }
  out_xyz_q_s[0] = lt->px;
  out_xyz_q_s[1] = lt->py;
  out_xyz_q_s[2] = lt->pz;
  out_xyz_q_s[3] = lt->qx;
  out_xyz_q_s[4] = lt->qy;
  out_xyz_q_s[5] = lt->qz;
  out_xyz_q_s[6] = lt->qw;
  out_xyz_q_s[7] = lt->sx;
  out_xyz_q_s[8] = lt->sy;
  out_xyz_q_s[9] = lt->sz;
  return true;
}

auto write_local_pose(ecs_world_t* world, std::uint64_t entity,
                      const float* xyz_q_s) noexcept -> bool {
  if (world == nullptr || entity == 0 || xyz_q_s == nullptr) {
    return false;
  }
  const auto e = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, e)) {
    return false;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return false;
  }
  // ecs_get_mut_id does not add: emplace first (same rule as the scene
  // reader's SetPose path).
  ecs_add_id(world, e, local_id);
  auto* lt = static_cast<engine::ecs::LocalTransform*>(
      ecs_get_mut_id(world, e, local_id));
  if (lt == nullptr) {
    return false;
  }
  lt->px = xyz_q_s[0];
  lt->py = xyz_q_s[1];
  lt->pz = xyz_q_s[2];
  lt->qx = xyz_q_s[3];
  lt->qy = xyz_q_s[4];
  lt->qz = xyz_q_s[5];
  lt->qw = xyz_q_s[6];
  lt->sx = xyz_q_s[7];
  lt->sy = xyz_q_s[8];
  lt->sz = xyz_q_s[9];
  return true;
}

}  // namespace engine::scene
