// editor_bridge.hpp — M5-02: native editor session over one Flecs world.
//
// An EngineEditorSession binds one EngineWorld (ecs_world_t + LocalTransform
// component id) and serves the ABI 4 editor tail: hierarchy walks
// (EcsChildOf), prefab probes (EcsIsA), in-place LocalTransform edits, and
// PIE snapshots (row backups with Local, parent, name plus every other
// component per entity: data blobs for component ids with type info,
// id re-adds for tags/pairs).
//
// Lifetime: sessions/snapshots are heap objects owned by the C#
// Engine.Editor (destroy/discard idempotent, noexcept throughout — an
// exception crossing the C ABI is UB). Snapshot restore resurrects deleted
// entities with their original ids (ecs_make_alive), re-adds tags, replays
// component bytes with ecs_set_id, removes gained components, and deletes
// PIE-spawned extras, so entering/exiting Play Mode is a memcpy-scale replay
// (<10ms).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;

namespace engine::editor {

// One captured entity row: identity + Local data + hierarchy + name +
// every other component (P3-03: component-complete snapshots).
struct SnapshotRow {
  uint64_t entity{0};
  float px{0}, py{0}, pz{0};
  float qx{0}, qy{0}, qz{0}, qw{1};
  float sx{1}, sy{1}, sz{1};
  uint64_t parent{0};  // 0 = root
  std::string name;    // may be empty
  bool has_local{false};
  // Extra owned components (non-pair ids with type info, excluding Local):
  // raw bytes copied from ecs_get_id, restored with ecs_set_id.
  struct Blob {
    uint64_t id{0};
    std::vector<uint8_t> bytes;
  };
  std::vector<Blob> blobs;
  // Tags + pairs (no type info): re-added with ecs_add_id on restore.
  std::vector<uint64_t> tags;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)  // rows is private, same-toolchain builds
#endif                            // both sides; no cross-compiler ABI.
struct ENGINE_CORE_API EditorSnapshot {
  std::vector<SnapshotRow> rows;
};
#ifdef _MSC_VER
#pragma warning(pop)  // C4251 scoped to EditorSnapshot.
#endif

// Editor session bound to one world. The world + LocalTransform component
// id are captured at create (from EcsWorld internals via raw world + id
// lookup by name — no dependency on EcsWorld's C++ type across the ABI).
class ENGINE_CORE_API EditorSession {
 public:
  EditorSession() noexcept = default;
  EditorSession(const EditorSession&) = delete;
  auto operator=(const EditorSession&) -> EditorSession& = delete;
  ~EditorSession() noexcept = default;

  // Bind to a live world. Returns false on null world or missing
  // LocalTransform component.
  auto bind(ecs_world_t* world) noexcept -> bool;

  [[nodiscard]] auto bound() const noexcept -> bool { return world_ != nullptr; }

  auto spawn(const char* name, uint64_t* out_entity) noexcept -> int32_t;
  auto set_name(uint64_t entity, const char* name) noexcept -> int32_t;
  auto set_parent(uint64_t entity, uint64_t parent) noexcept -> int32_t;
  auto add_local(uint64_t entity) noexcept -> int32_t;
  auto set_local(uint64_t entity, const float* xyz_q_s) noexcept -> int32_t;
  auto get_local(uint64_t entity, float* out_xyz_q_s) const noexcept -> int32_t;
  auto count_children(uint64_t parent, uint64_t* out_count) const noexcept -> int32_t;
  auto get_child(uint64_t parent, uint64_t index, uint64_t* out_child) const noexcept -> int32_t;
  auto count_roots(uint64_t* out_count) const noexcept -> int32_t;
  auto get_root(uint64_t index, uint64_t* out_child) const noexcept -> int32_t;
  auto is_prefab_instance(uint64_t entity) const noexcept -> int32_t;
  auto begin_edit(uint64_t entity) noexcept -> float*;
  auto end_edit() noexcept -> int32_t;

  auto snapshot_take() noexcept -> EditorSnapshot*;
  auto snapshot_take_subtree(uint64_t root) noexcept -> EditorSnapshot*;
  auto snapshot_restore(const EditorSnapshot* snapshot) noexcept -> int32_t;
  auto snapshot_restore_subtree(const EditorSnapshot* snapshot, uint64_t root) noexcept -> int32_t;
  auto destroy_entity(uint64_t entity) noexcept -> int32_t;
  auto get_name(uint64_t entity, char* out, uint64_t capacity) const noexcept -> int32_t;
  auto duplicate(uint64_t entity, uint64_t* out_entity) noexcept -> int32_t;

 private:
  ecs_world_t* world_{nullptr};
  uint64_t local_id_{0};
  bool editing_{false};
  uint64_t edit_entity_{0};

  [[nodiscard]] auto alive(uint64_t entity) const noexcept -> bool;
  [[nodiscard]] auto local_ptr(uint64_t entity) const noexcept -> const float*;
  [[nodiscard]] auto local_ptr_mut(uint64_t entity) noexcept -> float*;
};

}  // namespace engine::editor
