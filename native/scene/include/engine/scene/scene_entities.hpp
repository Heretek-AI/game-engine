// scene_entities.hpp — P6-09: shared entity-traversal + component-access
// helpers for the P2-06 scene JSON pair and the prefab module.
//
// Extracted byte-for-byte from scene_json_write.cpp (CollectRows/OrderRows/
// Lookup) so the writer's output — and the save->load->save byte-identical
// round-trip gate — cannot drift between the two users. Rows carry the
// entity + parent + name (editor_bridge walks the same way:
// ecs_get_target(EcsChildOf, 0) for parents, ecs_get_name for names).
//
// The component walk mirrors the writer's bucket model:
//   - LocalTransform (local_id_) -> pose (caller reads via LocalOf);
//   - data component ids (type info, size > 0, not a pair) -> blob bytes;
//   - everything else (tags, pairs incl. ChildOf/IsA) -> skipped here
//     (hierarchy is covered by row.parent; prefab links ride the prefab
//     registry, never the file).
// WorldTransform is derived and never enumerated. noexcept throughout
// (bridge boundary rule).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;

namespace engine::scene {

// One walked entity: identity + hierarchy + name.
struct SceneRow {
  std::uint64_t entity{0};
  std::uint64_t parent{0};  // 0 = root
  std::string name;         // may be empty
};

// One data component on an entity: component id + name + raw bytes.
struct SceneComp {
  std::uint64_t id{0};
  std::string name;  // flecs component name ("unknown" when unnamed)
  std::vector<std::uint8_t> bytes;
  std::uint32_t size{0};
  std::uint32_t align{1};
};

// Walk every LocalTransform-carrying entity (the scene membership marker,
// same rule as P3-03 editor snapshots), ordered parents-before-children
// (creation order within each sibling set), so output is deterministic.
ENGINE_CORE_API auto collect_scene_rows(ecs_world_t* world) noexcept
    -> std::vector<SceneRow>;

// Walk the subtree rooted at `root` (root + all LocalTransform-carrying
// descendants), ordered parents-before-children. Empty when root is
// unknown/dead or carries no LocalTransform.
ENGINE_CORE_API auto collect_subtree_rows(ecs_world_t* world,
                                          std::uint64_t root) noexcept
    -> std::vector<SceneRow>;

// Component id lookup that tolerates missing components (returns 0).
ENGINE_CORE_API auto lookup_component(ecs_world_t* world,
                                      const char* name) noexcept
    -> std::uint64_t;

// Enumerate one entity's data components (writer bucket model above).
// Skips LocalTransform/WorldTransform/built-ins-with-writer (Camera,
// MeshRenderer, DirectionalLight — the caller serializes those by name),
// tags, and pairs. Empty when the entity is unknown/dead.
ENGINE_CORE_API auto enum_data_components(ecs_world_t* world,
                                          std::uint64_t entity) noexcept
    -> std::vector<SceneComp>;

// Read one entity's LocalTransform pose (10 floats:
// px,py,pz,qx,qy,qz,qw,sx,sy,sz). False when unknown/dead or no Local.
ENGINE_CORE_API auto read_local_pose(ecs_world_t* world,
                                     std::uint64_t entity,
                                     float* out_xyz_q_s) noexcept -> bool;

// Write one entity's LocalTransform pose (emplaces Local first, same rule
// as the scene reader's SetPose path). False on unknown/dead world/entity.
ENGINE_CORE_API auto write_local_pose(ecs_world_t* world,
                                      std::uint64_t entity,
                                      const float* xyz_q_s) noexcept -> bool;

}  // namespace engine::scene
