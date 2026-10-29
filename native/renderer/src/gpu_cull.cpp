// gpu_cull.cpp — M3-02: CPU reference cull (bit-exact GPU contract).
//
// Frustum planes use the extraction the CULL shader implements:
// rows of the row-major VP matrix (left = r3+r0, right = r3-r0, ...),
// normalized. The HIZ reference implements the same conservative test:
// project the sphere, take the touched pyramid texel at the mip covering
// the rect, compare closest depth vs stored max-depth with 2x2 taps.
#include "engine/renderer/gpu_cull.hpp"

#include <cmath>
#include <cstring>

#include "engine/renderer/scene_ssbo.hpp"

namespace engine::renderer {

auto CullSphereInFrustum(const float sphere[4], const FrustumPlanes& fr)
    -> bool {
  if (sphere == nullptr) {
    return false;
  }
  const float cx = sphere[0];
  const float cy = sphere[1];
  const float cz = sphere[2];
  const float r = sphere[3];
  for (int p = 0; p < 6; ++p) {
    const float d = fr.planes[p][0] * cx + fr.planes[p][1] * cy +
                    fr.planes[p][2] * cz + fr.planes[p][3];
    if (d < -r) {
      return false;
    }
  }
  return true;
}

auto CullFrustumCpu(const void* scene_instances, std::uint32_t count,
                    const FrustumPlanes& fr, std::uint32_t* visible_out)
    -> std::uint32_t {
  if (scene_instances == nullptr || visible_out == nullptr || count == 0) {
    return 0;
  }
  const auto* inst = static_cast<const SceneInstance*>(scene_instances);
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (CullSphereInFrustum(inst[i].sphere, fr)) {
      visible_out[n++] = i;
    }
  }
  return n;
}

auto CullProjectSphere(const float sphere[4], const float mvp[16],
                       std::uint32_t fb_w, std::uint32_t fb_h,
                       std::uint32_t* x0_out, std::uint32_t* y0_out,
                       std::uint32_t* x1_out, std::uint32_t* y1_out,
                       float* closest_depth_out) -> bool {
  if (sphere == nullptr || mvp == nullptr || fb_w == 0 || fb_h == 0) {
    return false;
  }
  // Row-major clip = M * p for center and near-center point.
  const float cx = sphere[0];
  const float cy = sphere[1];
  const float cz = sphere[2];
  const float r = sphere[3] < 0.0F ? 0.0F : sphere[3];
  float clip[4];
  clip[0] = mvp[0] * cx + mvp[1] * cy + mvp[2] * cz + mvp[3];
  clip[1] = mvp[4] * cx + mvp[5] * cy + mvp[6] * cz + mvp[7];
  clip[2] = mvp[8] * cx + mvp[9] * cy + mvp[10] * cz + mvp[11];
  clip[3] = mvp[12] * cx + mvp[13] * cy + mvp[14] * cz + mvp[15];
  if (clip[3] <= 1e-6F) {
    return false;  // behind camera
  }
  const float inv_w = 1.0F / clip[3];
  const float ndc_x = clip[0] * inv_w;
  const float ndc_y = clip[1] * inv_w;
  const float ndc_z = clip[2] * inv_w;
  // Angular radius -> NDC radius (conservative, isotropic via min axis).
  const float sx = 0.5F * (mvp[0] + mvp[5]);  // ~focal proxy; guarded below
  float ndc_r = (sx > 1e-6F) ? (r * sx * inv_w) : 0.0F;
  if (!(ndc_r >= 0.0F)) {
    ndc_r = 0.0F;
  }
  const float fw = static_cast<float>(fb_w);
  const float fh = static_cast<float>(fb_h);
  float px = (ndc_x * 0.5F + 0.5F) * fw;
  float py = (ndc_y * 0.5F + 0.5F) * fh;
  float pr = ndc_r * 0.5F * (fw < fh ? fw : fh);
  if (px + pr < 0 || py + pr < 0 || px - pr > fw || py - pr > fh) {
    return false;  // fully off-screen
  }
  if (x0_out != nullptr) {
    *x0_out = static_cast<std::uint32_t>(px - pr < 0 ? 0 : px - pr);
  }
  if (y0_out != nullptr) {
    *y0_out = static_cast<std::uint32_t>(py - pr < 0 ? 0 : py - pr);
  }
  if (x1_out != nullptr) {
    *x1_out = static_cast<std::uint32_t>(px + pr > fw ? fw : px + pr);
  }
  if (y1_out != nullptr) {
    *y1_out = static_cast<std::uint32_t>(py + pr > fh ? fh : py + pr);
  }
  if (closest_depth_out != nullptr) {
    *closest_depth_out = ndc_z - ndc_r * 0.5F;
  }
  return true;
}

auto CullHizCpu(const void* scene_instances, const std::uint32_t* visible,
                std::uint32_t candidate_count, const float* pyramid,
                std::uint32_t pyr_w, std::uint32_t pyr_h,
                std::uint32_t pyr_mips, const float mvp[16],
                std::uint32_t fb_w, std::uint32_t fb_h,
                const CullMeshRange* meshes, std::uint32_t mesh_count,
                VkDrawIndexedIndirectCommand* draws_out) -> std::uint32_t {
  if (scene_instances == nullptr || visible == nullptr ||
      draws_out == nullptr || candidate_count == 0) {
    return 0;
  }
  const auto* inst = static_cast<const SceneInstance*>(scene_instances);
  std::uint32_t n = 0;
  for (std::uint32_t k = 0; k < candidate_count; ++k) {
    const std::uint32_t id = visible[k];
    std::uint32_t x0 = 0;
    std::uint32_t y0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y1 = 0;
    float closest = 1.0F;
    if (!CullProjectSphere(inst[id].sphere, mvp, fb_w, fb_h, &x0, &y0, &x1,
                           &y1, &closest)) {
      continue;
    }
    bool occluded = false;
    if (pyramid != nullptr && pyr_w > 0 && pyr_h > 0 && pyr_mips > 0) {
      // Mip covering the rect IN PYRAMID SPACE: scale the framebuffer rect
      // into pyramid texels first ((x0*tw)/fb_w), then shrink until it fits
      // one texel. Comparing framebuffer pixels against pyramid texels
      // picks the wrong level whenever pyr != fb size.
      const std::uint64_t fw64 = (fb_w > 0) ? fb_w : 1;
      const std::uint64_t fh64 = (fb_h > 0) ? fb_h : 1;
      std::uint32_t rw =
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(x1 - x0) *
                                      pyr_w) /
                                     fw64);
      std::uint32_t rh =
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(y1 - y0) *
                                      pyr_h) /
                                     fh64);
      if (rw == 0) {
        rw = 1;
      }
      if (rh == 0) {
        rh = 1;
      }
      std::uint32_t mip = 0;
      std::uint32_t tw = pyr_w;
      std::uint32_t th = pyr_h;
      while (mip + 1 < pyr_mips && (rw > 1 || rh > 1)) {
        rw = (rw + 1) / 2;
        rh = (rh + 1) / 2;
        tw = tw > 1 ? tw / 2 : 1;
        th = th > 1 ? th / 2 : 1;
        ++mip;
      }
      // Base offset of mip level (tightly packed chain).
      std::uint32_t base = 0;
      std::uint32_t bw = pyr_w;
      std::uint32_t bh = pyr_h;
      for (std::uint32_t m = 0; m < mip; ++m) {
        base += bw * bh;
        bw = bw > 1 ? bw / 2 : 1;
        bh = bh > 1 ? bh / 2 : 1;
      }
      // Tap origin at the rect MINIMUM corner (x0, not the center): at the
      // covering mip the rect spans up to one texel, and center-floor taps
      // miss texel floor(center)-1 when the center fraction is < 0.5 —
      // max_d from outside the footprint breaks the conservative contract
      // (visible instance culled). x1/y1 are exclusive bounds from
      // CullProjectSphere's clamp; center-tap equivalence is preserved for
      // 1-texel rects.
      const std::uint32_t tx = static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(x0) * tw) / fw64);
      const std::uint32_t ty = static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(y0) * th) / fh64);
      // 2x2 taps for watertightness (clamped to level extent). Init to
      // -inf (not -1): pyramid texels are max-depths that may legitimately
      // be negative (identity-MVP test scenes); seeding -1 would mask any
      // stored max below -1 and wrongly keep the instance.
      float max_d = -3.402823466e+38F;
      for (std::uint32_t oy = 0; oy < 2; ++oy) {
        for (std::uint32_t ox = 0; ox < 2; ++ox) {
          std::uint32_t sx = tx + ox;
          std::uint32_t sy = ty + oy;
          if (sx >= tw) {
            sx = tw - 1;
          }
          if (sy >= th) {
            sy = th - 1;
          }
          const float d = pyramid[base + sy * tw + sx];
          if (d > max_d) {
            max_d = d;
          }
        }
      }
      // Pyramid stores MAX depth (farther = larger NDC z); the sphere is
      // occluded when its closest depth is behind the stored max.
      occluded = closest > max_d;
    }
    if (occluded) {
      continue;
    }
    VkDrawIndexedIndirectCommand cmd{};
    const std::uint32_t mi = inst[id].mesh_index;
    if (meshes != nullptr && mi < mesh_count) {
      cmd.indexCount = meshes[mi].index_count;
      cmd.firstIndex = meshes[mi].first_index;
      cmd.vertexOffset = meshes[mi].vertex_offset;
    } else {
      cmd.indexCount = 36;  // unit-cube fallback (12 tris)
    }
    cmd.instanceCount = 1;
    cmd.firstInstance = id;
    draws_out[n++] = cmd;
  }
  return n;
}

GpuCull::~GpuCull() noexcept { shutdown(); }

auto GpuCull::init(std::uint32_t max_instances, const std::uint32_t* cull_spv,
                   std::size_t cull_spv_words, const std::uint32_t* hiz_spv,
                   std::size_t hiz_spv_words) noexcept -> Status {
  if (ready_) {
    return MakeOk();
  }
  if (max_instances == 0 || max_instances > (1u << 24)) {
    return MakeFail(StatusCode::kInvalidArgument, "bad max_instances");
  }
  if (cull_spv == nullptr || cull_spv_words == 0 || hiz_spv == nullptr ||
      hiz_spv_words == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "null shader blobs");
  }
  max_instances_ = max_instances;
  cull_spv_ = cull_spv;
  cull_words_ = cull_spv_words;
  hiz_spv_ = hiz_spv;
  hiz_words_ = hiz_spv_words;
  ready_ = true;
  return MakeOk();
}

auto GpuCull::shutdown() noexcept -> void {
  destroy_gpu();
  max_instances_ = 0;
  cull_spv_ = nullptr;
  cull_words_ = 0;
  hiz_spv_ = nullptr;
  hiz_words_ = 0;
  ready_ = false;
}

auto GpuCull::create_gpu(VkDevice device,
                         VkPipelineLayout bindless_layout) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)device;
  (void)bindless_layout;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "cull not init");
  }
  if (cull_pipeline_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "cull gpu exists");
  }
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "null device");
  }
  (void)bindless_layout;
  // M3-03 frame setup creates the real compute pipelines (shader module
  // from the .inc blobs + BDA push layout) next to the PBR pipeline, so
  // sampler/pool/queue ownership stays in one TU. Sentinel marks validated
  // sizing here.
  cull_pipeline_ = reinterpret_cast<VkPipeline>(static_cast<std::uintptr_t>(1));
  hiz_pipeline_ = reinterpret_cast<VkPipeline>(static_cast<std::uintptr_t>(2));
  indirect_buffer_ =
      reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(3));
  count_buffer_ = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(4));
  visible_buffer_ =
      reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(5));
  return MakeOk();
#endif
}

auto GpuCull::destroy_gpu() noexcept -> void {
  cull_pipeline_ = nullptr;
  hiz_pipeline_ = nullptr;
  indirect_buffer_ = nullptr;
  count_buffer_ = nullptr;
  visible_buffer_ = nullptr;
}

}  // namespace engine::renderer
