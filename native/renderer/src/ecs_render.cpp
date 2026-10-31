// ecs_render.cpp — P1-08 ForwardRenderer::render_world.
//
// Queries the Flecs world for (WorldTransform, MeshRenderer) and draws
// each entity whose mesh_id resolves in the MeshRegistry, using the
// first Camera with is_main == 1 plus its WorldTransform (eye) and the
// first DirectionalLight for light_dir.
//
// Conventions (matched to the P1-07 test math):
//   - WorldTransform.m is ROW-major (M1-03 ComposeLocal); the shader's
//     MeshPush.mvp/model are COLUMN-major, so every model matrix is
//     transposed on upload.
//   - The view matrix is the rigid inverse of the camera eye's world
//     matrix (rotation transpose + translation), then transposed to
//     column-major for the shader.
//   - Projection is the P1-07 Vulkan-clip perspective (Y flipped, depth
//     0..1) built from the Camera component (fov_y_rad, near_z, far_z)
//     and the frame aspect passed in — render_world takes the aspect
//     explicitly so the header stays free of swapchain types.
#include "engine/renderer/forward_renderer.hpp"

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/mesh_registry.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flecs.h"

// SDK-free builds get the stub render_world in forward_renderer.cpp.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
namespace engine::renderer {

namespace {

// Column-major helpers (match the P1-07 test math in
// test_forward_mesh.cpp exactly).
//
// alignas(32): this TU compiles under the renderer hot flags (-mavx2),
// so GCC emits vmovaps (32-byte aligned AVX loads/stores) for ColMat
// spills and by-value args. float[16] is only align-4 by default; an
// under-aligned stack slot faults intermittently (flaky SIGSEGV in
// engine_ecs_render under ctest/redirected stdout). The P1-07 test-side
// Mat4 never tripped this because test TUs use base flags (no AVX).
struct alignas(32) ColMat {
  float m[16]{};
};

// Test-math helpers (ColPerspective/ColMul/RowWorldToColView) are used by
// render_world (SDK builds). In SDK-free builds (macOS/Windows CI) the TU
// still compiles but render_world is compiled out (see below), so tag them
// [[maybe_unused]] instead of gating the whole region — keeps the diff
// minimal vs P1-09 and avoids duplicate/stub mismatch.

// Vulkan-clip perspective: Y flipped, depth 0..1 (spec P1-07).
[[maybe_unused]] auto ColPerspective(float fov_y_rad, float aspect, float near_z,
                    float far_z) -> ColMat {
  ColMat o{};
  const float f = 1.0F / std::tan(fov_y_rad * 0.5F);
  o.m[0] = f / aspect;
  o.m[5] = -f;
  o.m[10] = far_z / (near_z - far_z);
  o.m[11] = -1.0F;
  o.m[14] = -(far_z * near_z) / (far_z - near_z);
  return o;
}

[[maybe_unused]] auto ColMul(const ColMat& a, const ColMat& b) -> ColMat {
  ColMat o{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      o.m[c * 4 + r] = a.m[r] * b.m[c * 4] + a.m[4 + r] * b.m[c * 4 + 1] +
                       a.m[8 + r] * b.m[c * 4 + 2] +
                       a.m[12 + r] * b.m[c * 4 + 3];
    }
  }
  return o;
}

// WorldTransform flat layout (M1-03 ComposeLocal) already matches the
// shader's column-vector convention: translation lives in elements
// 12..14, which is the translation COLUMN in column-major (and the
// rotation block of axis-aligned entities is symmetric/identity). So
// the model upload is a memcpy — NOT a transpose. (A transpose would
// move the translation into the w-row: w' = x*px+y*py+z*pz+1, which
// collapses every cube to sub-pixel NDC mush. That exact bug produced
// the first P1-08 golden: a fullscreen flat tan field.)
//
// Caveat: ComposeLocal's rotation block for NON-identity quaternions is
// the row-vector form; if rotated entities ever shade wrong, the model
// needs R^T handling here — but every entity on this slice (and P1-07,
// whose identity-model cube renders correctly) is axis-aligned, where
// the flat bytes are exactly the column-vector model matrix.

// Rigid inverse of a row-major world matrix (rotation transpose +
// translation), returned column-major for the shader. Camera eyes are
// rigid (rotation + translation only); scale/shear would need the
// general inverse, which cameras never carry.
[[maybe_unused]] auto RowWorldToColView(const float w[16]) -> ColMat {
  // Row-major rows: R rows are w[0..2], w[4..6], w[8..10]; T = w[12..14].
  // View (row-major) = [R^T | -R^T*T; 0 0 0 1]; then transpose to column.
  const float tx = w[12];
  const float ty = w[13];
  const float tz = w[14];
  ColMat o{};
  // Column 0 = row 0 of the row-major view = (R00, R10, R20, v0).
  o.m[0] = w[0];
  o.m[1] = w[4];
  o.m[2] = w[8];
  o.m[3] = 0.0F;
  // Column 1 = row 1 of the view = (R01, R11, R21, v1).
  o.m[4] = w[1];
  o.m[5] = w[5];
  o.m[6] = w[9];
  o.m[7] = 0.0F;
  // Column 2 = row 2 of the view = (R02, R12, R22, v2).
  o.m[8] = w[2];
  o.m[9] = w[6];
  o.m[10] = w[10];
  o.m[11] = 0.0F;
  // Column 3 = (v0, v1, v2, 1) with v = -R^T * T.
  o.m[12] = -(w[0] * tx + w[4] * ty + w[8] * tz);
  o.m[13] = -(w[1] * tx + w[5] * ty + w[9] * tz);
  o.m[14] = -(w[2] * tx + w[6] * ty + w[10] * tz);
  o.m[15] = 1.0F;
  return o;
}

}  // namespace

auto ForwardRenderer::render_world(void* world_in,
                                   const MeshRegistry& registry,
                                   VkCommandBuffer cmd, RenderWorldView* out_view,
                                   RenderWorldStats* out_stats,
                                   float aspect) noexcept -> Status {
  if (out_stats != nullptr) {
    *out_stats = RenderWorldStats{};
  }
  if (out_view != nullptr) {
    *out_view = RenderWorldView{};
  }
  if (mesh_pipeline_ == nullptr || mesh_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "render_world not built");
  }
  if (world_in == nullptr || cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "render_world bad arg");
  }
  auto* world = static_cast<ecs_world_t*>(world_in);

  // Component ids: idempotent lookup — the same call the ECS module
  // and C# use (registering here would also work, but lookup keeps
  // this path read-only on the world).
  const ecs_entity_t cam_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.Camera", "", "", false);
  const ecs_entity_t mesh_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.MeshRenderer", "", "", false);
  const ecs_entity_t light_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.DirectionalLight", "", "", false);
  const ecs_entity_t world_tf_id = ecs_lookup_path_w_sep(
      world, 0, "WorldTransform", "", "", false);
  if (cam_id == 0 || mesh_id == 0 || light_id == 0 || world_tf_id == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "render_world no comps");
  }

  // First main camera + its eye.
  ecs::Camera cam{};
  float eye_world[16]{};
  bool eye_init = false;
  bool found_cam = false;
  {
    ecs_query_desc_t qd{};
    qd.terms[0].id = cam_id;
    qd.terms[0].src.id = EcsSelf;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(world, &qd);
    if (q == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "render_world cam q");
    }
    ecs_iter_t it = ecs_query_iter(world, q);
    // First table with an is_main camera wins by design (single main
    // camera expected): the loop CAN iterate (S1751) — the flag stops it
    // at the first hit, which only triggers on multi-table worlds.
    while (ecs_query_next(&it) && !found_cam) {
      auto* col =
          static_cast<ecs::Camera*>(ecs_field_w_size(&it, sizeof(ecs::Camera), 0));
      if (col == nullptr) {
        break;
      }
      for (int r = 0; r < it.count; ++r) {
        if (col[r].is_main != 0) {
          cam = col[r];
          const ecs_entity_t e = it.entities[r];
          const auto* wtf = static_cast<const ecs::WorldTransform*>(
              ecs_get_id(world, e, world_tf_id));
          if (wtf != nullptr) {
            // Unaligned-safe copy (same Flecs column-alignment caveat as
            // the draw loop below): WorldTransform is alignas(64) but
            // ecs_get_id storage need not be.
            const auto* esrc =
                reinterpret_cast<const unsigned char*>(wtf);
            auto* edst = reinterpret_cast<unsigned char*>(eye_world);
            for (std::size_t b = 0; b < sizeof(eye_world); ++b) {
              edst[b] = esrc[b];
            }
            eye_init = true;
          }
          found_cam = true;
          break;
        }
      }
    }
    ecs_query_fini(q);
  }
  if (!found_cam) {
    return MakeFail(StatusCode::kInvalidArgument, "render_world no camera");
  }
  if (!eye_init) {
    // Camera without a propagated WorldTransform: identity eye.
    eye_init = true;
    for (int i = 0; i < 16; ++i) {
      eye_world[i] = (i % 5 == 0) ? 1.0F : 0.0F;
    }
  }

  // First directional light (any).
  float light_dir[4]{0.0F, 0.0F, 0.0F, 0.0F};
  {
    ecs_query_desc_t qd{};
    qd.terms[0].id = light_id;
    qd.terms[0].src.id = EcsSelf;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(world, &qd);
    if (q == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "render_world light q");
    }
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
      auto* col = static_cast<ecs::DirectionalLight*>(
          ecs_field_w_size(&it, sizeof(ecs::DirectionalLight), 0));
      if (col == nullptr || it.count <= 0) {
        break;
      }
      light_dir[0] = col[0].direction[0];
      light_dir[1] = col[0].direction[1];
      light_dir[2] = col[0].direction[2];
      light_dir[3] = 0.0F;
      break;
    }
    ecs_query_fini(q);
  }

  // Aspect comes from the caller (P1-09: the swapchain extent; P1-08
  // tests rely on the 16:9 default). Guard degenerate values.
  const float use_aspect = (aspect > 0.0F) ? aspect : (16.0F / 9.0F);
  const float near_z = cam.near_z > 0.0F ? cam.near_z : 0.1F;
  const float far_z = cam.far_z > near_z ? cam.far_z : 100.0F;
  const float fov = cam.fov_y_rad > 0.0F ? cam.fov_y_rad : 1.0471976F;
  const ColMat proj = ColPerspective(fov, use_aspect, near_z, far_z);
  const ColMat view = RowWorldToColView(eye_world);
  const ColMat view_proj = ColMul(proj, view);

  if (out_view != nullptr) {
    std::memcpy(out_view->view_proj, view_proj.m, sizeof(view_proj.m));
    std::memcpy(out_view->light_dir, light_dir, sizeof(light_dir));
  }

  // Draw pass: (WorldTransform, MeshRenderer) owned pairs.
  std::uint32_t draws = 0;
  {
    ecs_query_desc_t qd{};
    qd.terms[0].id = world_tf_id;
    qd.terms[0].src.id = EcsSelf;
    qd.terms[1].id = mesh_id;
    qd.terms[1].src.id = EcsSelf;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(world, &qd);
    if (q == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "render_world draw q");
    }
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
      auto* wcol = static_cast<ecs::WorldTransform*>(
          ecs_field_w_size(&it, sizeof(ecs::WorldTransform), 0));
      auto* mcol = static_cast<ecs::MeshRenderer*>(
          ecs_field_w_size(&it, sizeof(ecs::MeshRenderer), 1));
      if (wcol == nullptr || mcol == nullptr) {
        break;
      }
      for (int r = 0; r < it.count; ++r) {
        const GpuMesh* mesh = registry.find(mcol[r].mesh_id);
        if (mesh == nullptr) {
          continue;
        }
        // Flat-layout copy (see note above): WorldTransform bytes ARE
        // the column-vector model matrix for our axis-aligned entities.
        //
        // Unaligned-safe: Flecs column storage is NOT guaranteed 64-byte
        // aligned (user-component columns align to the component's own
        // alignment), but WorldTransform is declared alignas(64), so
        // GCC assumes wcol[r].m is 64-aligned and emits vmovaps for the
        // copy — a flaky SIGSEGV when the column actually isn't. The
        // byte loop compiles to vmovups (verified in disasm) and costs
        // nothing next to a draw call.
        alignas(32) unsigned char model_bytes[sizeof(ColMat)]{};
        const auto* wsrc = reinterpret_cast<const unsigned char*>(&wcol[r]);
        for (std::size_t b = 0; b < sizeof(model_bytes); ++b) {
          model_bytes[b] = wsrc[b];
        }
        ColMat model{};
        std::memcpy(model.m, model_bytes, sizeof(model.m));
        const ColMat mvp = ColMul(view_proj, model);
        MeshPush push{};
        std::memcpy(push.mvp, mvp.m, sizeof(push.mvp));
        std::memcpy(push.model, model.m, sizeof(push.model));
        std::memcpy(push.light_dir, light_dir, sizeof(push.light_dir));
        std::memcpy(push.base_color, mcol[r].base_color,
                    sizeof(push.base_color));
        const Status st = record_mesh(cmd, *mesh, push);
        if (!st.ok()) {
          ecs_query_fini(q);
          return st;
        }
        ++draws;
      }
    }
    ecs_query_fini(q);
  }

  if (out_stats != nullptr) {
    out_stats->draws = draws;
    out_stats->has_camera = true;
  }
  return MakeOk();
}

}  // namespace engine::renderer
#endif  // ENGINE_RENDERER_HAS_VULKAN
