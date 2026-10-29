// compositor_bridge.hpp — M3-03: C#-driven custom passes over RenderGraph.
//
// A Compositor owns a RenderGraph plus an ordered list of custom passes
// registered from managed code (GraphicsCompositor.AddPass). Built-in PBR
// passes (vis/shade/post) compile to fixed graph nodes; each custom pass
// declares its image accesses + one C callback invoked at record time.
//
// Frame flow (CPU-side, no GPU needed for plan + callback order):
//   begin_frame(graph images...) -> for each compiled pass in order:
//     built-in -> record barrier plan (baked Barriers()); custom ->
//     invoke its callback with the pass index. end_frame() validates the
//     full sequence ran. GPU command recording for custom passes is M3-04's
//     consumer of the callback token (this card proves registration,
//     ordering, fault isolation, and ABI shape).
//
// Fault isolation: a custom callback that returns nonzero is recorded as
// failed; the compositor continues with the remaining passes (one bad
// post pass must not kill the frame) and reports the failure count.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/render_graph.hpp"

namespace engine::renderer {

// Custom pass callback (C ABI): invoked once per frame when the pass's
// turn arrives in compiled order. `pass_index` is the compiled slot;
// `user_data` round-trips the registration context. Return 0 on success;
// nonzero marks the pass failed (isolated, frame continues).
using CompositorPassCallback = int32_t (*)(std::uint32_t pass_index,
                                           void* user_data);

inline constexpr std::size_t kMaxCustomPasses = 32;

// One registered custom pass: graph node + callback.
struct CustomPassDesc {
  const char* name{nullptr};
  PassAccess reads[kMaxPassAccesses]{};
  std::size_t read_count{0};
  PassAccess writes[kMaxPassAccesses]{};
  std::size_t write_count{0};
  CompositorPassCallback callback{nullptr};
  void* user_data{nullptr};
};

class ENGINE_CORE_API Compositor {
 public:
  Compositor() noexcept = default;
  Compositor(const Compositor&) = delete;
  auto operator=(const Compositor&) -> Compositor& = delete;
  ~Compositor() noexcept = default;

  // Access the owned graph (add images + built-in passes before compile).
  auto graph() noexcept -> RenderGraph& { return graph_; }

  // Register a custom pass: adds the graph node AND stores the callback.
  // Over-capacity / duplicate / null-callback -> error Status.
  auto add_custom_pass(const CustomPassDesc& desc) noexcept -> Status;

  // Compile the graph (built-ins + customs). Must succeed before run.
  auto compile() noexcept -> Status;
  [[nodiscard]] auto compiled() const noexcept -> bool {
    return compiled_;
  }

  // Run one frame: walk compiled order, invoke each custom callback once.
  // Returns ok; per-pass results land in pass_results() (0 = ok / callback
  // rc / -1 skipped-uncompiled). Failed customs don't stop the frame.
  auto run_frame() noexcept -> Status;
  [[nodiscard]] auto custom_count() const noexcept -> std::size_t {
    return customs_.size();
  }
  [[nodiscard]] auto failed_count() const noexcept -> std::size_t {
    return failed_;
  }
  // Per-custom-pass last-frame result, parallel to registration order.
  [[nodiscard]] auto pass_result(std::size_t i) const noexcept -> int32_t;

 private:
  struct CustomRec {
    std::array<char, 64> name{};
    CompositorPassCallback callback{nullptr};
    void* user_data{nullptr};
    std::uint32_t graph_pass{0};  // compiled execution slot (see compile)
    std::uint32_t add_index{0};   // add-order index in the graph
    int32_t last_result{0};
  };
  RenderGraph graph_;
  std::vector<CustomRec> customs_;
  bool compiled_{false};
  std::size_t failed_{0};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif
