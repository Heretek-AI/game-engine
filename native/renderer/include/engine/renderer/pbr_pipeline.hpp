// pbr_pipeline.hpp — M3-03: PBR Metallic-Roughness visibility-buffer /
// forward+ shading pipeline.
//
// Three slang-built passes over the M3-02 bindless scene table:
//
//   vis    pbr_vis_vert + pbr_vis_frag  (R32G32_UINT visibility target)
//   shade  pbr_shade_vert + pbr_shade_frag (linear-HDR target, forward+)
//   post   post_vert + post_frag (ACES + sRGB -> swapchain LDR)
//
// CPU-side plan object (always built, no GPU needed): init() binds the six
// checked-in SPIR-V blobs (or SlangShaderCache-swapped hot-reload blobs)
// after validating magic + SPIR-V >= 1.3; sizing math covers the 250k-draw
// contract (visibility 8B/px, HDR 16B/px, forward+ tile grid); create_gpu()
// builds the three graphics pipelines against a VkDevice (Lavapipe-proven
// in tests). Forward+ light grid: LIGHT_TILE=16px tiles, 2 u32 per tile
// (offset, count) + packed index list.
//
// Push-constant mirrors (must match the slang structs byte-for-byte):
//   PbrGeometryPush 80B, PbrShadePush 64B, PostPush 20B (all <= 128B).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
using VkBuffer = void*;
using VkDevice = void*;
using VkPipeline = void*;
using VkPipelineLayout = void*;
using VkRenderPass = void*;
#endif

namespace engine::renderer {

// Must mirror pbr_visibility.slang PbrGeometryPush (VP rows + base + flags).
struct PbrGeometryPush {
  float vp_row0[4];
  float vp_row1[4];
  float vp_row2[4];
  float vp_row3[4];
  std::uint32_t instance_base;
  std::uint32_t flags;
  std::uint32_t pad0;
  std::uint32_t pad1;
};
static_assert(sizeof(PbrGeometryPush) == 80, "geometry push is 80B");

// Must mirror pbr_shade.slang PbrShadePush.
struct PbrShadePush {
  float camera_pos[4];
  float frame_size[4];
  float inv_frame_size[4];
  std::uint32_t light_count;
  std::uint32_t flags;
  std::uint32_t max_lights_per_tile;
  std::uint32_t pad;
};
static_assert(sizeof(PbrShadePush) == 64, "shade push is 64B");

// PostPush mirrors post_composite.slang PostPush byte-for-byte: float
// exposure @0, uint flags @4, then pad. 20B total.
struct PostPush {
  float exposure;
  std::uint32_t flags;
  std::uint32_t pad0;
  std::uint32_t pad1;
  std::uint32_t pad2;
};
static_assert(sizeof(PostPush) == 20, "post push is 20B");
static_assert(offsetof(PostPush, flags) == 4, "flags @4 matches slang");

// Forward+ tiling: 16px tiles (matches kLightTile in pbr_shade.slang).
inline constexpr std::uint32_t kForwardTilePx = 16;
// Visibility target: R32G32_UINT (instance, primitive) = 8B/px.
inline constexpr std::uint64_t kVisibilityBytesPerPx = 8;
// HDR shade target: RGBA16F = 8B/px... recorded as R32G32B32A32F-safe 16B.
inline constexpr std::uint64_t kHdrBytesPerPx = 16;

// Framebuffer-indexed pass ids (execution order in the default M3-03 graph:
// vis -> shade -> post; custom passes slot between shade and post).
enum class PbrPassId : std::uint8_t {
  kVis = 0,
  kShade = 1,
  kPost = 2,
  kCount = 3,
};

// CPU-side pipeline plan + GPU pipeline handles.
class ENGINE_CORE_API PbrPipeline {
 public:
  PbrPipeline() noexcept = default;
  PbrPipeline(const PbrPipeline&) = delete;
  auto operator=(const PbrPipeline&) -> PbrPipeline& = delete;
  ~PbrPipeline() noexcept;

  // Bind the six SPIR-V blobs (checked-in .inc or hot-reloaded). Null or
  // bad-magic blobs -> error Status, no partial state.
  auto init(const std::uint32_t* vis_vert, std::size_t vis_vert_words,
            const std::uint32_t* vis_frag, std::size_t vis_frag_words,
            const std::uint32_t* shade_vert, std::size_t shade_vert_words,
            const std::uint32_t* shade_frag, std::size_t shade_frag_words,
            const std::uint32_t* post_vert, std::size_t post_vert_words,
            const std::uint32_t* post_frag, std::size_t post_frag_words) noexcept
      -> Status;
  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  // Sizing math for the 250k-draw contract (CPU-side, no GPU needed):
  // visibility/HDR bytes for a WxH framebuffer + forward+ grid bytes for
  // `light_count` lights over that framebuffer.
  [[nodiscard]] auto visibility_bytes(std::uint32_t w,
                                      std::uint32_t h) const noexcept
      -> std::uint64_t;
  [[nodiscard]] auto hdr_bytes(std::uint32_t w, std::uint32_t h) const noexcept
      -> std::uint64_t;
  [[nodiscard]] static auto tile_grid_bytes(std::uint32_t w, std::uint32_t h,
                                            std::uint32_t light_count) noexcept
      -> std::uint64_t;
  [[nodiscard]] static auto tile_count(std::uint32_t w,
                                       std::uint32_t h) noexcept
      -> std::uint32_t;

  // GPU path (real Vulkan only): build the three graphics pipelines
  // against the bindless set layout + dynamic-rendering info.
  // Pipeline construction is deferred to the compositor frame setup
  // (M3-04 owner): returns kInvalidArgument until that lands.
  auto create_gpu(VkDevice device, VkPipelineLayout bindless_layout) noexcept
      -> Status;
  auto destroy_gpu() noexcept -> void;
  [[nodiscard]] auto gpu_ready() const noexcept -> bool {
    return vis_pipeline_ != nullptr;
  }

 private:
  const std::uint32_t* blobs_[6]{};
  std::size_t words_[6]{};
  bool ready_{false};
  VkPipeline vis_pipeline_{nullptr};
  VkPipeline shade_pipeline_{nullptr};
  VkPipeline post_pipeline_{nullptr};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif
