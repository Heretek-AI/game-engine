// mesh_test_helpers.hpp — P5-02 shared GPU-test helpers (NOT a golden).
//
// The mesh-family GPU tests (test_forward_mesh, test_ecs_render,
// test_pick_ids) spell the same CPU-side scaffolding: a procedural unit
// cube, a minimal column-major mat4 suite (Vulkan clip space), and the
// color/depth image barriers for dynamic-rendering passes. One header
// keeps the three TUs from tripping the Sonar duplication gate
// (P4-01 precedent: golden_pipe_helper.hpp, golden_vk_helper.hpp).
//
// Header-only, Vulkan-guarded (stub builds skip it). Callers include it
// inside their anonymous namespace and use their own check()/failures.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

namespace engine::test {

// --- procedural unit cube: 24 verts (4/face, face normals), 36 indices --
struct HelperCube {
  std::array<::engine::renderer::MeshVertex, 24> verts{};
  std::array<std::uint32_t, 36> idx{};
};

inline auto make_helper_cube() -> HelperCube {
  HelperCube c{};
  // faces: normal + 4 CCW corners (viewed from outside).
  const float faces[6][7][3] = {
      // +X
      {{1, 0, 0}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
      // -X
      {{-1, 0, 0}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}},
      // +Y
      {{0, 1, 0}, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
      // -Y
      {{0, -1, 0}, {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}},
      // +Z
      {{0, 0, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
      // -Z
      {{0, 0, -1}, {1, -1, -1}, {-1, -1, -1}, {1, 1, -1}, {-1, 1, -1}},
  };
  for (int f = 0; f < 6; ++f) {
    for (int v = 0; v < 4; ++v) {
      auto& out = c.verts[static_cast<std::size_t>(f) * 4 + v];
      out.pos[0] = faces[f][1 + v][0] * 0.5F;
      out.pos[1] = faces[f][1 + v][1] * 0.5F;
      out.pos[2] = faces[f][1 + v][2] * 0.5F;
      out.normal[0] = faces[f][0][0];
      out.normal[1] = faces[f][0][1];
      out.normal[2] = faces[f][0][2];
      out.uv[0] = (v == 0 || v == 3) ? 0.0F : 1.0F;
      out.uv[1] = (v < 2) ? 0.0F : 1.0F;
    }
    const std::uint32_t b = static_cast<std::uint32_t>(f) * 4;
    const std::uint32_t quads[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
    for (int k = 0; k < 6; ++k) {
      c.idx[static_cast<std::size_t>(f) * 6 + k] = quads[k];
    }
  }
  return c;
}

// --- minimal column-major mat4 (Vulkan clip space) ----------------------
struct HelperMat4 {
  float m[16]{};
};

inline auto helper_mat_identity() -> HelperMat4 {
  HelperMat4 o{};
  o.m[0] = o.m[5] = o.m[10] = o.m[15] = 1.0F;
  return o;
}

inline auto helper_mat_translate(float x, float y, float z) -> HelperMat4 {
  HelperMat4 o = helper_mat_identity();
  o.m[12] = x;
  o.m[13] = y;
  o.m[14] = z;
  return o;
}

// Vulkan perspective: Y flipped, depth 0..1.
inline auto helper_mat_perspective_vk(float fov_y_rad, float aspect,
                                      float near_z, float far_z)
    -> HelperMat4 {
  HelperMat4 o{};
  const float f = 1.0F / std::tan(fov_y_rad * 0.5F);
  o.m[0] = f / aspect;
  o.m[5] = -f;
  o.m[10] = far_z / (near_z - far_z);
  o.m[11] = -1.0F;
  o.m[14] = -(far_z * near_z) / (far_z - near_z);
  return o;
}

inline auto helper_mat_look_at(const float* eye, const float* center,
                               const float* up) -> HelperMat4 {
  float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
  const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  f[0] /= fl;
  f[1] /= fl;
  f[2] /= fl;
  float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                f[0] * up[1] - f[1] * up[0]};
  const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  s[0] /= sl;
  s[1] /= sl;
  s[2] /= sl;
  float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                s[0] * f[1] - s[1] * f[0]};
  HelperMat4 o = helper_mat_identity();
  o.m[0] = s[0];
  o.m[1] = u[0];
  o.m[2] = -f[0];
  o.m[4] = s[1];
  o.m[5] = u[1];
  o.m[6] = -f[1];
  o.m[8] = s[2];
  o.m[9] = u[2];
  o.m[10] = -f[2];
  o.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  o.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  o.m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
  return o;
}

inline auto helper_mat_mul(const HelperMat4& a, const HelperMat4& b)
    -> HelperMat4 {
  HelperMat4 o{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      o.m[c * 4 + r] = a.m[r] * b.m[c * 4] + a.m[4 + r] * b.m[c * 4 + 1] +
                       a.m[8 + r] * b.m[c * 4 + 2] +
                       a.m[12 + r] * b.m[c * 4 + 3];
    }
  }
  return o;
}

// Fill a 160-byte MeshPush-compatible block (mvp/model/light/base,
// each 16 floats/bytes... caller passes the TU's MeshPush fields).
inline void helper_fill_push(float* mvp_out, float* model_out,
                             float* light_out, float* base_out,
                             const HelperMat4& mvp, const HelperMat4& model,
                             const float* light_dir, const float* base_color) {
  std::memcpy(mvp_out, mvp.m, 64);
  std::memcpy(model_out, model.m, 64);
  std::memcpy(light_out, light_dir, 16);
  std::memcpy(base_out, base_color, 16);
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// Color barrier functor: UNDEFINED->ATTACHMENT style transitions for the
// swapchain image around dynamic-rendering passes. State-free (the entry
// point is passed per call so TUs keep their own proc tables).
struct HelperBarriers {
  PFN_vkCmdPipelineBarrier2 cmd_barrier2{nullptr};
  VkImage depth_image{nullptr};

  inline void color(VkCommandBuffer cmd, VkImage img, VkImageLayout old_l,
                    VkImageLayout new_l, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) const noexcept {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.dstStageMask = dst_stage;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    cmd_barrier2(cmd, &di);
  }

  inline void depth(VkCommandBuffer cmd, VkImageLayout old_l,
                    VkImageLayout new_l) const noexcept {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                         ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                         : (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                          ? 0
                          : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = depth_image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    cmd_barrier2(cmd, &di);
  }
};
#endif

}  // namespace engine::test
