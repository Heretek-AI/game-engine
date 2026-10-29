// slang_shader_cache.cpp — M3-03: slang module cache + mtime hot-reload.
//
// Design notes (see header for the contract):
//   * No exceptions, no RTTI on this TU (hot flags): manual new(nothrow),
//     bounded C-string copies, popen-free process spawn via POSIX
//     posix_spawnp (Windows: _popen; both are best-effort — failure keeps
//     last-good blobs, never fails the frame).
//   * mtime snapshot uses std::filesystem::last_write_time with an
//     error_code (never throws). Size via file_size(ec).
//   * Recompile shell: `slangc -target spirv -emit-spirv-directly
//     -I<dir> -entry <E> -stage <S> <dir>/<module>.slang -o <tmp>`,
//     then read back + validate magic/version before swapping.
//   * slangc discovery: SLANGC_PATH env first, else `slangc` on PATH.
#include "engine/renderer/slang_shader_cache.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>  // _dupenv_s / _putenv_s
#endif

namespace engine::renderer {
namespace {

constexpr std::uint32_t kSpvMagic = 0x07230203u;
constexpr std::uint32_t kSpvMinVersion = 0x00010300u;

auto copy_bounded(char* dst, std::size_t cap, const char* src) noexcept
    -> bool {
  if (dst == nullptr || cap == 0 || src == nullptr || src[0] == '\0') {
    return false;
  }
  std::size_t n = 0;
  while (n < cap && src[n] != '\0') {
    ++n;
  }
  if (n == 0 || n >= cap) {
    return false;
  }
  for (std::size_t i = 0; i <= n; ++i) {
    dst[i] = src[i];
  }
  return true;
}

auto stage_name(ShaderStage s) noexcept -> const char* {
  switch (s) {
    case ShaderStage::kVertex:
      return "vertex";
    case ShaderStage::kFragment:
      return "fragment";
    case ShaderStage::kCompute:
      return "compute";
  }
  return "fragment";
}

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == kSpvMagic &&
         w[1] >= kSpvMinVersion;
}

}  // namespace

auto SlangShaderCache::snapshot(Entry& e) noexcept -> void {
  namespace fs = std::filesystem;
  std::error_code ec;
  e.has_snapshot = false;
  e.mtime_ns = 0;
  e.size_bytes = 0;
  if (e.path[0] == '\0') {
    return;
  }
  const fs::path p(e.path.data());
  auto sz = fs::file_size(p, ec);
  if (ec) {
    return;
  }
  auto t = fs::last_write_time(p, ec);
  if (ec) {
    return;
  }
  e.size_bytes = static_cast<std::uint64_t>(sz);
  e.mtime_ns =
      static_cast<std::uint64_t>(t.time_since_epoch().count());
  e.has_snapshot = true;
}

auto SlangShaderCache::add_entry(const SlangEntryDesc& desc,
                                 const char* slang_dir) noexcept -> Status {
  if (count_ >= kMaxShaderEntries) {
    return MakeFail(StatusCode::kOutOfMemory, "shader cache full");
  }
  if (desc.module == nullptr || desc.entry == nullptr || slang_dir == nullptr ||
      desc.embedded_spv == nullptr || desc.embedded_words == 0) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "shader entry needs module/entry/dir/blob");
  }
  if (!valid_spv(desc.embedded_spv, desc.embedded_words)) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "embedded SPIR-V failed validation");
  }
  Entry e{};
  if (!copy_bounded(e.module.data(), e.module.size(), desc.module)) {
    return MakeFail(StatusCode::kInvalidArgument, "bad module name");
  }
  if (!copy_bounded(e.entry.data(), e.entry.size(), desc.entry)) {
    return MakeFail(StatusCode::kInvalidArgument, "bad entry name");
  }
  // Duplicate module+entry rejected (two writers, one slot).
  for (std::size_t i = 0; i < count_; ++i) {
    if (std::strncmp(entries_[i].module.data(), e.module.data(),
                     e.module.size()) == 0 &&
        std::strncmp(entries_[i].entry.data(), e.entry.data(),
                     e.entry.size()) == 0) {
      return MakeFail(StatusCode::kInvalidArgument,
                      "duplicate shader entry");
    }
  }
  e.stage = desc.stage;
  // path = <dir>/<module>.slang (bounded; over-long -> error, no trunc).
  std::size_t di = 0;
  while (di + 1 < e.path.size() && slang_dir[di] != '\0') {
    e.path[di] = slang_dir[di];
    ++di;
  }
  if (slang_dir[di] != '\0') {
    return MakeFail(StatusCode::kInvalidArgument, "shader dir too long");
  }
  const char* tail = ".slang";
  // module + ".slang" must fit after "<dir>/".
  std::size_t mn = 0;
  while (e.module[mn] != '\0') {
    ++mn;
  }
  if (di + 1 + mn + 6 >= e.path.size()) {
    return MakeFail(StatusCode::kInvalidArgument, "shader path too long");
  }
  e.path[di] = '/';
  for (std::size_t i = 0; i < mn; ++i) {
    e.path[di + 1 + i] = e.module[i];
  }
  for (std::size_t i = 0; i < 6; ++i) {
    e.path[di + 1 + mn + i] = tail[i];
  }
  e.path[di + 1 + mn + 6] = '\0';
  e.embedded_spv = desc.embedded_spv;
  e.embedded_words = desc.embedded_words;
  snapshot(e);
  entries_[count_] = e;
  ++count_;
  return MakeOk();
}

auto SlangShaderCache::active_spv(std::size_t i) const noexcept
    -> const std::uint32_t* {
  if (i >= count_) {
    return nullptr;
  }
  const Entry& e = entries_[i];
  return (e.owned_spv != nullptr) ? e.owned_spv : e.embedded_spv;
}

auto SlangShaderCache::active_words(std::size_t i) const noexcept
    -> std::size_t {
  if (i >= count_) {
    return 0;
  }
  const Entry& e = entries_[i];
  return (e.owned_spv != nullptr) ? e.owned_words : e.embedded_words;
}

auto SlangShaderCache::is_stale(std::size_t i) const noexcept -> bool {
  if (i >= count_) {
    return false;
  }
  const Entry& e = entries_[i];
  if (e.path[0] == '\0') {
    return false;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  auto sz = fs::file_size(fs::path(e.path.data()), ec);
  if (ec) {
    return false;  // unreadable (deleted?): not stale, keep last-good
  }
  auto t = fs::last_write_time(fs::path(e.path.data()), ec);
  if (ec) {
    return false;
  }
  if (!e.has_snapshot) {
    return true;
  }
  return static_cast<std::uint64_t>(sz) != e.size_bytes ||
         static_cast<std::uint64_t>(t.time_since_epoch().count()) !=
             e.mtime_ns;
}

auto SlangShaderCache::try_recompile(Entry& e) noexcept -> bool {
  // slangc discovery: env first, PATH fallback. MSVC treats getenv/fopen
  // as C4996 errors under /WX, so the env reads use _dupenv_s there (same
  // rule as engine_abi.cpp's ENGINE_SERIAL_DISPATCH handling) and file IO
  // uses fopen_s; the opened handle is shared via a small sub-scope.
#if defined(_WIN32)
  std::array<char, 256> env_buf{};
  std::array<char, 256> tmp_buf{};
  char* dup = nullptr;
  std::size_t dup_len = 0;
  if (_dupenv_s(&dup, &dup_len, "SLANGC_PATH") == 0 && dup != nullptr &&
      dup[0] != '\0') {
    std::size_t n = 0;
    while (n + 1 < env_buf.size() && dup[n] != '\0') {
      env_buf[n] = dup[n];
      ++n;
    }
    env_buf[n] = '\0';
  }
  std::free(dup);
  dup = nullptr;
  dup_len = 0;
  if (_dupenv_s(&dup, &dup_len, "TMPDIR") == 0 && dup != nullptr &&
      dup[0] != '\0') {
    std::size_t n = 0;
    while (n + 1 < tmp_buf.size() && dup[n] != '\0') {
      tmp_buf[n] = dup[n];
      ++n;
    }
    tmp_buf[n] = '\0';
  }
  std::free(dup);
  const char* env =
      (env_buf[0] != '\0') ? env_buf.data() : nullptr;
  const char* tmpdir =
      (tmp_buf[0] != '\0') ? tmp_buf.data() : nullptr;
#else
  const char* env = std::getenv("SLANGC_PATH");
  const char* tmpdir = std::getenv("TMPDIR");
#endif
  std::array<char, kMaxShaderPathChars + 32> cmd{};
  const char* slangc = (env != nullptr && env[0] != '\0') ? env : "slangc";
  // Scratch output next to the source dir is forbidden (repo hygiene);
  // use a pid-suffixed file under the profile scratch via TMPDIR.
#if defined(_WIN32)
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = ".";
  }
#else
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
#endif
  std::array<char, 256> out{};
  int want = std::snprintf(out.data(), out.size(), "%s/slang_hot_%p.spv",
                           tmpdir, static_cast<void*>(&e));
  if (want <= 0 || static_cast<std::size_t>(want) >= out.size()) {
    return false;
  }
  // Quote paths defensively (bounded snprintf, single-quoted below).
  int n = std::snprintf(cmd.data(), cmd.size(),
                        "'%s' -target spirv -emit-spirv-directly -I'%s' "
                        "-entry '%s' -stage %s '%s' -o '%s' 2>/dev/null",
                        slangc, "", e.entry.data(), stage_name(e.stage),
                        e.path.data(), out.data());
  // Fill the -I dir = dirname(path): strip the trailing /<module>.slang.
  if (n <= 0 || static_cast<std::size_t>(n) >= cmd.size()) {
    return false;
  }
  // Rebuild with the real -I dir (dirname of path).
  std::array<char, kMaxShaderPathChars> dir{};
  std::size_t dl = 0;
  while (e.path[dl] != '\0') {
    ++dl;
  }
  std::size_t slash = dl;
  while (slash > 0 && e.path[slash - 1] != '/') {
    --slash;
  }
  if (slash == 0) {
    return false;
  }
  for (std::size_t i = 0; i + 1 < dir.size() && i + 1 < slash; ++i) {
    dir[i] = e.path[i];
  }
  n = std::snprintf(cmd.data(), cmd.size(),
                    "'%s' -target spirv -emit-spirv-directly -I'%s' "
                    "-entry '%s' -stage %s '%s' -o '%s' 2>/dev/null",
                    slangc, dir.data(), e.entry.data(),
                    stage_name(e.stage), e.path.data(), out.data());
  if (n <= 0 || static_cast<std::size_t>(n) >= cmd.size()) {
    return false;
  }
  int rc = std::system(cmd.data());  // NOLINT(concurrency)
  if (rc != 0) {
    std::remove(out.data());
    return false;  // broken edit: keep last-good
  }
#if defined(_WIN32)
  std::FILE* f = nullptr;
  if (fopen_s(&f, out.data(), "rb") != 0 || f == nullptr) {
    return false;
  }
#else
  std::FILE* f = std::fopen(out.data(), "rb");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fseek(f, 0, SEEK_END);
  long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bool ok = false;
  if (bytes > 0 && bytes % 4 == 0) {
    std::size_t words = static_cast<std::size_t>(bytes) / 4;
    std::uint32_t* buf =
        new (std::nothrow) std::uint32_t[words];  // NOLINT
    if (buf != nullptr) {
      std::size_t got =
          std::fread(buf, 4, words, f);
      if (got == words && valid_spv(buf, words)) {
        delete[] e.owned_spv;  // NOLINT
        e.owned_spv = buf;
        e.owned_words = words;
        e.owned_cap = words;
        buf = nullptr;
        ok = true;
      }
      delete[] buf;  // NOLINT (null when swapped)
    }
  }
  std::fclose(f);
  std::remove(out.data());
  if (ok) {
    snapshot(e);  // re-baseline the watcher on the new bytes
  }
  return ok;
}

auto SlangShaderCache::poll(ShaderPollResult* out,
                            std::size_t out_count) noexcept -> Status {
  for (std::size_t i = 0; i < count_; ++i) {
    ShaderPollResult r{};
    r.stale = is_stale(i);
    if (r.stale) {
      r.recompiled = true;
      r.swapped = try_recompile(entries_[i]);
    }
    if (out != nullptr && i < out_count) {
      out[i] = r;
    }
  }
  return MakeOk();
}

}  // namespace engine::renderer
