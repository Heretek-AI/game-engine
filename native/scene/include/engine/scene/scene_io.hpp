// scene_io.hpp — P2-06: scene files (JSON v1) save and load.
//
// Format (spec §P2-06, exact):
//   {"format":"engine-scene","version":1,"entities":[
//     {"id":1,"name":"Floor","parent":0,"components":{
//       "LocalTransform":{"position":[0,0,0],"rotation":[0,0,0,1],
//                         "scale":[10,0.2,10]},
//       "MeshRenderer":{"mesh":"assets/cube.glb#0",
//                       "base_color":[0.8,0.8,0.8,1]},
//       "Mover":{"$bytes":"0000803f00000000","$size":8,"$schema":"a1b2c3d4"}}}]}
//
// Rules:
//   * Built-ins (LocalTransform, Camera, MeshRenderer, DirectionalLight)
//     get named fields. Everything else is a hex blob: $bytes + $size +
//     $schema, where $schema is FNV-1a 32 of "name:size:align" rendered as
//     8 lowercase hex digits. Mismatch on load -> skip with ONE warning
//     printed per bad component, never a crash.
//   * id/parent are file-local (1..N, parent 0 = root).
//   * Meshes serialize as asset path + "#" + submesh index (never runtime
//     mesh_id). The renderer owns no path table, so this module keeps a
//     process-lifetime registry (scene_register_mesh_source) mapping
//     mesh_id -> "path#index". An id with no registry entry writes an empty
//     mesh string (loads as mesh_id 0 = "no mesh"); the reader still
//     accepts the legacy "mesh_id:<n>" form from older files.
//   * JSON reader/writer live in this module (no new dependency); floats
//     print via std::to_chars shortest round-trip.
//   * save_scene filters to entities carrying LocalTransform (the scene
//     membership marker, same as editor snapshots); WorldTransform is
//     derived and never serialized.
//   * Deterministic order: parents before children (creation order within
//     each sibling set), so save -> load -> save is byte-identical.
//   * noexcept throughout (bridge boundary rule); errors surface as
//     int32_t codes shared with the ABI error space.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;

namespace engine::scene {

// Error codes (negative; 0 = ok). Positive return from load_scene counts
// skipped bad user components (warnings already printed).
constexpr int kSceneOk = 0;
constexpr int kSceneNullArg = -1;
constexpr int kSceneIoError = -2;
constexpr int kSceneBadFormat = -3;
constexpr int kSceneNoLocal = -4;

// Save every LocalTransform-carrying entity in `world` to `path` (UTF-8).
// Returns kSceneOk or a negative kScene* code.
ENGINE_CORE_API auto save_scene(ecs_world_t* world, const char* path) noexcept
    -> int32_t;

// Load the scene file at `path` into `world` (existing Local entities are
// left alone; file entities are created fresh with file-local parent links
// replayed). Unknown components are registered on demand with the recorded
// size/alignment; size/schema mismatches are skipped with one warning each.
// Returns the skip count (>= 0), or a negative kScene* code on hard error.
ENGINE_CORE_API auto load_scene(ecs_world_t* world, const char* path) noexcept
    -> int32_t;

// Mesh source registry: "assets/cube.glb#0" style reference for a runtime
// mesh_id. Load resolves through this; an unregistered id writes an empty
// mesh string (never a runtime id).
ENGINE_CORE_API auto register_mesh_source(std::uint32_t mesh_id,
                                          const char* ref) noexcept -> void;
ENGINE_CORE_API auto clear_mesh_sources() noexcept -> void;
// Reverse lookup for the reader: ref -> mesh_id, 0 when unknown.
ENGINE_CORE_API auto mesh_id_for_ref(const char* ref) noexcept
    -> std::uint32_t;

// FNV-1a 32 of "name:size:align" (the $schema hash). Exposed for tests.
ENGINE_CORE_API auto schema_hash(const char* name, std::uint32_t size,
                                 std::uint32_t align) noexcept -> std::uint32_t;

}  // namespace engine::scene
