// prefab.hpp — P6-09: prefab assets (scene-subset files) + instances.
//
// A prefab asset is a JSON file sharing the P2-06 scene component payloads
// but with its own envelope:
//
//   {"format":"engine-prefab","version":1,"entities":[
//     {"id":1,"name":"Root","parent":0,"components":{...}},
//     {"id":2,"name":"Child","parent":1,"components":{...}}]}
//
// An entity object may carry a `PrefabRef` sibling key instead of (or in
// addition to) `components`:
//
//   {"id":3,"name":"Nested","parent":1,
//    "PrefabRef":{"asset":"props/lamp.engine-prefab"}}
//
// which mounts another prefab's instance tree at that point on
// instantiate (nested prefabs, by reference — never inlined).
//
// Instances are tracked in a PrefabRegistry (one per world, owned by the
// bridge EngineWorldImpl like the P2-05 drain registry):
//   * each asset path maps to one flecs prefab anchor entity; every
//     instance ROOT carries (EcsIsA, anchor), so the existing
//     `editor_is_prefab_instance` probe keeps working unchanged;
//   * each instance root maps to its asset path + base image (component
//     bytes captured at instantiate / last apply / last update).
//
// Override semantics are a three-way merge at byte granularity:
//   new_live = (live == base) ? asset : live
// so editing one LocalTransform lane (e.g. position.x) keeps that byte
// range while every other lane follows the asset. apply() writes live
// state back to the asset file and rebases (overrides cleared);
// revert() replays the base image onto live state.
//
// Save/load of assets with nested PrefabRef links requires the caller to
// supply a link resolver (the bridge passes the registry-backed one; the
// native test passes a null-resolver that treats mount points as plain
// entities). All functions are noexcept; errors surface as int32_t codes
// shared with the scene/ABI error space (0 = ok, negative = hard error,
// positive = skip/warning counts where documented).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#include "engine/scene/scene_io.hpp"

struct ecs_world_t;

namespace engine::prefab {

// Error codes (negative; 0 = ok). Positive returns are documented counts.
constexpr int kPrefabOk = 0;
constexpr int kPrefabNullArg = -1;
constexpr int kPrefabIoError = -2;
constexpr int kPrefabBadFormat = -3;
constexpr int kPrefabNoLocal = -4;
constexpr int kPrefabUnknownAsset = -5;  // mount asset missing/unreadable
constexpr int kPrefabNotInstance = -6;  // entity is no registered root
constexpr int kPrefabCycle = -7;        // nested mount cycle, mount skipped

// One captured component row of a base image.
struct PrefabBaseComp {
  std::string name;  // component name ("LocalTransform" for the pose)
  std::vector<std::uint8_t> bytes;  // raw component bytes (10 floats for pose)
};

// One captured entity row of a base image (entity = instance-relative
// file-local id at capture time, matching the asset's id space).
struct PrefabBaseRow {
  std::uint32_t file_id{0};
  std::string name;
  std::uint32_t parent_file_id{0};
  std::vector<PrefabBaseComp> comps;
};

// Base image of one instance: rows in asset id space.
struct PrefabBaseImage {
  std::vector<PrefabBaseRow> rows;
};

// Link resolver: maps an asset path to its parsed entity list during
// save (instance root -> asset for PrefabRef emission). Returns true and
// fills out_asset when `entity` is a registered instance root.
using LinkResolveFn = bool (*)(std::uint64_t entity, std::string& out_asset,
                               void* user);

// Save the subtree rooted at `root` to `path` as an engine-prefab file.
// Nested instance subtrees (IsA-anchored roots below `root`, resolved via
// `resolve`) are emitted as PrefabRef mount points with their computed
// overrides; everything else is inlined. Null resolve inlines everything.
// Returns kPrefabOk or a negative kPrefab* code.
ENGINE_CORE_API auto save_prefab(ecs_world_t* world, std::uint64_t root,
                                 const char* path, scene::SceneDescribeFn describe,
                                 void* describe_user, LinkResolveFn resolve,
                                 void* resolve_user) noexcept -> int32_t;

// Parse one prefab asset file, decoding component payloads via `describe`
// (named user fields; null == blob-only path). Returns kPrefabOk with rows
// filled, or a negative kPrefab* code (rows untouched). `out_mounts`
// parallels `rows`: nonempty asset path when that entity is a PrefabRef
// mount point.
ENGINE_CORE_API auto parse_prefab_file(
    const char* path, scene::SceneDescribeFn describe, void* user,
    std::vector<PrefabBaseRow>& out_rows,
    std::vector<std::string>& out_mounts) noexcept -> int32_t;

// Serialize rows (asset id space) to the engine-prefab envelope. Used by
// save_prefab (live world -> rows) and apply (live instance -> file).
ENGINE_CORE_API auto serialize_prefab_rows(
    const std::vector<PrefabBaseRow>& rows,
    const std::vector<std::string>& mounts, std::string& out) noexcept
    -> int32_t;

// Registry: per-world instance tracking. Owned by the bridge
// EngineWorldImpl (created with the world, destroyed in ApiWorldDestroy).
class ENGINE_CORE_API PrefabRegistry {
 public:
  PrefabRegistry() noexcept = default;
  PrefabRegistry(const PrefabRegistry&) = delete;
  auto operator=(const PrefabRegistry&) -> PrefabRegistry& = delete;
  ~PrefabRegistry() noexcept = default;

  // Anchor prefab entity for `asset` (creates a flecs prefab on demand).
  // Returns 0 when the world is null/unbound.
  auto anchor_for(ecs_world_t* world, const std::string& asset) noexcept
      -> std::uint64_t;

  // Look up the asset path an anchor entity was created for. Empty when
  // `anchor` is not a registry anchor.
  auto asset_for_anchor(std::uint64_t anchor) const noexcept -> std::string;

  // True when `entity` carries (IsA, anchor) for a registered anchor.
  auto is_instance_root(ecs_world_t* world,
                        std::uint64_t entity) const noexcept -> bool;

  // Asset path for a registered instance root. Empty when unregistered.
  auto asset_for_instance(std::uint64_t root) const noexcept -> std::string;

  // Register an instance root with its asset + base image (replaces any
  // prior registration for `root`).
  auto register_instance(std::uint64_t root, const std::string& asset,
                         PrefabBaseImage image) noexcept -> void;

  // Base image for a registered root (nullptr when unregistered).
  auto base_for(std::uint64_t root) const noexcept
      -> const PrefabBaseImage*;

  // Mutable base image for a registered root (nullptr when unregistered).
  auto base_for_mut(std::uint64_t root) noexcept -> PrefabBaseImage*;

  // Drop one registration (e.g. entity destroyed). No-op when absent.
  auto unregister(std::uint64_t root) noexcept -> void;

  // All live-registered roots for `asset` (prunes dead entities when
  // `world` is non-null).
  auto instances_of(ecs_world_t* world, const std::string& asset) noexcept
      -> std::vector<std::uint64_t>;

 private:
  std::vector<std::pair<std::string, std::uint64_t>> anchors_;
  struct Instance {
    std::uint64_t root{0};
    std::string asset;
    PrefabBaseImage base;
  };
  std::vector<Instance> instances_;
};

// Instantiate the prefab asset at `prefab_path` into `world`:
// materialize entities (nested PrefabRef mounts recurse with cycle guard),
// attach (IsA, anchor) to the instance root, capture the base image into
// `registry`. Returns kPrefabOk with out_root set, or a negative code.
ENGINE_CORE_API auto instantiate_prefab(
    ecs_world_t* world, const char* prefab_path, scene::SceneDescribeFn describe,
    void* describe_user, PrefabRegistry& registry,
    std::uint64_t* out_root) noexcept -> int32_t;

// Reload `prefab_path` and three-way-merge every live registered instance:
// new_live = (live == base) ? asset : live, per component payload.
// Rebases merged rows to the new asset image. Returns kPrefabOk with the
// instance count in out_updated, or a negative code (file errors abort
// before touching any instance).
ENGINE_CORE_API auto update_prefab_instances(
    ecs_world_t* world, const char* prefab_path, scene::SceneDescribeFn describe,
    void* describe_user, PrefabRegistry& registry,
    std::uint64_t* out_updated) noexcept -> int32_t;

// Write the live subtree of registered instance `root` back to its asset
// file (nested links stay links; overrides fold into the file), then
// rebase the base image to live. Returns kPrefabOk or a negative code.
ENGINE_CORE_API auto apply_prefab(ecs_world_t* world, std::uint64_t root,
                                  scene::SceneDescribeFn describe, void* describe_user,
                                  PrefabRegistry& registry) noexcept -> int32_t;

// Replay the base image onto the live subtree of registered instance
// `root` (overrides cleared by construction). Returns kPrefabOk or a
// negative code.
ENGINE_CORE_API auto revert_prefab(ecs_world_t* world, std::uint64_t root,
                                   PrefabRegistry& registry) noexcept
    -> int32_t;

}  // namespace engine::prefab
