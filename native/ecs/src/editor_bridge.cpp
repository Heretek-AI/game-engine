// editor_bridge.cpp — M5-02 implementation; see editor_bridge.hpp.
//
// Hierarchy walks use ecs_children{_next} / ecs_each_id over
// ecs_pair(EcsChildOf, parent); roots are matched against "no EcsChildOf
// target" via ecs_get_target(..., EcsChildOf, 0) == 0. Prefab instances are
// entities carrying any (EcsIsA, *) pair (ecs_get_target(..., EcsIsA, 0)).
// Names copy via ecs_get_name (borrowed pointer, copied into std::string).
//
// P3-03 component-complete snapshots: for every row, walk
// ecs_get_type(world, e) and split each id into one of three buckets:
//   - LocalTransform (local_id_) -> the pose fields (existing behavior);
//   - component ids with ecs_get_type_info(world, id) != nullptr and
//     size > 0 -> byte blob copied from ecs_get_id (ChildOf pairs skipped:
//     hierarchy is covered by row.parent);
//   - everything else (tags, pairs incl. ChildOf/IsA) -> recorded by id,
//     re-added with ecs_add_id on restore.
// Restore: make-alive deleted rows, replay Local + blobs via ecs_set_id,
// re-add recorded tags, REMOVE components the entity gained after the
// snapshot (same three-bucket walk, ids absent from the row are removed),
// replay hierarchy + names, delete extras (whole-world snapshots only).
// All noexcept; ALLOCATION may throw on snapshot row growth — caught at the
// ABI boundary into error codes (the bridge layer below catches, this TU
// stays noexcept-clean).
#include "engine/ecs/editor_bridge.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace engine::editor {

namespace {

auto TransformOf(ecs_world_t* world, uint64_t local_id, uint64_t entity) noexcept -> ecs::LocalTransform* {
  return static_cast<ecs::LocalTransform*>(ecs_get_mut_id(world, static_cast<ecs_entity_t>(entity),
                                                     static_cast<ecs_entity_t>(local_id)));
}

// Snapshot-row replay: member-wise stores through the component pointer.
// (No stack temp + whole-struct assignment: the alignas(64) row must be
// stored in place — a stack copy trips alignment-sensitive codegen here.)
auto CopyPose(ecs::LocalTransform* dst, const SnapshotRow& row) noexcept -> void {
  dst->px = row.px;
  dst->py = row.py;
  dst->pz = row.pz;
  dst->qx = row.qx;
  dst->qy = row.qy;
  dst->qz = row.qz;
  dst->qw = row.qw;
  dst->sx = row.sx;
  dst->sy = row.sy;
  dst->sz = row.sz;
}

// Hierarchy + name replay for one snapshot row (all rows alive already).
auto ReplayHierarchy(ecs_world_t* world, const SnapshotRow& row) noexcept -> void {
  const auto e = static_cast<ecs_entity_t>(row.entity);
  if (row.parent != 0) {
    ecs_add_pair(world, e, EcsChildOf, static_cast<ecs_entity_t>(row.parent));
  } else {
    const ecs_entity_t cur = ecs_get_target(world, e, EcsChildOf, 0);
    if (cur != 0) {
      ecs_remove_pair(world, e, EcsChildOf, cur);
    }
  }
  ecs_set_name(world, e, row.name.empty() ? nullptr : row.name.c_str());
}

// Delete PIE-spawned extras: live Local ids absent from the snapshot
// (forward declaration; defined after snapshot_restore).
auto SweepExtras(ecs_world_t* world, uint64_t local_id, const std::vector<SnapshotRow>& rows) noexcept -> void {
  std::vector<ecs_entity_t> live;
  std::vector<ecs_entity_t> known;
  known.reserve(rows.size());
  for (const auto& row : rows) {
    known.push_back(static_cast<ecs_entity_t>(row.entity));
  }
  ecs_query_desc_t qd{};
  qd.terms[0].id = static_cast<ecs_entity_t>(local_id);
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* q = ecs_query_init(world, &qd);
  if (q != nullptr) {
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
      for (int i = 0; i < it.count; ++i) {
        live.push_back(it.entities[i]);
      }
    }
    ecs_query_fini(q);
  }
  std::ranges::sort(live);
  std::ranges::sort(known);
  for (ecs_entity_t id : live) {
    if (!std::ranges::binary_search(known, id)) {
      ecs_delete(world, id);
    }
  }
}

// Capture every non-Local component of one entity into a snapshot row:
// blobs for data components, id records for tags/pairs. The ChildOf pair is
// skipped (row.parent covers hierarchy); IsA pairs ARE recorded so prefab
// links survive the round-trip.
auto CaptureExtra(ecs_world_t* world, uint64_t local_id, ecs_entity_t e, SnapshotRow& row) noexcept -> void {
  const ecs_type_t* type = ecs_get_type(world, e);
  if (type == nullptr) {
    return;
  }
  const ecs_id_t local = static_cast<ecs_id_t>(local_id);
  for (int32_t i = 0; i < type->count; ++i) {
    const ecs_id_t id = type->array[i];
    if (id == local) {
      continue;  // LocalTransform lives in the pose fields.
    }
    if (ECS_IS_PAIR(id)) {
      // ChildOf is hierarchy (row.parent); every other pair (IsA, ...) is
      // replayed with ecs_add_id on restore.
      const ecs_entity_t rel = ECS_PAIR_FIRST(id);
      if (rel == EcsChildOf) {
        continue;
      }
      row.tags.push_back(id);
      continue;
    }
    const ecs_type_info_t* ti = ecs_get_type_info(world, id);
    if (ti == nullptr || ti->size <= 0) {
      row.tags.push_back(id);  // Tag: no data, record the id.
      continue;
    }
    const auto* src = static_cast<const uint8_t*>(ecs_get_id(world, e, id));
    if (src == nullptr) {
      continue;  // Inherited via IsA: owned copy arrives through the IsA pair.
    }
    SnapshotRow::Blob blob;
    blob.id = id;
    const auto size = static_cast<size_t>(ti->size);
    blob.bytes.assign(src, src + size);
    row.blobs.push_back(std::move(blob));
  }
}

// Replay one row's blobs + tags onto an already-alive entity (Local re-added
// by the caller first).
auto ReplayComponents(ecs_world_t* world, const SnapshotRow& row) noexcept -> void {
  const auto e = static_cast<ecs_entity_t>(row.entity);
  for (uint64_t tag : row.tags) {
    ecs_add_id(world, e, static_cast<ecs_id_t>(tag));
  }
  for (const auto& blob : row.blobs) {
    if (!blob.bytes.empty()) {
      ecs_set_id(world, e, static_cast<ecs_id_t>(blob.id), blob.bytes.size(), blob.bytes.data());
    }
  }
}

// Remove components the entity gained after the snapshot: every current id
// not present in the row (Local excluded — the caller keeps it; ChildOf is
// hierarchy, replayed separately).
auto RemoveGained(ecs_world_t* world, uint64_t local_id, const SnapshotRow& row) noexcept -> void {
  const auto e = static_cast<ecs_entity_t>(row.entity);
  const ecs_id_t local = static_cast<ecs_id_t>(local_id);
  const ecs_type_t* type = ecs_get_type(world, e);
  if (type == nullptr) {
    return;
  }
  for (int32_t i = 0; i < type->count; ++i) {
    const ecs_id_t id = type->array[i];
    if (id == local) {
      continue;
    }
    if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == EcsChildOf) {
      continue;
    }
    bool known = false;
    if (!ECS_IS_PAIR(id)) {
      const ecs_type_info_t* ti = ecs_get_type_info(world, id);
      const bool is_blob = ti != nullptr && ti->size > 0;
      if (is_blob) {
        for (const auto& blob : row.blobs) {
          if (blob.id == id) {
            known = true;
            break;
          }
        }
      } else {
        for (uint64_t tag : row.tags) {
          if (tag == id) {
            known = true;
            break;
          }
        }
      }
    } else {
      for (uint64_t tag : row.tags) {
        if (tag == id) {
          known = true;
          break;
        }
      }
    }
    if (!known) {
      ecs_remove_id(world, e, id);
    }
  }
}

// Whole-row pose copy from the packed ABI buffer into the component row.
// The pose payload is 10 contiguous floats starting at px (3 pos + 4 quat
// + 3 scale = 40 bytes); the row is the 64-byte LocalTransform column.
// Bounds are by construction (ABI contract, asserted 64B both sides).
auto ApplyPose(float* dst, const float* src) noexcept -> void {
  constexpr size_t kPoseFloats = 10;
  static_assert(kPoseFloats * sizeof(float) <= sizeof(ecs::LocalTransform),
                "pose payload fits the LocalTransform row");
  std::memcpy(dst, src, kPoseFloats * sizeof(float));  // NOSONAR(cpp:S3519,cpp:S6422) 10-float ABI payload, bounds asserted above
}

auto CopyOut(const float* row, float* out) noexcept -> void {
  // Row points at px (first of 10 packed floats): plain memcpy, no
  // member-pointer reinterpretation.
  constexpr size_t kPoseFloats = 10;
  static_assert(kPoseFloats * sizeof(float) <= sizeof(ecs::LocalTransform),
                "pose payload fits the LocalTransform row");
  std::memcpy(out, row, kPoseFloats * sizeof(float));  // NOSONAR(cpp:S3519,cpp:S6422) 10-float ABI payload, bounds asserted above
}

}  // namespace

auto EditorSession::bind(ecs_world_t* world) noexcept -> bool {
  if (world == nullptr) {
    return false;
  }
  world_ = world;
  // Resolve LocalTransform by well-known name (registered by EcsWorld).
  const ecs_entity_t comp = ecs_lookup(world_, "LocalTransform");
  if (comp == 0 || !ecs_is_alive(world_, comp)) {
    world_ = nullptr;
    return false;
  }
  local_id_ = comp;
  return true;
}

auto EditorSession::alive(uint64_t entity) const noexcept -> bool {
  if (entity == 0 || world_ == nullptr) {
    return false;
  }
  return ecs_is_alive(world_, static_cast<ecs_entity_t>(entity));
}

auto EditorSession::local_ptr(uint64_t entity) const noexcept -> const float* {
  if (!alive(entity) || local_id_ == 0) {
    return nullptr;
  }
  // Read-only accessor: ecs_get_id never mutates (missing Local is a soft
  // null, e.g. freshly spawned identity rows in tests).
  const auto* l = static_cast<const ecs::LocalTransform*>(ecs_get_id(world_, static_cast<ecs_entity_t>(entity),
                                                                     static_cast<ecs_entity_t>(local_id_)));
  return l == nullptr ? nullptr : &(l->px);
}

auto EditorSession::local_ptr_mut(uint64_t entity) noexcept -> float* {
  if (!alive(entity) || local_id_ == 0) {
    return nullptr;
  }
  // ecs_get_mut_id requires the component: guard with has first.
  if (!ecs_has_id(world_, static_cast<ecs_entity_t>(entity), static_cast<ecs_entity_t>(local_id_))) {
    return nullptr;
  }
  auto* l = static_cast<ecs::LocalTransform*>(ecs_get_mut_id(world_, static_cast<ecs_entity_t>(entity),
                                                        static_cast<ecs_entity_t>(local_id_)));
  return l == nullptr ? nullptr : &(l->px);
}

auto EditorSession::spawn(const char* name, uint64_t* out_entity) noexcept -> int32_t {
  if (world_ == nullptr || local_id_ == 0) {
    return -1;
  }
  // Fresh entity first (ecs_new never resolves to an existing row), then
  // assign the literal name verbatim (ecs_set_name does no path splitting:
  // "Cube.001" stays one name, and duplicates are allowed — the name index
  // keeps the latest entity for that key). ecs_entity_init with desc.name
  // would instead LOOK UP the name in scope ("." sep) and return the
  // existing entity, silently resetting its transform on duplicate spawn.
  const ecs_entity_t e = ecs_new(world_);
  if (e == 0) {
    return -2;
  }
  ecs_add_id(world_, e, static_cast<ecs_entity_t>(local_id_));
  if (name != nullptr && name[0] != '\0') {
    ecs_set_name(world_, e, name);
  }
  if (auto* l = TransformOf(world_, local_id_, e); l != nullptr) {
    l->px = l->py = l->pz = 0.0F;
    l->qx = l->qy = l->qz = 0.0F;
    l->qw = 1.0F;
    l->sx = l->sy = l->sz = 1.0F;
  }
  if (out_entity != nullptr) {
    *out_entity = e;
  }
  return 0;
}

auto EditorSession::set_name(uint64_t entity, const char* name) noexcept -> int32_t {
  if (!alive(entity)) {
    return -1;
  }
  // Empty/NULL clears the name (Flecs treats NULL as anonymous).
  const char* use = (name != nullptr && name[0] != '\0') ? name : nullptr;
  ecs_set_name(world_, static_cast<ecs_entity_t>(entity), use);
  return 0;
}

auto EditorSession::set_parent(uint64_t entity, uint64_t parent) noexcept -> int32_t {
  if (!alive(entity)) {
    return -1;
  }
  if (parent != 0 && !alive(parent)) {
    return -2;
  }
  if (entity == parent) {
    return -3;
  }
  const auto e = static_cast<ecs_entity_t>(entity);
  if (parent == 0) {
    const ecs_entity_t cur = ecs_get_target(world_, e, EcsChildOf, 0);
    if (cur != 0) {
      ecs_remove_pair(world_, e, EcsChildOf, cur);
    }
    return 0;
  }
  // Cycle guard (release Flecs skips the EcsAcyclic debug check): walk the
  // proposed parent's ChildOf chain; reject when it reaches entity (-4).
  // Without this, a Root<-Child cycle makes HierarchyModel.BuildNode
  // recurse until StackOverflow.
  for (ecs_entity_t p = static_cast<ecs_entity_t>(parent); p != 0;
       p = ecs_get_target(world_, p, EcsChildOf, 0)) {
    if (p == e) {
      return -4;
    }
  }
  ecs_add_pair(world_, e, EcsChildOf, static_cast<ecs_entity_t>(parent));
  return 0;
}

auto EditorSession::add_local(uint64_t entity) noexcept -> int32_t {
  if (!alive(entity) || local_id_ == 0) {
    return -1;
  }
  ecs_add_id(world_, static_cast<ecs_entity_t>(entity), static_cast<ecs_entity_t>(local_id_));
  return 0;
}

auto EditorSession::set_local(uint64_t entity, const float* xyz_q_s) noexcept -> int32_t {
  if (xyz_q_s == nullptr) {
    return -1;
  }
  auto* l = local_ptr_mut(entity);
  if (l == nullptr) {
    return -2;
  }
  // l points at px (first of 10 packed floats: p/q/s contiguous).
  ApplyPose(l, xyz_q_s);
  ecs_modified_id(world_, static_cast<ecs_entity_t>(entity), static_cast<ecs_entity_t>(local_id_));
  return 0;
}

auto EditorSession::get_local(uint64_t entity, float* out_xyz_q_s) const noexcept -> int32_t {
  if (out_xyz_q_s == nullptr) {
    return -1;
  }
  auto* raw = local_ptr(entity);
  if (raw == nullptr) {
    return -2;
  }
  CopyOut(raw, out_xyz_q_s);
  return 0;
}

auto EditorSession::count_children(uint64_t parent, uint64_t* out_count) const noexcept -> int32_t {
  if (out_count == nullptr || world_ == nullptr) {
    return -1;
  }
  *out_count = 0;
  if (parent == 0) {
    return count_roots(out_count);
  }
  if (!alive(parent)) {
    return -2;
  }
  ecs_iter_t it = ecs_children(world_, static_cast<ecs_entity_t>(parent));
  while (ecs_children_next(&it)) {
    for (int i = 0; i < it.count; ++i) {
      (*out_count)++;
    }
  }
  return 0;
}

auto EditorSession::get_child(uint64_t parent, uint64_t index, uint64_t* out_child) const noexcept -> int32_t {
  if (out_child == nullptr || world_ == nullptr) {
    return -1;
  }
  if (parent == 0) {
    return get_root(index, out_child);
  }
  if (!alive(parent)) {
    return -2;
  }
  uint64_t cursor = 0;
  ecs_iter_t it = ecs_children(world_, static_cast<ecs_entity_t>(parent));
  while (ecs_children_next(&it)) {
    for (int i = 0; i < it.count; ++i) {
      if (cursor == index) {
        *out_child = it.entities[i];
        return 0;
      }
      cursor++;
    }
  }
  return -3;  // OOB
}

auto EditorSession::count_roots(uint64_t* out_count) const noexcept -> int32_t {
  if (out_count == nullptr || world_ == nullptr) {
    return -1;
  }
  *out_count = 0;
  // Roots = entities WITH LocalTransform but with NO EcsChildOf target.
  ecs_query_desc_t qd{};
  qd.terms[0].id = static_cast<ecs_entity_t>(local_id_);
  qd.terms[1].id = ecs_pair(EcsChildOf, EcsWildcard);
  qd.terms[1].oper = EcsNot;
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* q = ecs_query_init(world_, &qd);
  if (q == nullptr) {
    return -2;
  }
  ecs_iter_t it = ecs_query_iter(world_, q);
  while (ecs_query_next(&it)) {
    *out_count += static_cast<uint64_t>(it.count);
  }
  ecs_query_fini(q);
  return 0;
}

auto EditorSession::get_root(uint64_t index, uint64_t* out_child) const noexcept -> int32_t {
  if (out_child == nullptr || world_ == nullptr) {
    return -1;
  }
  ecs_query_desc_t qd{};
  qd.terms[0].id = static_cast<ecs_entity_t>(local_id_);
  qd.terms[1].id = ecs_pair(EcsChildOf, EcsWildcard);
  qd.terms[1].oper = EcsNot;
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* q = ecs_query_init(world_, &qd);
  if (q == nullptr) {
    return -2;
  }
  uint64_t cursor = 0;
  int32_t rc = -3;
  ecs_iter_t it = ecs_query_iter(world_, q);
  while (ecs_query_next(&it)) {
    for (int i = 0; i < it.count; ++i) {
      if (cursor == index) {
        *out_child = it.entities[i];
        rc = 0;
        break;
      }
      cursor++;
    }
    if (rc == 0) {
      break;
    }
  }
  ecs_query_fini(q);
  return rc;
}

auto EditorSession::is_prefab_instance(uint64_t entity) const noexcept -> int32_t {
  if (!alive(entity)) {
    return -1;
  }
  return ecs_get_target(world_, static_cast<ecs_entity_t>(entity), EcsIsA, 0) != 0 ? 1 : 0;
}

auto EditorSession::begin_edit(uint64_t entity) noexcept -> float* {
  if (editing_) {
    return nullptr;
  }
  auto* raw = local_ptr_mut(entity);
  if (raw == nullptr) {
    return nullptr;
  }
  editing_ = true;
  edit_entity_ = entity;
  return raw;
}

auto EditorSession::end_edit() noexcept -> int32_t {
  if (!editing_) {
    return -1;
  }
  editing_ = false;
  // Signal the in-place writes to Flecs (set_local path calls
  // ecs_modified_id; the raw-pointer path must too) so OnSet observers
  // fire and change detection (ecs_query_changed) sees the edit.
  if (edit_entity_ != 0 && alive(edit_entity_) && local_id_ != 0) {
    ecs_modified_id(world_, static_cast<ecs_entity_t>(edit_entity_),
                    static_cast<ecs_entity_t>(local_id_));
  }
  edit_entity_ = 0;
  return 0;
}

// NOTE: this TU builds -fno-exceptions (hot flags): no try/catch, no
// throwing new. Snapshot rows grow via vector::push_back guarded by the
// nothrow snapshot object; allocations use the throwing form only where
// OOM is unrecoverable at editor scale (same rule as EcsWorld paths).
auto EditorSession::snapshot_take() noexcept -> EditorSnapshot* {
  if (world_ == nullptr || local_id_ == 0) {
    return nullptr;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI handle: session owns it, discard destroys it (bridge layer, same as M1-04 worlds)
  auto* snap = new (std::nothrow) EditorSnapshot();
  if (snap == nullptr) {
    return nullptr;
  }
  ecs_query_desc_t qd{};
  qd.terms[0].id = static_cast<ecs_entity_t>(local_id_);
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* q = ecs_query_init(world_, &qd);
  if (q == nullptr) {
    delete snap;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired cleanup of the handle above
    return nullptr;
  }
  ecs_iter_t it = ecs_query_iter(world_, q);
  while (ecs_query_next(&it)) {
    auto* col = static_cast<ecs::LocalTransform*>(ecs_field_w_size(&it, sizeof(ecs::LocalTransform), 0));
    if (col == nullptr) {
      continue;
    }
    for (int i = 0; i < it.count; ++i) {
      SnapshotRow row;
      row.entity = it.entities[i];
      row.px = col[i].px;
      row.py = col[i].py;
      row.pz = col[i].pz;
      row.qx = col[i].qx;
      row.qy = col[i].qy;
      row.qz = col[i].qz;
      row.qw = col[i].qw;
      row.sx = col[i].sx;
      row.sy = col[i].sy;
      row.sz = col[i].sz;
      row.has_local = true;
      row.parent = ecs_get_target(world_, it.entities[i], EcsChildOf, 0);
      if (const char* n = ecs_get_name(world_, it.entities[i]); n != nullptr) {
        row.name = n;
      }
      CaptureExtra(world_, local_id_, it.entities[i], row);
      snap->rows.push_back(std::move(row));
    }
  }
  ecs_query_fini(q);
  return snap;
}

auto EditorSession::snapshot_restore(const EditorSnapshot* snapshot) noexcept -> int32_t {
  if (world_ == nullptr || local_id_ == 0) {
    return -1;
  }
  if (snapshot == nullptr) {
    return -2;
  }
  // Delete PIE-spawned extras FIRST: a recycled index (same index, newer
  // generation) must be dead before ecs_make_alive restores the old id,
  // else make_alive rejects it (index alive with different generation)
  // and the snapshot entity can never resurrect.
  SweepExtras(world_, local_id_, snapshot->rows);
  // Resurrect every row with its ORIGINAL id (generation included).
  for (const auto& row : snapshot->rows) {
    const auto e = static_cast<ecs_entity_t>(row.entity);
    if (!ecs_is_alive(world_, e)) {
      ecs_make_alive(world_, e);
    }
  }
  for (const auto& row : snapshot->rows) {
    const auto e = static_cast<ecs_entity_t>(row.entity);
    if (row.has_local) {
      ecs_add_id(world_, e, static_cast<ecs_entity_t>(local_id_));
      if (auto* l = TransformOf(world_, local_id_, row.entity); l != nullptr) {
        CopyPose(l, row);
      }
    }
    // Tags + blobs replayed onto the live row; gained components removed.
    ReplayComponents(world_, row);
    RemoveGained(world_, local_id_, row);
  }
  // Hierarchy + names replayed in a second pass (all rows alive now).
  for (const auto& row : snapshot->rows) {
    ReplayHierarchy(world_, row);
  }
  return 0;
}

auto EditorSession::snapshot_take_subtree(uint64_t root) noexcept -> EditorSnapshot* {
  if (world_ == nullptr || local_id_ == 0) {
    return nullptr;
  }
  const auto r = static_cast<ecs_entity_t>(root);
  if (root == 0 || !alive(root)) {
    return nullptr;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI handle: session owns it, discard destroys it (bridge layer, same as the whole-scene pair)
  auto* snap = new (std::nothrow) EditorSnapshot();
  if (snap == nullptr) {
    return nullptr;
  }
  // BFS over the ChildOf tree from root (cycle-safe: a visited set).
  std::vector<ecs_entity_t> stack{r};
  std::vector<ecs_entity_t> seen{r};
  while (!stack.empty()) {
    const ecs_entity_t e = stack.back();
    stack.pop_back();
    SnapshotRow row;
    row.entity = e;
    if (auto* l = TransformOf(world_, local_id_, e); l != nullptr) {
      row.px = l->px;
      row.py = l->py;
      row.pz = l->pz;
      row.qx = l->qx;
      row.qy = l->qy;
      row.qz = l->qz;
      row.qw = l->qw;
      row.sx = l->sx;
      row.sy = l->sy;
      row.sz = l->sz;
      row.has_local = true;
    }
    row.parent = ecs_get_target(world_, e, EcsChildOf, 0);
    if (const char* n = ecs_get_name(world_, e); n != nullptr) {
      row.name = n;
    }
    CaptureExtra(world_, local_id_, e, row);
    snap->rows.push_back(std::move(row));
    ecs_iter_t it = ecs_children(world_, e);
    while (ecs_children_next(&it)) {
      for (int i = 0; i < it.count; ++i) {
        const ecs_entity_t child = it.entities[i];
        bool known = false;
        for (ecs_entity_t s : seen) {
          if (s == child) {
            known = true;
            break;
          }
        }
        if (!known) {
          seen.push_back(child);
          stack.push_back(child);
        }
      }
    }
  }
  return snap;
}

auto EditorSession::snapshot_restore_subtree(const EditorSnapshot* snapshot, uint64_t root) noexcept -> int32_t {
  if (world_ == nullptr || local_id_ == 0) {
    return -1;
  }
  if (snapshot == nullptr) {
    return -2;
  }
  if (root != 0 && !alive(root)) {
    // Resurrect the root first so descendants have a live parent target.
    ecs_make_alive(world_, static_cast<ecs_entity_t>(root));
  }
  // Resurrect every captured row with its ORIGINAL id.
  for (const auto& row : snapshot->rows) {
    const auto e = static_cast<ecs_entity_t>(row.entity);
    if (!ecs_is_alive(world_, e)) {
      ecs_make_alive(world_, e);
    }
  }
  // Components + hierarchy for captured rows only (never touches the rest
  // of the world — whole-scene sweep would delete live siblings).
  for (const auto& row : snapshot->rows) {
    const auto e = static_cast<ecs_entity_t>(row.entity);
    if (row.has_local) {
      ecs_add_id(world_, e, static_cast<ecs_entity_t>(local_id_));
      if (auto* l = TransformOf(world_, local_id_, row.entity); l != nullptr) {
        CopyPose(l, row);
      }
    }
    ReplayComponents(world_, row);
    RemoveGained(world_, local_id_, row);
  }
  for (const auto& row : snapshot->rows) {
    ReplayHierarchy(world_, row);
  }
  return 0;
}

auto EditorSession::destroy_entity(uint64_t entity) noexcept -> int32_t {
  if (world_ == nullptr) {
    return -1;
  }
  if (!alive(entity)) {
    return -2;
  }
  ecs_delete(world_, static_cast<ecs_entity_t>(entity));
  return 0;
}

auto EditorSession::get_name(uint64_t entity, char* out, uint64_t capacity) const noexcept -> int32_t {
  if (out == nullptr || capacity == 0) {
    return -1;
  }
  out[0] = '\0';
  if (!alive(entity)) {
    return -2;
  }
  const char* n = ecs_get_name(world_, static_cast<ecs_entity_t>(entity));
  if (n == nullptr) {
    return 0;  // Unnamed: empty string.
  }
  uint64_t i = 0;
  while (n[i] != '\0' && i + 1 < capacity) {
    out[i] = n[i];
    ++i;
  }
  out[i] = '\0';
  return 0;
}

auto EditorSession::duplicate(uint64_t entity, uint64_t* out_entity) noexcept -> int32_t {
  if (world_ == nullptr || local_id_ == 0) {
    return -1;
  }
  if (!alive(entity)) {
    return -2;
  }
  const auto src = static_cast<ecs_entity_t>(entity);
  const ecs_entity_t copy = ecs_new(world_);
  if (copy == 0) {
    return -3;
  }
  // Fresh LocalTransform row, then copy every owned component (no pairs:
  // hierarchy is re-attached below, other pairs replayed by id).
  ecs_add_id(world_, copy, static_cast<ecs_entity_t>(local_id_));
  if (auto* dst = TransformOf(world_, local_id_, copy); dst != nullptr) {
    if (const auto* l = TransformOf(world_, local_id_, entity); l != nullptr) {
      *dst = *l;
    }
  }
  const ecs_type_t* type = ecs_get_type(world_, src);
  if (type != nullptr) {
    for (int32_t i = 0; i < type->count; ++i) {
      const ecs_id_t id = type->array[i];
      if (id == static_cast<ecs_id_t>(local_id_)) {
        continue;
      }
      if (ECS_IS_PAIR(id)) {
        if (ECS_PAIR_FIRST(id) == EcsChildOf) {
          continue;
        }
        ecs_add_id(world_, copy, id);
        continue;
      }
      const ecs_type_info_t* ti = ecs_get_type_info(world_, id);
      if (ti == nullptr || ti->size <= 0) {
        ecs_add_id(world_, copy, id);
        continue;
      }
      if (const void* s = ecs_get_id(world_, src, id); s != nullptr) {
        ecs_set_id(world_, copy, id, static_cast<size_t>(ti->size), s);
      }
    }
  }
  // Sibling placement: same parent, copy name with " Copy" suffix.
  const ecs_entity_t parent = ecs_get_target(world_, src, EcsChildOf, 0);
  if (parent != 0) {
    ecs_add_pair(world_, copy, EcsChildOf, parent);
  }
  if (const char* n = ecs_get_name(world_, src); n != nullptr) {
    std::string copy_name = n;
    copy_name += " Copy";
    ecs_set_name(world_, copy, copy_name.c_str());
  }
  if (out_entity != nullptr) {
    *out_entity = copy;
  }
  return 0;
}

}  // namespace engine::editor
