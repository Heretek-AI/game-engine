// render_graph.cpp — M3-01: RenderGraph implementation (CPU-side only).
#include "engine/renderer/render_graph.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace engine::renderer {
namespace {

auto valid_image_ref(const char* src) noexcept -> bool;

// Bounded C-string check: true iff [src, src+64) contains a NUL (i.e. the
// name fits the slot). Never scans past 64B — hostile non-terminated
// pointers fail instead of over-reading.
auto valid_image_ref(const char* src) noexcept -> bool {
  if (src == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < 64; ++i) {
    if (src[i] == '\0') {
      return i > 0;
    }
  }
  return false;
}

auto copy_name(std::array<char, 64>& dst, const char* src) noexcept -> bool {
  if (src == nullptr || src[0] == '\0') {
    return false;
  }
  std::size_t n = std::strlen(src);
  if (n >= dst.size()) {
    return false;
  }
  std::memcpy(dst.data(), src, n + 1);
  return true;
}

auto view_of(const std::array<char, 64>& a) noexcept -> std::string_view {
  // Bounded scan: never read past the 64B slot even if the name fill
  // missed the NUL (defensive; copy_name always terminates, but the
  // graph must stay ASan-clean under hostile inputs too).
  std::size_t n = 0;
  while (n < a.size() && a[n] != '\0') {
    ++n;
  }
  return {a.data(), n};
}

}  // namespace

auto RenderGraph::find_image(std::string_view n) const noexcept -> int {
  for (std::size_t i = 0; i < images_.size(); ++i) {
    if (view_of(images_[i].name) == n) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

auto RenderGraph::find_pass(std::string_view n) const noexcept -> int {
  for (std::size_t i = 0; i < passes_.size(); ++i) {
    if (view_of(passes_[i].name) == n) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Interned string pool: access names reference caller memory (often stack
// arrays like `PassAccess w[2]` inside a test function). The graph must
// OWN those strings — compile() runs after the caller's frame returns.
// Pool: fixed 1024 x 64B slots, bump-allocated, never freed until the
// graph dies (frame-graph usage: rebuilt per scene, not per frame).
auto RenderGraph::intern_string(const char* s) noexcept -> char* {
  if (s == nullptr || s[0] == '\0') {
    return nullptr;
  }
  std::size_t n = 0;
  while (n < 64 && s[n] != '\0') {
    ++n;
  }
  if (n == 0 || n >= 64) {
    return nullptr;  // empty or unterminated/too long
  }
  if (string_pool_.size() >= kStringPoolSlots) {
    return nullptr;
  }
  std::array<char, 64> slot{};
  for (std::size_t i = 0; i <= n; ++i) {
    slot[i] = s[i];  // bounded copy incl. NUL; never calls strlen
  }
  string_pool_.push_back(slot);
  return string_pool_.back().data();
}

auto RenderGraph::add_image(const GraphImageDesc& desc) noexcept -> Status {
  if (desc.name == nullptr || desc.name[0] == '\0') {
    return MakeFail(StatusCode::kInvalidArgument, "image name required");
  }
  if (desc.width == 0 || desc.height == 0 || desc.depth == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "image extent must be non-zero");
  }
  if (images_.size() >= kMaxGraphImages) {
    return MakeFail(StatusCode::kOutOfMemory, "too many graph images");
  }
  if (find_image(desc.name) >= 0) {
    return MakeFail(StatusCode::kInvalidArgument, "duplicate image name");
  }
  ImageRec rec{};
  if (!copy_name(rec.name, desc.name)) {
    return MakeFail(StatusCode::kInvalidArgument, "image name too long");
  }
  rec.desc = desc;
  rec.desc.name = rec.name.data();
  images_.push_back(rec);
  compiled_ = false;
  return MakeOk();
}

auto RenderGraph::add_pass(std::string_view name,
                           std::span<const PassAccess> reads,
                           std::span<const PassAccess> writes,
                           std::span<const char* const> order_after) noexcept
    -> Status {
  if (name.empty() || name.size() >= 64) {
    return MakeFail(StatusCode::kInvalidArgument, "pass name required (<64)");
  }
  if (passes_.size() >= kMaxGraphPasses) {
    return MakeFail(StatusCode::kOutOfMemory, "too many graph passes");
  }
  if (find_pass(name) >= 0) {
    return MakeFail(StatusCode::kInvalidArgument, "duplicate pass name");
  }
  if (reads.size() > kMaxPassAccesses || writes.size() > kMaxPassAccesses) {
    return MakeFail(StatusCode::kInvalidArgument, "too many pass accesses");
  }
  for (const auto& a : reads) {
    if (!valid_image_ref(a.image)) {
      return MakeFail(StatusCode::kInvalidArgument, "read image name required");
    }
  }
  for (const auto& a : writes) {
    if (!valid_image_ref(a.image)) {
      return MakeFail(StatusCode::kInvalidArgument, "write image name required");
    }
  }
  PassRec rec{};
  if (name.empty() || name.size() >= rec.name.size()) {
    return MakeFail(StatusCode::kInvalidArgument, "pass name too long");
  }
  for (std::size_t i = 0; i <= name.size(); ++i) {
    rec.name[i] = name[i];  // bounded copy incl. NUL; no std::string (hot TU)
  }
  rec.reads.reserve(reads.size());
  for (const auto& a : reads) {
    PassAccess owned = a;  // copy the POINTER struct first
    if (owned.image != nullptr) {
      // Intern the image-name string: pass-local storage owns the copy so
      // the caller's buffer (often a stack array) may die after the call.
      // Fixed 64B slots keep the frame path allocation-free.
      char* slot = intern_string(owned.image);
      if (slot == nullptr) {
        return MakeFail(StatusCode::kOutOfMemory, "string pool exhausted");
      }
      owned.image = slot;
    }
    rec.reads.push_back(owned);
  }
  rec.writes.reserve(writes.size());
  for (const auto& a : writes) {
    PassAccess owned = a;
    if (owned.image != nullptr) {
      char* slot = intern_string(owned.image);
      if (slot == nullptr) {
        return MakeFail(StatusCode::kOutOfMemory, "string pool exhausted");
      }
      owned.image = slot;
    }
    rec.writes.push_back(owned);
  }
  for (const char* dep : order_after) {
    if (dep == nullptr || dep[0] == '\0') {
      return MakeFail(StatusCode::kInvalidArgument, "order_after name required");
    }
    PassRec::OrderDep od{};
    if (!copy_name(od.name, dep)) {
      return MakeFail(StatusCode::kInvalidArgument, "order_after name too long");
    }
    rec.after_names.push_back(od);
  }
  passes_.push_back(std::move(rec));
  compiled_ = false;
  return MakeOk();
}

auto RenderGraph::pass_name(std::uint32_t i) const noexcept -> const char* {
  if (i >= passes_.size()) {
    return "";
  }
  return passes_[i].name.data();
}

auto RenderGraph::image_name(std::uint32_t i) const noexcept -> const char* {
  if (i >= images_.size()) {
    return "";
  }
  return images_[i].name.data();
}

auto RenderGraph::compile() noexcept -> Status {
  order_.clear();
  barriers_.clear();
  transients_.clear();
  transient_heap_bytes_ = 0;
  compiled_ = false;

  const std::size_t n = passes_.size();
  if (n == 0) {
    compiled_ = true;
    return MakeOk();
  }
  if (n > kMaxGraphPasses) {
    return MakeFail(StatusCode::kOutOfMemory, "too many graph passes");
  }

  // Resolve order_after names -> indices; validate access image names.
  for (std::size_t p = 0; p < n; ++p) {
    passes_[p].after.clear();
    for (auto& dep : passes_[p].after_names) {
      int f = find_pass(view_of(dep.name));
      if (f < 0) {
        return MakeFail(StatusCode::kUnknownResource, "order_after unknown pass");
      }
      dep.index = static_cast<std::uint32_t>(f);
      passes_[p].after.push_back(dep.index);
    }
    for (const auto& a : passes_[p].reads) {
      if (!valid_image_ref(a.image) || find_image(a.image) < 0) {
        return MakeFail(StatusCode::kUnknownResource, "read of unknown image");
      }
    }
    for (const auto& a : passes_[p].writes) {
      if (!valid_image_ref(a.image) || find_image(a.image) < 0) {
        return MakeFail(StatusCode::kUnknownResource, "write of unknown image");
      }
    }
  }

  // Build adjacency: hazard edges + explicit order_after edges.
  // adj[u] lists v with edge u -> v (u must run first).
  std::vector<std::vector<std::uint32_t>> adj(n);
  auto add_edge = [&](std::uint32_t u, std::uint32_t v) {
    if (u == v) {
      return;
    }
    auto& vec = adj[u];
    if (std::find(vec.begin(), vec.end(), v) == vec.end()) {
      vec.push_back(v);
    }
  };

  // Per image, chain accesses in PASS ADD ORDER: every consecutive pair
  // that is not read->read becomes an edge (RAW/WAR/WAW hazard). Same
  // image written twice without a reader still serializes (WAW).
  for (std::size_t img = 0; img < images_.size(); ++img) {
    std::string_view iname = view_of(images_[img].name);
    struct Use {
      std::uint32_t pass;
      bool is_write;
    };
    std::vector<Use> uses;
    for (std::size_t p = 0; p < n; ++p) {
      bool r = false;
      bool w = false;
      for (const auto& a : passes_[p].reads) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          r = true;
        }
      }
      for (const auto& a : passes_[p].writes) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          w = true;
        }
      }
      if (w) {
        uses.push_back({static_cast<std::uint32_t>(p), true});
      } else if (r) {
        uses.push_back({static_cast<std::uint32_t>(p), false});
      }
    }
    for (std::size_t k = 1; k < uses.size(); ++k) {
      if (!uses[k - 1].is_write && !uses[k].is_write) {
        continue;  // read->read: no hazard
      }
      add_edge(uses[k - 1].pass, uses[k].pass);
    }
  }
  for (std::size_t p = 0; p < n; ++p) {
    for (std::uint32_t u : passes_[p].after) {
      add_edge(u, static_cast<std::uint32_t>(p));
    }
  }

  // Kahn's algorithm, deterministic: always pick the smallest ready index.
  std::vector<std::size_t> indeg(n, 0);
  for (std::size_t u = 0; u < n; ++u) {
    for (std::uint32_t v : adj[u]) {
      indeg[v] += 1;
    }
  }
  std::vector<std::uint32_t> ready;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (indeg[i] == 0) {
      ready.push_back(i);
    }
  }
  order_.reserve(n);
  while (!ready.empty()) {
    auto it = std::min_element(ready.begin(), ready.end());
    std::uint32_t u = *it;
    ready.erase(it);
    order_.push_back(u);
    for (std::uint32_t v : adj[u]) {
      if (indeg[v] > 0 && --indeg[v] == 0) {
        ready.push_back(v);
      }
    }
  }
  if (order_.size() != n) {
    order_.clear();
    return MakeFail(StatusCode::kGraphCycle, "render graph has a cycle");
  }

  // position[pass] = execution slot.
  std::vector<std::size_t> pos(n, 0);
  for (std::size_t slot = 0; slot < n; ++slot) {
    pos[order_[slot]] = slot;
  }

  // Barriers: for each image, walk users in EXECUTION order; consecutive
  // users with different (layout, access, stage) need a transition.
  // First use transitions from UNDEFINED (unless it declares UNDEFINED);
  // last use also emits an "external" release barrier when its layout is
  // a presentable/readback layout (PRESENT/SHADER_READ/TRANSFER_SRC) so
  // M3-02 knows the image leaves the graph in that state.
  for (std::size_t img = 0; img < images_.size(); ++img) {
    std::string_view iname = view_of(images_[img].name);
    struct Use {
      std::uint32_t pass;
      PassAccess acc;
      bool is_write;
    };
    std::vector<Use> uses;
    for (std::uint32_t p : order_) {
      const PassRec& pr = passes_[p];
      // A pass may both read AND write the same image (read-modify-write,
      // e.g. lighting reads color then writes it): record BOTH uses so
      // the read->write transition inside the pass is not lost.
      for (const auto& a : pr.reads) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          uses.push_back({p, a, false});
        }
      }
      for (const auto& a : pr.writes) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          uses.push_back({p, a, true});
        }
      }
    }
    for (std::size_t k = 0; k < uses.size(); ++k) {
      const Use& u = uses[k];
      GraphBarrier b{};
      b.image = static_cast<std::uint32_t>(img);
      b.dst_pass = u.pass;
      b.new_layout = u.acc.layout;
      b.dst_access = u.acc.access;
      b.dst_stage = u.acc.stage;
      if (k == 0) {
        b.src_pass = GraphBarrier::kInvalidPass;
        b.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      } else {
        const Use& prev = uses[k - 1];
        if (prev.acc.layout == u.acc.layout &&
            prev.acc.access == u.acc.access &&
            prev.acc.stage == u.acc.stage) {
          continue;  // same state: no barrier
        }
        b.src_pass = prev.pass;
        b.old_layout = prev.acc.layout;
        b.src_access = prev.acc.access;
        b.src_stage = prev.acc.stage;
      }
      if (k == 0 && u.acc.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        continue;  // already undefined: nothing to transition from
      }
      barriers_.push_back(b);
    }
    if (!uses.empty()) {
      const Use& last = uses.back();
      if (last.acc.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
          last.acc.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
          last.acc.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        GraphBarrier rel{};
        rel.image = static_cast<std::uint32_t>(img);
        rel.src_pass = last.pass;
        rel.dst_pass = GraphBarrier::kInvalidPass;
        rel.old_layout = last.acc.layout;
        rel.new_layout = last.acc.layout;
        rel.src_access = last.acc.access;
        rel.src_stage = last.acc.stage;
        barriers_.push_back(rel);
      }
    }
  }

  // Transient aliasing: first/last execution slot per image; two
  // transient images may share bytes iff lifetimes DON'T overlap.
  // First-fit over a virtual heap; size = w*h*d*4B (M3-01 estimate, real
  // VkImage sizes come from the driver in M3-02).
  struct Life {
    std::uint32_t img;
    std::size_t first;
    std::size_t last;
    std::uint64_t size;
  };
  std::vector<Life> lives;
  for (std::size_t img = 0; img < images_.size(); ++img) {
    if (!images_[img].desc.transient) {
      continue;
    }
    std::string_view iname = view_of(images_[img].name);
    std::size_t first = n;
    std::size_t last = 0;
    bool used = false;
    for (std::size_t slot = 0; slot < n; ++slot) {
      const PassRec& pr = passes_[order_[slot]];
      bool hit = false;
      for (const auto& a : pr.reads) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          hit = true;
        }
      }
      for (const auto& a : pr.writes) {
        if (a.image != nullptr && std::string_view(a.image) == iname) {
          hit = true;
        }
      }
      if (hit) {
        used = true;
        if (slot < first) {
          first = slot;
        }
        if (slot > last) {
          last = slot;
        }
      }
    }
    if (!used) {
      continue;
    }
    const auto& d = images_[img].desc;
    std::uint64_t px =
        static_cast<std::uint64_t>(d.width) * d.height * d.depth;
    lives.push_back({static_cast<std::uint32_t>(img), first, last, px * 4});
  }
  std::sort(lives.begin(), lives.end(), [](const Life& a, const Life& b) {
    if (a.first != b.first) {
      return a.first < b.first;
    }
    return a.size > b.size;
  });
  struct Block {
    std::uint64_t offset;
    std::uint64_t size;
    std::size_t last;  // last slot occupied
  };
  std::vector<Block> blocks;
  std::uint64_t heap = 0;
  for (const Life& lf : lives) {
    bool placed = false;
    for (Block& bl : blocks) {
      if (bl.last < lf.first || (bl.last == lf.first && lf.first == lf.last)) {
        // Lifetimes [first,last] overlap iff NOT (bl.last < lf.first).
        // Single-slot lives sharing a boundary slot still conflict when
        // they are the SAME slot; the strict < keeps that safe.
        if (bl.size >= lf.size) {
          transients_.push_back({lf.img, bl.offset, lf.size});
          bl.last = lf.last;
          placed = true;
          break;
        }
      }
    }
    if (!placed) {
      // 256-byte alignment for transient rows (driver-friendly pitch).
      heap = (heap + 255) & ~std::uint64_t{255};
      transients_.push_back({lf.img, heap, lf.size});
      blocks.push_back({heap, lf.size, lf.last});
      heap += lf.size;
    }
  }
  transient_heap_bytes_ = heap;

  compiled_ = true;
  return MakeOk();
}

}  // namespace engine::renderer
