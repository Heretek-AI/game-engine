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

// Idempotent name registration shared by EcsWorld::register_render() and
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
