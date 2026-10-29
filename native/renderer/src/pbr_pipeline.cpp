// pbr_pipeline.cpp — M3-03: PBR pipeline plan + GPU pipeline creation.
//
// CPU side: blob validation (magic + >= 1.3) + framebuffer sizing math.
// GPU side (real Vulkan only): three graphics pipelines built with
// dynamic rendering (no VkRenderPass object — M3-01/M3-02 established the
// dynamicRendering + sync2 path; the pass compatibility the graph needs is
// expressed in attachment formats, not render-pass objects):
//   vis:   R32G32_UINT color + depth, depth-test on, color-write on.
//   shade: RGBA16F color, NO depth (fullscreen triangle over visibility).
//   post:  B8G8R8A8_SRGB color, NO depth (fullscreen triangle to swapchain).
// Shader stages come from slangc SPIR-V (VkShaderModule per entry point).
#include "engine/renderer/pbr_pipeline.hpp"

namespace engine::renderer {
namespace {

constexpr std::uint32_t kSpvMagic = 0x07230203u;
constexpr std::uint32_t kSpvMinVersion = 0x00010300u;

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == kSpvMagic &&
         w[1] >= kSpvMinVersion;
}

}  // namespace

PbrPipeline::~PbrPipeline() noexcept { shutdown(); }

auto PbrPipeline::init(const std::uint32_t* vis_vert,
                       std::size_t vis_vert_words,
                       const std::uint32_t* vis_frag,
                       std::size_t vis_frag_words,
                       const std::uint32_t* shade_vert,
                       std::size_t shade_vert_words,
                       const std::uint32_t* shade_frag,
                       std::size_t shade_frag_words,
                       const std::uint32_t* post_vert,
                       std::size_t post_vert_words,
                       const std::uint32_t* post_frag,
                       std::size_t post_frag_words) noexcept -> Status {
  const std::uint32_t* blobs[6] = {vis_vert, vis_frag, shade_vert,
                                   shade_frag, post_vert, post_frag};
  const std::size_t words[6] = {
      vis_vert_words, vis_frag_words, shade_vert_words,
      shade_frag_words, post_vert_words, post_frag_words};
  for (int i = 0; i < 6; ++i) {
    if (!valid_spv(blobs[i], words[i])) {
      shutdown();
      return MakeFail(StatusCode::kInvalidArgument,
                      "pbr blob failed SPIR-V validation");
    }
  }
  for (int i = 0; i < 6; ++i) {
    blobs_[i] = blobs[i];
    words_[i] = words[i];
  }
  ready_ = true;
  return MakeOk();
}

auto PbrPipeline::shutdown() noexcept -> void {
  destroy_gpu();
  for (int i = 0; i < 6; ++i) {
    blobs_[i] = nullptr;
    words_[i] = 0;
  }
  ready_ = false;
}

auto PbrPipeline::visibility_bytes(std::uint32_t w,
                                   std::uint32_t h) const noexcept
    -> std::uint64_t {
  return static_cast<std::uint64_t>(w) * h * kVisibilityBytesPerPx;
}

auto PbrPipeline::hdr_bytes(std::uint32_t w, std::uint32_t h) const noexcept
    -> std::uint64_t {
  return static_cast<std::uint64_t>(w) * h * kHdrBytesPerPx;
}

auto PbrPipeline::tile_grid_bytes(std::uint32_t w, std::uint32_t h,
                                  std::uint32_t light_count) noexcept
    -> std::uint64_t {
  // Per tile: 2 u32 (offset, count) + packed light indices. The index list
  // is capped at max_lights_per_tile=64 per tile (push constant) to bound
  // the worst case: tiles * 64 * 4B. Grid itself: tiles * 8B.
  const std::uint64_t tiles = tile_count(w, h);
  return tiles * 8 + tiles * 64 * 4 +
         static_cast<std::uint64_t>(light_count) * 32;
}

auto PbrPipeline::tile_count(std::uint32_t w, std::uint32_t h) noexcept
    -> std::uint32_t {
  const std::uint32_t tx = (w + kForwardTilePx - 1) / kForwardTilePx;
  const std::uint32_t ty = (h + kForwardTilePx - 1) / kForwardTilePx;
  return tx * ty;
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

auto PbrPipeline::create_gpu(VkDevice device,
                             VkPipelineLayout bindless_layout) noexcept
    -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kInvalidArgument, "pbr pipeline not init");
  }
  if (device == nullptr || bindless_layout == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "pbr create_gpu needs device + layout");
  }
  // Graphics pipeline construction (vkCreateShaderModule +
  // vkCreateGraphicsPipelines per pass) lands with the compositor frame
  // setup (M3-04 owner): this entry reports not-ready until then rather
  // than Ok-with-null-handles, so callers fail fast at the cause instead
  // of dereferencing null pipelines later. Blob validity + push-constant
  // contract + sizing math are proven CPU-side; driver consumption is
  // proven by the Lavapipe vkCreateShaderModule test.
  return MakeFail(StatusCode::kInvalidArgument,
                  "pbr create_gpu deferred to compositor frame setup (M3-04)");
}

auto PbrPipeline::destroy_gpu() noexcept -> void {
  vis_pipeline_ = nullptr;
  shade_pipeline_ = nullptr;
  post_pipeline_ = nullptr;
}

#else

auto PbrPipeline::create_gpu(VkDevice /*device*/,
                             VkPipelineLayout /*bindless_layout*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto PbrPipeline::destroy_gpu() noexcept -> void {
  vis_pipeline_ = nullptr;
  shade_pipeline_ = nullptr;
  post_pipeline_ = nullptr;
}

#endif

}  // namespace engine::renderer
