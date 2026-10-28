// dotnet_host.cpp — M1-05 implementation; see dotnet_host.hpp.
//
// Resolution order for libhostfxr (first hit wins, all noexcept/dlerror):
//   1. DOTNET_ROOT env (host/fxr/<ver>/libhostfxr.so) — hermetic CI/dev.
//   2. nethost get_hostfxr_path (linked statically from the .NET pack).
// Failure anywhere -> create() returns nullptr; the Benchmark1M runner
// reports which stage failed (no silent fallback to a wrong runtime).
#include "engine/bridge/dotnet_host.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// nethost ships as a static lib in the Microsoft.NETCore.App.Host pack;
// link it via the bridge target's compile include + link lib below. When
// the pack is unavailable (offline configure, wrong RID), fall back to a
// minimal local declaration of get_hostfxr_path — the implementation lives
// in libnethost.a when linked, and the runtime fallback in ScanFxDir
// (DOTNET_ROOT/host/fxr) applies either way.
#if defined(ENGINE_HAS_NETHOST) && __has_include("nethost.h")
#include "nethost.h"
#else
// No nethost on this configure (ENGINE_HAS_NETHOST unset): resolve hostfxr
// purely via ScanFxDir. get_hostfxr_path is unavailable — TryNethost is a
// stub returning false so create() falls back to DOTNET_ROOT/host/fxr.
inline auto TryNethostStub() noexcept -> bool { return false; }
#define DOTNET_HOST_NO_NETHOST 1
#endif

// Portable shims: dlopen/dlsym/dlclose + opendir/readdir/closedir exist on
// POSIX; on Windows they map to LoadLibrary/GetProcAddress/FreeLibrary and
// a FindFirstFile-based directory scan. Keeps dotnet_host.cpp building
// under MSVC (/WX) without dirent.h.

// hostfxr entry points we resolve via dlsym (exact signatures from the
// .NET 10 hostfxr.h; NOT including it keeps engine sources free of the
// pack's include path beyond nethost.h).
using HostfxrInitForConfigFn = int (*)(const char* runtime_config_path,
                                       const void* parameters,
                                       void** host_context_handle);
using HostfxrGetDelegateFn = int (*)(const void* host_context_handle, int type,
                                     void** delegate);
using HostfxrCloseFn = int (*)(const void* host_context_handle);

namespace engine::bridge {
namespace {

// Portable env/file shims (MSVC treats getenv/fopen as C4996 errors under
// /WX; the POSIX paths stay untouched):
#if defined(_WIN32)
auto EnvGet(const char* name, char* out, std::size_t cap) noexcept -> bool {
  if (name == nullptr || out == nullptr || cap == 0) {
    return false;
  }
  char* buf = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr || buf[0] == '\0') {
    std::free(buf);
    return false;
  }
  std::size_t n = std::strlen(buf);
  if (n + 1 > cap) {
    std::free(buf);
    return false;
  }
  std::memcpy(out, buf, n + 1);
  std::free(buf);
  return true;
}

auto FileReadable(const char* path) noexcept -> bool {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
    return false;
  }
  std::fclose(f);
  return true;
}
#else
auto EnvGet(const char* name, char* out, std::size_t cap) noexcept -> bool {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0' || out == nullptr || cap == 0) {
    return false;
  }
  std::size_t n = std::strlen(v);
  if (n + 1 > cap) {
    return false;
  }
  std::memcpy(out, v, n + 1);
  return true;
}

auto FileReadable(const char* path) noexcept -> bool {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  FILE* probe = std::fopen(path, "rb");
  if (probe == nullptr) {
    return false;
  }
  std::fclose(probe);
  return true;
}
#endif

// Platform library handle: dlopen handle (POSIX) or HMODULE (Windows).
#if defined(_WIN32)
using LibHandle = HMODULE;
#else
using LibHandle = void*;
#endif

constexpr int kDelegateLoadAssembly = 5;  // hdt_load_assembly... (hostfxr.h)

// UNMANAGEDCALLERSONLY_METHOD sentinel (coreclr_delegates.h): tells the
// loader the target is [UnmanagedCallersOnly] so no delegate type is needed.
// (-1 as a pointer is not a constant expression under -Werror, so this is
// a plain const, not constexpr.)
const char* const kUnmanagedCallersOnly =
    reinterpret_cast<const char*>(static_cast<uintptr_t>(-1));  // NOLINT

struct HostImpl {
  LibHandle fxr{nullptr};
  void* ctx{nullptr};
  LoadAssemblyFn load_fn{nullptr};
  char error[256]{};
};

auto LogBoot(const char* stage, const char* detail) noexcept -> void {
  std::fprintf(stderr, "[dotnet_host] %s%s%s\n", stage,
               detail != nullptr ? ": " : "", detail != nullptr ? detail : "");
}

// Forward hostfxr's own diagnostics to stderr (otherwise init failures
// are a bare rc with no reason). The writer registration is per-thread,
// so this runs on every thread that will call hostfxr (here: the single
// lifecycle thread that also calls initialize_for_runtime_config).
// CTest splits output per \n but a bare \r (Windows \r\n in hostfxr text)
// truncates the visible line — sanitize both so the full message survives.
auto HostfxrErrorWriter(const char* msg) noexcept -> void {
  if (msg == nullptr) {
    std::fprintf(stderr, "[hostfxr] (null)\n");
    return;
  }
  char clean[2048];
  std::size_t n = 0;
  for (const char* p = msg; *p != '\0' && n + 1 < sizeof(clean); ++p) {
    if (*p == '\r' || *p == '\n') {
      if (n > 0 && clean[n - 1] != ' ') {
        clean[n++] = ' ';
      }
    } else {
      clean[n++] = *p;
    }
  }
  clean[n] = '\0';
  std::fprintf(stderr, "[hostfxr] %s\n", clean);
}

auto InstallErrorWriter(LibHandle fxr) noexcept -> void {
  using SetWriterFn = void* (*)(void (*)(const char*));
#if defined(_WIN32)
  auto set_writer = reinterpret_cast<SetWriterFn>(  // NOLINT
      reinterpret_cast<void*>(GetProcAddress(fxr, "hostfxr_set_error_writer")));
#else
  auto set_writer = reinterpret_cast<SetWriterFn>(  // NOLINT
      dlsym(fxr, "hostfxr_set_error_writer"));
#endif
  if (set_writer == nullptr) {
    return;
  }
  set_writer(&HostfxrErrorWriter);
}

auto PlatformDlopen(const char* path, HostImpl* impl) noexcept -> LibHandle {
#if defined(_WIN32)
  // hostfxr.dll has no dlerror: record the Win32 code instead.
  LibHandle h = LoadLibraryA(path);
  if (h == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error), "LoadLibraryA rc=%lu",
                  static_cast<unsigned long>(GetLastError()));
    return nullptr;
  }
  return h;
#else
  void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error), "%.200s", dlerror());
    return nullptr;
  }
  return h;
#endif
}

auto PlatformDlclose(LibHandle h) noexcept -> void {
#if defined(_WIN32)
  if (h != nullptr) {
    FreeLibrary(h);
  }
#else
  if (h != nullptr) {
    dlclose(h);
  }
#endif
}

auto PlatformDlsym(LibHandle h, const char* name) noexcept -> void* {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(h, name));  // NOLINT
#else
  return dlsym(h, name);
#endif
}

auto TryDlopen(const char* path, HostImpl* impl) noexcept -> bool {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  LibHandle h = PlatformDlopen(path, impl);
  if (h == nullptr) {
    return false;
  }
  impl->fxr = h;
  InstallErrorWriter(h);
  return true;
}

// DOTNET_ROOT/host/fxr/<ver>/{libhostfxr.so,hostfxr.dll,libhostfxr.dylib}:
// scan the fxr dir for the lexicographically greatest version entry
// (stable SemVer sorts correctly). Runs FIRST: the install the SDK lays
// down is authoritative, and nethost's get_hostfxr_path can disagree
// (stale args, arch mismatch — observed rc=-2147450749 on macOS CI).
//
// DOTNET_ROOT resolution order: explicit dotnet_root arg (create()),
// then the DOTNET_ROOT env (setup-dotnet + hermetic dev), then the
// well-known install prefixes (/usr/share/dotnet, /usr/lib/dotnet,
// $HOME/.dotnet, /usr/local/share/dotnet). The smoke test's runtimeconfig
// lives next to the dotnet-built fixtures, so any of these locates a
// working hostfxr even when the env is unset.
static auto FxDirFromRoot(const char* root, char* out,
                          std::size_t cap) noexcept -> bool {
  if (root == nullptr || root[0] == '\0' || out == nullptr || cap == 0) {
    return false;
  }
  int n = std::snprintf(out, cap, "%s/host/fxr", root);
  return n > 0 && static_cast<std::size_t>(n) < cap;
}

// Scan ONE fxr dir for the newest hostfxr by (major, minor, patch);
// prerelease strings break ties only when the numerics match. String
// comparison misorders multi-digit components ("10.0.9" > "10.0.14"),
// so parse the numerics explicitly. Writes the winning path to out.
static auto BestHostfxrIn(const char* fxr_dir, char* out,
                          std::size_t cap) noexcept -> bool {
  struct Ver {
    int major{0};
    int minor{0};
    int patch{0};
    std::string pre;
    std::string path;
  };
  auto parse = [](const char* name, Ver& v) -> bool {
    // <major>.<minor>.<patch>[-prerelease], parsed by hand: std::sscanf
    // is C4996-deprecated under MSVC /WX (needs sscanf_s), so a tiny
    // manual parser keeps one code path on all three OSes.
    const char* p = name;
    auto digits = [](const char*& q, int& out) -> bool {
      if (*q < '0' || *q > '9') {
        return false;
      }
      long acc = 0;
      int len = 0;
      while (*q >= '0' && *q <= '9') {
        acc = acc * 10 + (*q - '0');
        ++q;
        ++len;
        if (len > 9 || acc > 999999999L) {
          return false;
        }
      }
      out = static_cast<int>(acc);
      return true;
    };
    if (!digits(p, v.major) || *p != '.') {
      return false;
    }
    ++p;
    if (!digits(p, v.minor) || *p != '.') {
      return false;
    }
    ++p;
    if (!digits(p, v.patch)) {
      return false;
    }
    if (*p == '-') {
      v.pre = p + 1;
      if (v.pre.empty()) {
        return false;
      }
    } else if (*p != '\0') {
      return false;
    }
    return v.major >= 0 && v.minor >= 0 && v.patch >= 0;
  };
  auto better = [](const Ver& a, const Ver& b) -> bool {
    // true when a outranks b. Release (empty pre) beats any prerelease
    // of the same numerics.
    if (a.major != b.major) {
      return a.major > b.major;
    }
    if (a.minor != b.minor) {
      return a.minor > b.minor;
    }
    if (a.patch != b.patch) {
      return a.patch > b.patch;
    }
    if (a.pre.empty() != b.pre.empty()) {
      return a.pre.empty();
    }
    return a.pre > b.pre;
  };
  bool have = false;
  Ver best{};
  auto consider = [&](const char* dir_name, const char* lib) {
    char dir[1024];
    int n = std::snprintf(dir, sizeof(dir), "%s/%s", fxr_dir, dir_name);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(dir)) {
      return;
    }
    std::string cand = std::string(dir) + "/" + lib;
    if (!FileReadable(cand.c_str())) {
      return;
    }
    Ver v{};
    if (!parse(dir_name, v)) {
      return;
    }
    v.path = cand;
    if (!have || better(v, best)) {
      best = v;
      have = true;
    }
  };
#if defined(_WIN32)
  // Windows: enumerate version dirs with FindFirstFile (no dirent.h).
  std::string pattern = std::string(fxr_dir) + "/*";
  WIN32_FIND_DATAA fd{};
  HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (fd.cFileName[0] == '.') {
        continue;
      }
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        continue;
      }
      consider(fd.cFileName, "hostfxr.dll");
    } while (FindNextFileA(find, &fd) != 0);
    FindClose(find);
  }
#else
  DIR* d = opendir(fxr_dir);
  if (d == nullptr) {
    return false;
  }
#if defined(__APPLE__)
  const char* kLibName = "libhostfxr.dylib";
#else
  const char* kLibName = "libhostfxr.so";
#endif
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    consider(e->d_name, kLibName);
  }
  closedir(d);
#endif
  if (!have || best.path.size() + 1 > cap) {
    return false;
  }
  std::memcpy(out, best.path.c_str(), best.path.size() + 1);
  return true;
}

auto ScanFxDir(HostImpl* impl) noexcept -> bool {
  // Walk candidate roots in order (override handled by the create() entry
  // point, which publishes DOTNET_ROOT first): env, then well-known
  // prefixes. First root with a hostfxr wins.
  char fxr[1024];
  char cand[4096];
  char env[4096];
  // 1. DOTNET_ROOT env.
  if (EnvGet("DOTNET_ROOT", env, sizeof(env)) &&
      FxDirFromRoot(env, fxr, sizeof(fxr)) &&
      BestHostfxrIn(fxr, cand, sizeof(cand))) {
    LogBoot("hostfxr candidate", cand);
    return TryDlopen(cand, impl);
  }
#if !defined(_WIN32)
  // 2. Well-known POSIX prefixes (setup-dotnet macOS/Linux, no env).
  static const char* kPrefixes[] = {"/usr/share/dotnet", "/usr/lib/dotnet",
                                    "/usr/local/share/dotnet"};
  for (const char* p : kPrefixes) {
    if (FxDirFromRoot(p, fxr, sizeof(fxr)) &&
        BestHostfxrIn(fxr, cand, sizeof(cand))) {
      LogBoot("hostfxr candidate", cand);
      return TryDlopen(cand, impl);
    }
  }
  // 3. $HOME/.dotnet (user-local SDK install).
  char home[1024];
  if (EnvGet("HOME", home, sizeof(home))) {
    char root[1024];
    int n = std::snprintf(root, sizeof(root), "%s/.dotnet", home);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(root) &&
        FxDirFromRoot(root, fxr, sizeof(fxr)) &&
        BestHostfxrIn(fxr, cand, sizeof(cand))) {
      LogBoot("hostfxr candidate", cand);
      return TryDlopen(cand, impl);
    }
  }
#else
  // 2. Windows defaults (setup-dotnet, no env).
  static const char* kPrefixes[] = {"C:/Program Files/dotnet",
                                    "C:/Program Files (x86)/dotnet"};
  for (const char* p : kPrefixes) {
    if (FxDirFromRoot(p, fxr, sizeof(fxr)) &&
        BestHostfxrIn(fxr, cand, sizeof(cand))) {
      LogBoot("hostfxr candidate", cand);
      return TryDlopen(cand, impl);
    }
  }
#endif
  return false;
}

auto TryNethost(HostImpl* impl) noexcept -> bool {
#ifdef DOTNET_HOST_NO_NETHOST
  (void)impl;
  return false;
#else
#if defined(_WIN32)
  // nethost's char_t is wchar_t on Windows: convert explicitly — the
  // implicit char[4096] -> wchar_t* conversion is C2664 under /WX.
  wchar_t wbuf[4096];
  size_t sz = sizeof(wbuf) / sizeof(wchar_t);
  struct get_hostfxr_parameters params {};
  params.size = sizeof(params);
  params.assembly_path = nullptr;
  params.dotnet_root = nullptr;
  const int rc = get_hostfxr_path(wbuf, &sz, &params);
  if (rc != 0) {
    std::snprintf(impl->error, sizeof(impl->error), "get_hostfxr_path rc=%d",
                  rc);
    return false;
  }
  char narrow[4096];
  errno_t conv = wcstombs_s(nullptr, narrow, sizeof(narrow), wbuf,
                             _TRUNCATE);
  if (conv != 0) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "get_hostfxr_path: wide->narrow failed rc=%d",
                  static_cast<int>(conv));
    return false;
  }
  LogBoot("nethost resolved hostfxr", narrow);
  return TryDlopen(narrow, impl);
#else
  char_t buf[4096];
  size_t sz = sizeof(buf) / sizeof(char_t);
  struct get_hostfxr_parameters params {};
  params.size = sizeof(params);
  params.assembly_path = nullptr;
  params.dotnet_root = nullptr;
  const int rc = get_hostfxr_path(buf, &sz, &params);
  if (rc != 0) {
    std::snprintf(impl->error, sizeof(impl->error), "get_hostfxr_path rc=%d",
                  rc);
    return false;
  }
  LogBoot("nethost resolved hostfxr", buf);
  return TryDlopen(buf, impl);
#endif
#endif
}

auto BindAndInit(HostImpl* impl, const char* runtime_config_path) noexcept
    -> bool {
  auto init = reinterpret_cast<HostfxrInitForConfigFn>(  // NOLINT
      PlatformDlsym(impl->fxr, "hostfxr_initialize_for_runtime_config"));
  auto get_del = reinterpret_cast<HostfxrGetDelegateFn>(  // NOLINT
      PlatformDlsym(impl->fxr, "hostfxr_get_runtime_delegate"));
  auto close_fn = reinterpret_cast<HostfxrCloseFn>(  // NOLINT
      PlatformDlsym(impl->fxr, "hostfxr_close"));
  (void)close_fn;
  if (init == nullptr || get_del == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "hostfxr symbols missing (init=%p get=%p)",
                  reinterpret_cast<void*>(init),   // NOLINT
                  reinterpret_cast<void*>(get_del));  // NOLINT
    return false;
  }
  // The error-writer registration is per-thread: this lifecycle thread must
  // (re-)register before calling init, or diagnostics stay silent.
  InstallErrorWriter(impl->fxr);
  void* ctx = nullptr;
  // hostfxr uses negative rc for failures; treat only rc < 0 as failure
  // (a zero rc with null ctx is still unusable). On failure with a live
  // ctx, close it via the resolved close_fn before returning.
  const int rc = init(runtime_config_path, nullptr, &ctx);
  if (rc < 0 || ctx == nullptr) {
    if (ctx != nullptr && close_fn != nullptr) {
      close_fn(ctx);
    }
    std::snprintf(impl->error, sizeof(impl->error),
                  "hostfxr_initialize_for_runtime_config rc=%d", rc);
    return false;
  }
  impl->ctx = ctx;
  void* del = nullptr;
  const int grc = get_del(ctx, kDelegateLoadAssembly, &del);
  if (grc != 0 || del == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "hostfxr_get_runtime_delegate rc=%d", grc);
    auto close = reinterpret_cast<HostfxrCloseFn>(  // NOLINT
        PlatformDlsym(impl->fxr, "hostfxr_close"));
    if (close != nullptr) {
      close(ctx);
    }
    impl->ctx = nullptr;
    return false;
  }
  impl->load_fn = reinterpret_cast<LoadAssemblyFn>(del);  // NOLINT
  return true;
}

}  // namespace

auto dotnet_host_create(const char* runtime_config_path,
                        const char* dotnet_root) noexcept -> DotNetHost* {
  if (runtime_config_path == nullptr) {
    return nullptr;
  }
  // dotnet_root override: highest precedence for hermetic runs. NOTE:
  // hostfxr reads DOTNET_ROOT from the process environment (not the
  // initialize-parameters, which are reserved): publish the override so a
  // second CoreCLR in one process resolves the same install.
  // Threading contract: create/destroy/load on ONE thread (M1-05
  // serializes lifecycle); the loaded entry point may run managed systems
  // on any thread.
  if (dotnet_root != nullptr && dotnet_root[0] != '\0') {
#if defined(_WIN32)
    (void)_putenv_s("DOTNET_ROOT", dotnet_root);
#else
    (void)setenv("DOTNET_ROOT", dotnet_root, 1);
#endif
  }
  auto* impl = new (std::nothrow) HostImpl();
  if (impl == nullptr) {
    return nullptr;
  }
  LogBoot("runtime config", runtime_config_path);
  if (!ScanFxDir(impl) && !TryNethost(impl)) {
    LogBoot("hostfxr resolution FAILED", impl->error);
    delete impl;  // NOLINT
    return nullptr;
  }
  if (!BindAndInit(impl, runtime_config_path)) {
    LogBoot("hostfxr init FAILED", impl->error);
    if (impl->fxr != nullptr) {
      PlatformDlclose(impl->fxr);
      impl->fxr = nullptr;
    }
    delete impl;  // NOLINT
    return nullptr;
  }
  LogBoot("CoreCLR initialized", nullptr);
  return reinterpret_cast<DotNetHost*>(impl);  // NOLINT
}

auto dotnet_host_destroy(DotNetHost* host) noexcept -> void {
  if (host == nullptr) {
    return;
  }
  auto* impl = reinterpret_cast<HostImpl*>(host);  // NOLINT
  if (impl->ctx != nullptr) {
    auto close = reinterpret_cast<HostfxrCloseFn>(  // NOLINT
        PlatformDlsym(impl->fxr, "hostfxr_close"));
    if (close != nullptr) {
      close(impl->ctx);
    }
  }
  if (impl->fxr != nullptr) {
    PlatformDlclose(impl->fxr);
    impl->fxr = nullptr;
  }
  delete impl;  // NOLINT
}

auto dotnet_host_load_fn(DotNetHost* host) noexcept -> LoadAssemblyFn {
  if (host == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<HostImpl*>(host)->load_fn;  // NOLINT
}

auto dotnet_host_get_system_pointer(DotNetHost* host, const char* assembly_path,
                                    const char* type_name,
                                    const char* method_name) noexcept -> void* {
  if (host == nullptr || assembly_path == nullptr || type_name == nullptr ||
      method_name == nullptr) {
    return nullptr;
  }
  auto* impl = reinterpret_cast<HostImpl*>(host);  // NOLINT
  if (impl->load_fn == nullptr) {
    return nullptr;
  }
  void* fn = nullptr;
  const int rc = impl->load_fn(assembly_path, type_name, method_name,
                               kUnmanagedCallersOnly, nullptr, &fn);
  if (rc != 0 || fn == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "load_assembly_and_get_function_pointer rc=%d", rc);
    LogBoot("bind FAILED", impl->error);
    return nullptr;
  }
  return fn;
}

}  // namespace engine::bridge
