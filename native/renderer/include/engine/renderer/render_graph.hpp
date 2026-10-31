// render_graph.hpp — M3-01: declarative DAG render graph.
//
// A RenderGraph is a CPU-side plan: passes declare the image resources
// they read/write (with desired layouts and access), and `compile()`
// topologically sorts them, inserts VkImageMemoryBarrier2 transitions
// (synchronization2) between writers/readers, and computes transient
// attachment aliasing (interval-based virtual allocation) for
// non-persistent images.
//
// M3-01 scope is the CPU-side plan only (no VkDevice needed): M3-02/M3-03
// consume `baked Barriers()` / `record_barriers()` against real Vulkan
// command buffers. All methods are noexcept; fallible operations report
// via Status (hot flags forbid exceptions).
//
// Example:
//
//   RenderGraph graph;
//   graph.add_image({.name="color", .format=VK_FORMAT_R8G8B8A8_SRGB,
//                    .width=1280, .height=720, .depth=1, .transient=true});
//   const PassAccess writes[] = {
//       {.image="color", .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
//   graph.add_pass("main", {}, writes);
//   if (!graph.compile().ok()) { /* report */ }
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/engine_core_export.hpp"
// MSVC C4251 (std::vector members in an exported class) is benign here:
// all members are private and both sides build with the same toolchain.
// Pushed once per header so the class declaration below never warns,
// regardless of where editors insert members.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

// Minimal Vulkan stand-ins so the planner builds WITHOUT the Vulkan SDK.
// When ENGINE_RENDERER_HAS_VULKAN is defined (real SDK on the include
// path), <vulkan/vulkan.h> provides the real types instead.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
using VkFormat = std::uint32_t;
using VkImageLayout = std::uint32_t;
using VkAccessFlags2 = std::uint64_t;
using VkPipelineStageFlags2 = std::uint64_t;
inline constexpr VkFormat VK_FORMAT_UNDEFINED = 0;
inline constexpr VkFormat VK_FORMAT_R8G8B8A8_UNORM = 37;
inline constexpr VkFormat VK_FORMAT_B8G8R8A8_UNORM = 44;
inline constexpr VkFormat VK_FORMAT_R8G8B8A8_SRGB = 43;
// P4-02: HDR scene target (shade pass output, post pass input):
// R16G16B16A16_SFLOAT, Vulkan numeric value 91 (Unspecified extension,
// core since 1.0 — matches <vulkan/vulkan.h> on SDK builds).
inline constexpr VkFormat VK_FORMAT_R16G16B16A16_SFLOAT = 91;
inline constexpr VkFormat VK_FORMAT_D32_SFLOAT = 126;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_UNDEFINED = 0;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_GENERAL = 1;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6;
inline constexpr VkImageLayout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
#endif

namespace engine::renderer {

// Limits keep all graph storage inline-friendly and ASan-clean under
// -fno-exceptions (no unbounded growth on the frame path).
inline constexpr std::size_t kMaxGraphPasses = 64;
inline constexpr std::size_t kMaxGraphImages = 64;
inline constexpr std::size_t kMaxPassAccesses = 16;
// Interned-name pool cap: 64 passes x 16 accesses x (read+write) = 2048
// worst case; 4096 slots x 64B = 256KiB per graph, reserved up front.
inline constexpr std::size_t kStringPoolSlots = 4096;

// A named 2D/3D image resource in the graph.
struct GraphImageDesc {
  const char* name{nullptr};  // required, unique
  VkFormat format{VK_FORMAT_UNDEFINED};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t depth{1};
  // transient=true: owned by the graph, aliasable with other transient
  // images whose lifetimes don't overlap (memory aliasing). false: e.g.
  // swapchain images — never aliased, no backing allocation planned.
  bool transient{true};
};

// How a pass uses one image.
struct PassAccess {
  const char* image{nullptr};  // GraphImageDesc::name
  VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};  // desired layout in pass
  VkAccessFlags2 access{0};    // access mask while bound in pass
  VkPipelineStageFlags2 stage{0};  // producer/consumer stage mask
};

// One DAG node is added via add_pass(name, reads, writes, order_after):
// `reads`/`writes` reference image names; `order_after` adds explicit
// edges beyond read/write hazards.

// A computed layout transition between two consecutive users of an image.
struct GraphBarrier {
  std::uint32_t src_pass{kInvalidPass};  // kInvalidPass = external (before first)
  std::uint32_t dst_pass{kInvalidPass};  // kInvalidPass = external (after last)
  std::uint32_t image{0};                // index into images()
  VkImageLayout old_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  VkImageLayout new_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  VkAccessFlags2 src_access{0};
  VkAccessFlags2 dst_access{0};
  VkPipelineStageFlags2 src_stage{0};
  VkPipelineStageFlags2 dst_stage{0};

  static constexpr std::uint32_t kInvalidPass = 0xFFFFFFFFu;
};

// Backing-store plan for one transient image (byte offsets into a single
// virtual transient heap; real VkImage/VMA allocation is M3-02's job).
struct TransientAlloc {
  std::uint32_t image{0};  // index into images()
  std::uint64_t offset{0};
  std::uint64_t size{0};
};

template <typename T>
auto span_or_empty(const std::vector<T>& v) noexcept
    -> std::span<const T> {
  if (v.empty()) {
    static constexpr T kPlaceholder{};
    return {&kPlaceholder, std::size_t{0}};
  }
  return {v.data(), v.size()};
}

class ENGINE_CORE_API RenderGraph {
 public:
  RenderGraph() noexcept { string_pool_.reserve(kStringPoolSlots); }
  RenderGraph(const RenderGraph&) = delete;
  auto operator=(const RenderGraph&) -> RenderGraph& = delete;
  ~RenderGraph() noexcept = default;

  // Add resources/passes in any order. Names are copied into owned
  // storage; caller string_views only need to live for the call.
  // Duplicate names or over-capacity -> Status (no exceptions).
  auto add_image(const GraphImageDesc& desc) noexcept -> Status;
  auto add_pass(std::string_view name, std::span<const PassAccess> reads,
                std::span<const PassAccess> writes,
                std::span<const char* const> order_after = {}) noexcept -> Status;

  // Validate + topologically sort. Edges come from write->read,
  // write->write, read->write hazards on the same image plus explicit
  // order_after. Cycle -> kGraphCycle; unknown name -> kUnknownResource.
  // Idempotent: re-compiles from scratch.
  auto compile() noexcept -> Status;

  [[nodiscard]] auto compiled() const noexcept -> bool { return compiled_; }
  // Add-order pass count (valid before compile). Compiled order lives
  // in execution_order() (valid after compile); both index the same
  // pass-name table.
  [[nodiscard]] auto pass_count() const noexcept -> std::size_t {
    return passes_.size();
  }
  [[nodiscard]] auto barrier_count() const noexcept -> std::size_t {
    return barriers_.size();
  }
  [[nodiscard]] auto transient_count() const noexcept -> std::size_t {
    return transients_.size();
  }
  [[nodiscard]] auto transient_heap_bytes() const noexcept -> std::uint64_t {
    return transient_heap_bytes_;
  }

  // Execution order (indices into pass_names()); valid after compile().
  // NOTE: std::span{ptr, 0} from data()==nullptr is UB (span requires
  // [ptr, ptr+n) to be a valid range even when n==0 for the sized
  // constructor... in practice data() on empty vectors may be null), so
  // guard empties with a static placeholder.
  [[nodiscard]] auto execution_order() const noexcept
      -> std::span<const std::uint32_t> {
    return span_or_empty(order_);
  }
  [[nodiscard]] auto barriers() const noexcept
      -> std::span<const GraphBarrier> {
    return span_or_empty(barriers_);
  }
  [[nodiscard]] auto transients() const noexcept
      -> std::span<const TransientAlloc> {
    return span_or_empty(transients_);
  }

  // Name tables (parallel to add order).
  [[nodiscard]] auto pass_name(std::uint32_t i) const noexcept -> const char*;
  [[nodiscard]] auto image_name(std::uint32_t i) const noexcept -> const char*;
  [[nodiscard]] auto image_count() const noexcept -> std::size_t {
    return images_.size();
  }

 private:
  struct ImageRec {
    std::array<char, 64> name{};
    GraphImageDesc desc{};
  };
  struct PassRec {
    std::array<char, 64> name{};
    std::vector<PassAccess> reads;
    std::vector<PassAccess> writes;
    std::vector<std::uint32_t> after;  // resolved pass indices
    struct OrderDep {
      std::array<char, 64> name{};
      std::uint32_t index{0};
    };
    std::vector<OrderDep> after_names;
  };

  [[nodiscard]] auto find_image(std::string_view n) const noexcept -> int;
  [[nodiscard]] auto find_pass(std::string_view n) const noexcept -> int;
  // Intern caller-owned name strings into the graph-owned pool (see
  // .cpp): returns null on empty/oversize/pool-exhausted.
  auto intern_string(const char* s) noexcept -> char*;

  std::vector<ImageRec> images_;
  std::vector<PassRec> passes_;
  // Graph-owned string pool for interned access names (see intern_string).
  // Stable addresses: reserved up-front (never reallocated after fill —
  // vector growth WOULD invalidate stored char*; reserve the max).
  std::vector<std::array<char, 64>> string_pool_;
  std::vector<std::uint32_t> order_;
  std::vector<GraphBarrier> barriers_;
  std::vector<TransientAlloc> transients_;
  std::uint64_t transient_heap_bytes_{0};
  bool compiled_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif
