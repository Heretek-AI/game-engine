// compositor_bridge.cpp — M3-03: custom-pass registration + frame walk.
#include "engine/renderer/compositor_bridge.hpp"

#include <cstring>
#include <string_view>

namespace engine::renderer {

auto Compositor::add_custom_pass(const CustomPassDesc& desc) noexcept
    -> Status {
  if (customs_.size() >= kMaxCustomPasses) {
    return MakeFail(StatusCode::kOutOfMemory, "too many custom passes");
  }
  if (desc.name == nullptr || desc.name[0] == '\0') {
    return MakeFail(StatusCode::kInvalidArgument,
                    "custom pass name required");
  }
  if (desc.callback == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "custom pass callback required");
  }
  if (desc.read_count > kMaxPassAccesses ||
      desc.write_count > kMaxPassAccesses) {
    return MakeFail(StatusCode::kInvalidArgument, "too many pass accesses");
  }
  std::string_view want(desc.name);
  for (const auto& c : customs_) {
    if (std::string_view(c.name.data()) == want) {
      return MakeFail(StatusCode::kInvalidArgument,
                      "duplicate custom pass name");
    }
  }
  CustomRec rec{};
  std::size_t n = 0;
  while (n + 1 < rec.name.size() && desc.name[n] != '\0') {
    rec.name[n] = desc.name[n];
    ++n;
  }
  if (desc.name[n] != '\0') {
    return MakeFail(StatusCode::kInvalidArgument,
                    "custom pass name too long");
  }
  rec.callback = desc.callback;
  rec.user_data = desc.user_data;
  // Graph node: copy bounded spans into the graph's add_pass shape.
  std::vector<PassAccess> reads_vec(desc.reads,
                                    desc.reads + desc.read_count);
  std::vector<PassAccess> writes_vec(desc.writes,
                                     desc.writes + desc.write_count);
  std::span<const PassAccess> reads(reads_vec.data(), reads_vec.size());
  std::span<const PassAccess> writes(writes_vec.data(), writes_vec.size());
  auto st = graph_.add_pass(want, reads, writes);
  if (!st.ok()) {
    return st;
  }
  // Add-order index; compile() below re-resolves it to the compiled
  // execution slot (execution_order holds add indices in run order).
  rec.graph_pass =
      static_cast<std::uint32_t>(graph_.pass_count() - 1);
  rec.add_index = rec.graph_pass;
  customs_.push_back(rec);
  compiled_ = false;
  return MakeOk();
}

auto Compositor::compile() noexcept -> Status {
  auto st = graph_.compile();
  if (!st.ok()) {
    return st;
  }
  // Re-resolve each custom's add index to its compiled execution slot so
  // run_frame() fires callbacks in true graph order (hazard edges may
  // reorder adds).
  for (auto& c : customs_) {
    c.graph_pass = c.add_index;  // default: same when compile kept order
  }
  for (std::uint32_t slot = 0; slot < graph_.execution_order().size();
       ++slot) {
    const std::uint32_t add_idx = graph_.execution_order()[slot];
    for (auto& c : customs_) {
      if (c.add_index == add_idx) {
        c.graph_pass = slot;
      }
    }
  }
  for (auto& c : customs_) {
    c.last_result = 0;
  }
  failed_ = 0;
  compiled_ = true;
  return MakeOk();
}

auto Compositor::run_frame() noexcept -> Status {
  if (!compiled_) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "compositor not compiled");
  }
  failed_ = 0;
  // Walk COMPILED order; invoke customs when their graph pass arrives.
  // Linear scan per custom (<= 32 customs x 64 passes — trivially cheap,
  // and hot-flags forbid associative containers here).
  std::uint32_t slot = 0;
  for (const std::uint32_t add_idx : graph_.execution_order()) {
    for (auto& c : customs_) {
      if (c.add_index == add_idx) {
        int32_t rc = 0;
        // Fault isolation boundary: a hostile callback returning nonzero
        // marks ITS pass failed; the frame continues. (A crashing
        // callback is out of scope — same rule as managed systems.)
        rc = c.callback(add_idx, c.user_data);
        c.last_result = rc;
        if (rc != 0) {
          ++failed_;
        }
      }
    }
    ++slot;
  }
  (void)slot;
  return MakeOk();
}

auto Compositor::pass_result(std::size_t i) const noexcept -> int32_t {
  if (i >= customs_.size()) {
    return -1;
  }
  return customs_[i].last_result;
}

}  // namespace engine::renderer
