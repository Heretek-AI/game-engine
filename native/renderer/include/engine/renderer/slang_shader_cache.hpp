// slang_shader_cache.hpp — M3-03: slang shader module cache + hot-reload.
//
// Owns one compiled SPIR-V blob per entry point. The OFFLINE build path
// (slang_build.py, run by a dev/CI with slangc) checks the .inc blobs in;
// this cache serves them at runtime with zero file IO on the frame path.
//
// Hot-reload (AC1): `poll()` compares each watched .slang file's mtime +
// size against the snapshot taken at load(). When a change is detected AND
// a slangc is available (SLANGC_PATH env or `slangc` on PATH), the module
// is recompiled to a scratch buffer and swapped ONLY if slangc exits 0 —
// a broken edit keeps the last-good SPIR-V live (fail-soft, no frame hitch
// from a typo). Without slangc, poll() reports kShaderStaleAvailable and
// keeps serving the embedded blobs (CI/dev-box determinism).
//
// Threading: load()/poll() are NOT frame-path calls (called from the
// render-thread setup or a background watcher). active() is lock-free
// read-only for the frame path — provided NO poll() runs concurrently
// (poll swaps owned_spv; concurrent active+poll would race the pointer).
// noexcept throughout (hot flags).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// One slang entry point (module = one .slang file on disk).
enum class ShaderStage : std::uint8_t {
  kVertex = 0,
  kFragment = 1,
  kCompute = 2,
};

// Describes one compiled entry point (mirrors slang_build.py ENTRY_POINTS).
struct SlangEntryDesc {
  const char* module{nullptr};   // slang stem, e.g. "pbr_shade"
  const char* entry{nullptr};    // entry point, e.g. "fsShade"
  ShaderStage stage{ShaderStage::kFragment};
  const std::uint32_t* embedded_spv{nullptr};  // checked-in blob
  std::size_t embedded_words{0};
};

inline constexpr std::size_t kMaxShaderEntries = 16;
inline constexpr std::size_t kMaxShaderPathChars = 512;

// Poll outcome for one entry.
struct ShaderPollResult {
  // True when the .slang source changed on disk since load().
  bool stale{false};
  // True when a recompile was attempted (slangc available).
  bool recompiled{false};
  // True when the new SPIR-V validated (magic + >= 1.3) and swapped in.
  bool swapped{false};
};

// Hot-reloadable slang module cache. Capacity: kMaxShaderEntries entries,
// each with a bounded source path. Recompiled SPIR-V lives in an owned
// new[](nothrow) buffer per entry (grown only in poll(), never on the
// frame path; freed in ~SlangShaderCache).
class ENGINE_CORE_API SlangShaderCache {
 public:
  SlangShaderCache() noexcept = default;
  SlangShaderCache(const SlangShaderCache&) = delete;
  auto operator=(const SlangShaderCache&) -> SlangShaderCache& = delete;
  ~SlangShaderCache() noexcept;  // frees owned_spv per entry (hot-swap live set)

  // Register one entry + snapshot its .slang mtime/size. `slang_dir` is
  // the directory holding "<module>.slang". Duplicate module+entry or
  // over-capacity -> error Status (no exceptions).
  auto add_entry(const SlangEntryDesc& desc, const char* slang_dir) noexcept
      -> Status;

  // Frame-path read: currently-active SPIR-V for entry `i` (embedded blob
  // until a hot-reload swap succeeds). Index bounds-checked (null on OOB).
  [[nodiscard]] auto active_spv(std::size_t i) const noexcept
      -> const std::uint32_t*;
  [[nodiscard]] auto active_words(std::size_t i) const noexcept -> std::size_t;
  [[nodiscard]] auto entry_count() const noexcept -> std::size_t {
    return count_;
  }

  // Returns true when entry i's source changed on disk since add_entry().
  // Pure check: no compile, no swap (drives the "stale" test pin).
  [[nodiscard]] auto is_stale(std::size_t i) const noexcept -> bool;

  // Check all entries for source changes; recompile+swap where slangc is
  // available. `out` (optional) receives per-entry results. Never throws;
  // a failed recompile keeps the last-good blob and reports swapped=false.
  auto poll(ShaderPollResult* out, std::size_t out_count) noexcept -> Status;

 private:
  struct Entry {
    std::array<char, 64> module{};
    std::array<char, 64> entry{};
    ShaderStage stage{ShaderStage::kFragment};
    std::array<char, kMaxShaderPathChars> path{};
    std::uint64_t mtime_ns{0};
    std::uint64_t size_bytes{0};
    const std::uint32_t* embedded_spv{nullptr};
    std::size_t embedded_words{0};
    // Recompiled words live here after a successful swap (owned, grown
    // only in poll()).
    std::uint32_t* owned_spv{nullptr};
    std::size_t owned_words{0};
    std::size_t owned_cap{0};
    bool has_snapshot{false};
  };

  Entry entries_[kMaxShaderEntries]{};
  std::size_t count_{0};

  auto snapshot(Entry& e) noexcept -> void;
  auto try_recompile(Entry& e) noexcept -> bool;
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif
