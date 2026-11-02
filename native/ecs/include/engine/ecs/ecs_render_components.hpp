// ecs_render_components.hpp — P1-08: ECS render components.
//
// Camera, MeshRenderer, DirectionalLight: the native components the
// ForwardRenderer::render_world query consumes, and the P1-09 C#
// structs (`Engine.Runtime.Camera` etc.) bind to.
//
// Layouts (spec P1-08; P1-09 size asserts depend on these exactly):
//   Camera           { float fov_y_rad, near_z, far_z; uint32_t is_main; }
//                    16 bytes, align 4.
//   MeshRenderer     { uint32_t mesh_id; float base_color[4]; }
//                    20 bytes, align 4.
//   DirectionalLight { float direction[3]; float intensity; }
//                    16 bytes, align 4.
// P6-06 2D layouts (size asserts in the sprite test depend on these):
//   SpriteRenderer2D { uint32_t atlas_id; int32_t sorting_layer;
//                      int32_t sort_order; float uv_rect[4];
//                      float color[4]; }  44 bytes, align 4.
//   OrthoCamera      { float height_world; float aspect; float pos[2];
//                      uint32_t pixel_perfect; float pixels_per_unit;
//                      float viewport_px[2]; float near_z; float far_z; }
//                    40 bytes, align 4.
//
// Registration rule (spec §6 gotcha 20): register through the shared
// name-registration helper (RegisterRenderComponent, same idempotent
// lookup-or-init semantics as GenericSystems::register_component under
// the full names "Engine.Runtime.Camera" / "Engine.Runtime.MeshRenderer"
// / "Engine.Runtime.DirectionalLight") — never ECS_COMPONENT or
// ecs_component_init directly. Any other name, path, or size makes
// Flecs abort or creates a second component that C# never sees.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;

namespace engine::ecs {

struct Camera {
  float fov_y_rad{0.0F};
  float near_z{0.0F};
  float far_z{0.0F};
  std::uint32_t is_main{0};
};
static_assert(sizeof(Camera) == 16, "Camera must be 16 bytes");
static_assert(alignof(Camera) == 4, "Camera must be 4-byte aligned");

struct MeshRenderer {
  std::uint32_t mesh_id{0};
  float base_color[4]{0.0F, 0.0F, 0.0F, 0.0F};
};
static_assert(sizeof(MeshRenderer) == 20, "MeshRenderer must be 20 bytes");
static_assert(alignof(MeshRenderer) == 4, "MeshRenderer must be 4-byte aligned");

struct DirectionalLight {
  float direction[3]{0.0F, 0.0F, 0.0F};
  float intensity{0.0F};
};
static_assert(sizeof(DirectionalLight) == 16,
              "DirectionalLight must be 16 bytes");
static_assert(alignof(DirectionalLight) == 4,
              "DirectionalLight must be 4-byte aligned");

// P6-06: 2D sprite draw (position/size come from LocalTransform +
// WorldTransform like the 3D path; the atlas region + tint live here).
struct SpriteRenderer2D {
  std::uint32_t atlas_id{0};
  std::int32_t sorting_layer{0};
  std::int32_t sort_order{0};
  float uv_rect[4]{0.0F, 0.0F, 1.0F, 1.0F};
  float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
};
static_assert(sizeof(SpriteRenderer2D) == 44,
              "SpriteRenderer2D must be 44 bytes");
static_assert(alignof(SpriteRenderer2D) == 4,
              "SpriteRenderer2D must be 4-byte aligned");

// P6-06: orthographic 2D camera (mirrors renderer::OrthoCamera2D field
// order so OrthoViewProj() can consume it directly).
struct OrthoCamera {
  float height_world{10.0F};
  float aspect{16.0F / 9.0F};
  float pos[2]{0.0F, 0.0F};
  std::uint32_t pixel_perfect{0};
  float pixels_per_unit{100.0F};
  float viewport_px[2]{640.0F, 360.0F};
  float near_z{-10.0F};
  float far_z{10.0F};
};
static_assert(sizeof(OrthoCamera) == 40, "OrthoCamera must be 40 bytes");
static_assert(alignof(OrthoCamera) == 4,
              "OrthoCamera must be 4-byte aligned");

// P6-08: grid tilemap draw (tile data lives in a managed-side TilemapAsset
// row-major id grid; the native component carries the grid frame + palette
// index so the batch planner can emit one quad per non-empty tile).
//   TilemapRenderer2D { width, height, tile_size, origin[2], atlas_id,
//                       sorting_layer, sort_order, pad }  32 bytes, align 4.
// P6-08: tilemap collider source (ECS-side marker; the merge itself runs
// in renderer::TilemapMergeSolids over the solid bitmask).
//   TilemapCollider2D { tile_size, solid_tile_count, pad[2] }
//                     16 bytes, align 4.
struct TilemapRenderer2D {
  std::uint32_t width{0};
  std::uint32_t height{0};
  float tile_size{1.0F};
  float origin[2]{0.0F, 0.0F};
  std::uint32_t atlas_id{0};
  std::int32_t sorting_layer{0};
  std::int32_t sort_order{0};
};
static_assert(sizeof(TilemapRenderer2D) == 32,
              "TilemapRenderer2D must be 32 bytes");
static_assert(alignof(TilemapRenderer2D) == 4,
              "TilemapRenderer2D must be 4-byte aligned");

struct TilemapCollider2D {
  float tile_size{1.0F};
  std::uint32_t solid_tile_count{0};
  std::uint32_t pad[2]{0, 0};
};
static_assert(sizeof(TilemapCollider2D) == 16,
              "TilemapCollider2D must be 16 bytes");
static_assert(alignof(TilemapCollider2D) == 4,
              "TilemapCollider2D must be 4-byte aligned");

// P5-05: GPU particle emitter settings + live simulation state.
//
//   ParticleSystemComponent { emission_rate, burst_count, burst_interval,
//     lifetime, start_speed, start_size, start_color[4], gravity,
//     end_color[4], end_size, blend, seed, pad, alive, spawned_total,
//     emit_carry, burst_clock }  92 bytes, align 4.
//
// Mirrors renderer::ParticleSystem field-for-field for the first 76 bytes
// (same offsets, so the params SSBO upload is a prefix copy); the trailing
// 16 bytes are live simulation state owned by native (never written by
// C# — the C# mirror carries the full 92 bytes so sizes agree, and user
// code only sets the settings prefix). Registered under
// "Engine.Runtime.ParticleSystem" via RegisterRenderComponent (same
// idempotent helper as the render components; the C# mirror in
// Particles.cs must match this size exactly).
struct ParticleSystemComponent {
  float emission_rate{0.0F};
  float burst_count{0.0F};
  float burst_interval{0.0F};
  float lifetime{1.0F};
  float start_speed{1.0F};
  float start_size{1.0F};
  float start_color[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float gravity{0.0F};
  float end_color[4]{1.0F, 1.0F, 1.0F, 0.0F};
  float end_size{1.0F};
  std::uint32_t blend{0};
  std::uint32_t seed{0};
  std::uint32_t pad{0};
  // Live state (native-owned; updated by the simulate step).
  std::uint32_t alive{0};
  std::uint32_t spawned_total{0};
  float emit_carry{0.0F};
  float burst_clock{0.0F};
};
static_assert(sizeof(ParticleSystemComponent) == 92,
              "ParticleSystemComponent must be 92 bytes");
static_assert(alignof(ParticleSystemComponent) == 4,
              "ParticleSystemComponent must be 4-byte aligned");
// the ABI bridge (P1-09): looks up `name` with an empty separator (so
// "Engine.Runtime.Camera" stays one flat name), returns the existing id
// when size/alignment match, registers when absent, and returns 0 on a
// layout mismatch (never aborts — Flecs would abort the process on a
// size-mismatched ecs_component_init, gotcha 20).
ENGINE_CORE_API auto RegisterRenderComponent(ecs_world_t* world,
                                             const char* name, std::uint32_t size,
                                             std::uint32_t alignment) noexcept
    -> std::uint64_t;

}  // namespace engine::ecs
