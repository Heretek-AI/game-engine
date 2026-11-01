// ecs_world.cpp — M1-03 implementation; see header.
#include "engine/ecs/ecs_world.hpp"

#include "engine/core/aligned_flecs_hooks.hpp"
#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flecs.h"

namespace engine::ecs {

EcsWorld::~EcsWorld() noexcept { destroy(); }

auto EcsWorld::construct() noexcept -> bool {
  if (world_ != nullptr) {
    return false;
  }
  // P0-05: LocalTransform/WorldTransform are alignas(64), and Flecs
  // allocates column storage with ecs_os_malloc/realloc (16-byte aligned
  // by default). The 64B-aligned heap hooks must be live before the first
  // ecs_init (ecs_os_set_api is one-shot), and before the task hooks below
  // are written into ecs_os_api (installing defaults would reset them).
  aligned_hooks_ = engine::core::EnsureAlignedFlecsHooks();
  if (sched_ != nullptr) {
    if (!sched_->running() || !InstallTaskHooks(sched_)) {
      return false;
    }
  }
  world_ = ecs_init();
  if (world_ == nullptr) {
    return false;
  }

  // Register built-ins with explicit sizes/alignments (no ECS_COMPONENT
  // macro: this TU compiles -fno-exceptions/-fno-rtti and must not rely
  // on static-registration tricks).
  ecs_entity_desc_t lent{};
  lent.name = "LocalTransform";
  const ecs_entity_t local_ent = ecs_entity_init(world_, &lent);
  ecs_component_desc_t ld{};
  ld.entity = local_ent;
  ld.type.size = static_cast<ecs_size_t>(sizeof(LocalTransform));
  ld.type.alignment = static_cast<ecs_size_t>(alignof(LocalTransform));
  local_id_ = ecs_component_init(world_, &ld);

  ecs_entity_desc_t went{};
  went.name = "WorldTransform";
  const ecs_entity_t world_ent = ecs_entity_init(world_, &went);
  ecs_component_desc_t wd{};
  wd.entity = world_ent;
  wd.type.size = static_cast<ecs_size_t>(sizeof(WorldTransform));
  wd.type.alignment = static_cast<ecs_size_t>(alignof(WorldTransform));
  world_id_ = ecs_component_init(world_, &wd);

  if (local_id_ == 0 || world_id_ == 0) {
    destroy();
    return false;
  }

  // Breadth-first cascade: each entity's LocalTransform, its parent's
  // WorldTransform (optional — roots have none), and its own
  // WorldTransform. Proven query shape: "Local, World(cascade ChildOf),
  // World" (parent field is shared/singleton per table).
  ecs_query_desc_t qd{};
  // Struct terms (not DSL): term 0 = own LocalTransform; term 1 = the
  // parent's WorldTransform, cascade-traversed over EcsChildOf (optional
  // so roots match with a null field); term 2 = own WorldTransform.
  // Matches the proven "Local, World(cascade ChildOf), World" shape.
  qd.terms[0].id = local_id_;
  qd.terms[1].id = world_id_;
  qd.terms[1].trav = EcsChildOf;
  qd.terms[1].src.id = EcsCascade | EcsUp;
  qd.terms[1].oper = EcsOptional;
  qd.terms[2].id = world_id_;
  qd.cache_kind = EcsQueryCacheAuto;
  cascade_query_ = ecs_query_init(world_, &qd);
  if (cascade_query_ == nullptr) {
    destroy();
    return false;
  }
  return true;
}

auto EcsWorld::destroy() noexcept -> void {
  if (world_ == nullptr) {
    return;
  }
  if (cascade_query_ != nullptr) {
    ecs_query_fini(cascade_query_);
    cascade_query_ = nullptr;
  }
  // Join task threads (if any) before the world dies.
  ecs_set_task_threads(world_, 1);
  ecs_set_threads(world_, 1);
  ecs_fini(world_);
  world_ = nullptr;
  local_id_ = 0;
  world_id_ = 0;
}

auto EcsWorld::set_task_threads(std::int32_t threads) noexcept -> bool {
  if (world_ == nullptr || threads < 1) {
    return false;
  }
  if (threads > 1) {
    if (sched_ == nullptr || !sched_->running()) {
      return false;
    }
    // Flecs task workers persist for the world's lifetime, each occupying
    // one pool worker. Clamp stages to worker_count()+1 (pool + caller) so
    // an oversubscribed stage count can never deadlock the worker sync.
    const std::int32_t cap =
        static_cast<std::int32_t>(sched_->worker_count()) + 1;
    ecs_set_task_threads(world_, threads < cap ? threads : cap);
  } else {
    ecs_set_task_threads(world_, 1);
    ecs_set_threads(world_, 1);
  }
  return true;
}

auto EcsWorld::propagate() noexcept -> bool {
  if (world_ == nullptr || cascade_query_ == nullptr) {
    return false;
  }
  ecs_iter_t it = ecs_query_iter(world_, cascade_query_);
  while (ecs_query_next(&it)) {
    auto* local = static_cast<LocalTransform*>(
        ecs_field_w_size(&it, sizeof(LocalTransform), 0));
    auto* parent = static_cast<WorldTransform*>(
        ecs_field_w_size(&it, sizeof(WorldTransform), 1));
    auto* own = static_cast<WorldTransform*>(
        ecs_field_w_size(&it, sizeof(WorldTransform), 2));
    if (local == nullptr || own == nullptr) {
      return false;
    }
    if (parent != nullptr) {
      for (int r = 0; r < it.count; ++r) {
        float composed_local[16];
        ComposeLocal(local[r], composed_local);
        // Row-vector order: World = Local * ParentWorld.
        MatMul4x4(composed_local, parent->m, own[r].m);
      }
    } else {
      // Roots (e.g. entities added before any parent existed, matched
      // with a null shared parent field): World = Local.
      for (int r = 0; r < it.count; ++r) {
        ComposeLocal(local[r], own[r].m);
      }
    }
  }
  return true;
}

auto EcsWorld::LocalColumnAligned(std::uint64_t entity) const noexcept
    -> bool {
  if (world_ == nullptr) {
    return false;
  }
  const void* col = ecs_get_id(world_, static_cast<ecs_entity_t>(entity),
                               static_cast<ecs_entity_t>(local_id_));
  return col != nullptr &&
         reinterpret_cast<std::uintptr_t>(col) %  // NOLINT: address check
                 alignof(LocalTransform) ==
             0;
}

auto EcsWorld::register_render() noexcept -> bool {
  if (world_ == nullptr) {
    return false;
  }
  const std::uint64_t cam = RegisterRenderComponent(
      world_, "Engine.Runtime.Camera", sizeof(Camera), alignof(Camera));
  const std::uint64_t mesh = RegisterRenderComponent(
      world_, "Engine.Runtime.MeshRenderer", sizeof(MeshRenderer),
      alignof(MeshRenderer));
  const std::uint64_t light = RegisterRenderComponent(
      world_, "Engine.Runtime.DirectionalLight", sizeof(DirectionalLight),
      alignof(DirectionalLight));
  // P6-06 2D components (same idempotent helper; C# mirrors in
  // Rendering.cs must match these sizes exactly).
  const std::uint64_t sprite2d = RegisterRenderComponent(
      world_, "Engine.Runtime.SpriteRenderer2D", sizeof(SpriteRenderer2D),
      alignof(SpriteRenderer2D));
  const std::uint64_t ortho = RegisterRenderComponent(
      world_, "Engine.Runtime.OrthoCamera", sizeof(OrthoCamera),
      alignof(OrthoCamera));
  // P6-08 tilemap components (C# mirrors in Tilemap.cs must match exactly).
  const std::uint64_t tilemap = RegisterRenderComponent(
      world_, "Engine.Runtime.TilemapRenderer2D", sizeof(TilemapRenderer2D),
      alignof(TilemapRenderer2D));
  const std::uint64_t tilecol = RegisterRenderComponent(
      world_, "Engine.Runtime.TilemapCollider2D", sizeof(TilemapCollider2D),
      alignof(TilemapCollider2D));
  return cam != 0 && mesh != 0 && light != 0 && sprite2d != 0 && ortho != 0 &&
         tilemap != 0 && tilecol != 0;
}

auto EcsWorld::bulk_create(std::size_t count) noexcept -> std::size_t {
  if (world_ == nullptr || count == 0) {
    return 0;
  }
  // ecs_bulk_new_w_id takes int32_t; refuse counts that would narrow.
  // (MSVC C4267 under /WX, Clang -Wconversion alike.)
  if (count > static_cast<std::size_t>(INT32_MAX)) {
    return 0;
  }
  const ecs_entity_t* ids = ecs_bulk_new_w_id(
      world_, static_cast<ecs_entity_t>(local_id_),
      static_cast<std::int32_t>(count));
  if (ids == nullptr) {
    return 0;
  }
  // P0-05 runtime check: an over-aligned column base is what makes the
  // member stores below safe (GCC emits aligned vmovaps for them). Fail
  // closed instead of faulting if the aligned hooks were not installable.
  if (!LocalColumnAligned(ids[0])) {
    std::fprintf(stderr,
                 "ecs: LocalTransform column not %zuB-aligned "
                 "(aligned Flecs hooks not installed)\n",
                 alignof(LocalTransform));
    return 0;
  }
  // Bulk-create with LocalTransform only, then add WorldTransform in
  // place (avoids a second id-list copy; each add is a table move that
  // preserves the Local data).
  for (std::size_t i = 0; i < count; ++i) {
    auto* l = static_cast<LocalTransform*>(
        ecs_get_mut_id(world_, ids[i], static_cast<ecs_entity_t>(local_id_)));
    if (l != nullptr) {
      l->px = 0.0F;
      l->py = 0.0F;
      l->pz = 0.0F;
      l->qx = 0.0F;
      l->qy = 0.0F;
      l->qz = 0.0F;
      l->qw = 1.0F;
      l->sx = 1.0F;
      l->sy = 1.0F;
      l->sz = 1.0F;
    }
    ecs_add_id(world_, ids[i], static_cast<ecs_entity_t>(world_id_));
    // A freshly added WorldTransform is uninitialized memory until the
    // next propagate(). Seed identity so managed callbacks never read
    // garbage between bulk_create and the first propagation.
    auto* w = static_cast<WorldTransform*>(
        ecs_get_mut_id(world_, ids[i], static_cast<ecs_entity_t>(world_id_)));
    if (w != nullptr) {
      MatIdentity(w->m);
    }
  }
  return count;
}

}  // namespace engine::ecs
