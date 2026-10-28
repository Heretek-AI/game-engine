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
#include <unistd.h>
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
//
// char_t is wchar_t on Windows: every string-taking entry point
// (initialize_for_runtime_config, load_assembly_and_get_function_pointer,
// set_error_writer) is WIDE there. The narrow typedefs below are the POSIX
// spelling; the Windows branch resolves the same symbols through the wide
// typedefs declared here. Calling a wide entry point through a narrow
// typedef interprets UTF-8 bytes as UTF-16 (observed Windows rc=0x80008093
// "runtimeconfig.json does not exist" for a file that fopen reads fine).
using HostfxrInitForConfigFn = int (*)(const char* runtime_config_path,
                                       const void* parameters,
                                       void** host_context_handle);
using HostfxrGetDelegateFn = int (*)(const void* host_context_handle, int type,
                                     void** delegate);
using HostfxrCloseFn = int (*)(const void* host_context_handle);
#if defined(_WIN32)
using HostfxrInitForConfigFnW = int (*)(const wchar_t* runtime_config_path,
                                        const void* parameters,
                                        void** host_context_handle);
// delegate_type_name is const char_t* in hostfxr.h (the -1 sentinel has the
// same bit pattern wide or narrow); typed void* here so the sentinel needs
// no string conversion at the call site.
using LoadAssemblyFnW = int (*)(const wchar_t* assembly_path,
                                const wchar_t* type_name,
                                const wchar_t* method_name,
                                const void* delegate_type_name, void* reserved,
                                void** delegate);
#endif

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

// UTF-8 narrow -> UTF-16 wide for hostfxr's Windows char_t entry points.
// Stack-buffered, noexcept (MultiByteToWideChar never throws); false when
// the conversion does not fit (caller fails closed with impl->error).
auto NarrowToWide(const char* src, wchar_t* out, std::size_t cap) noexcept
    -> bool {
  if (src == nullptr || out == nullptr || cap == 0) {
    return false;
  }
  const int need = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
  if (need <= 0 || static_cast<std::size_t>(need) > cap) {
    return false;
  }
  return ::MultiByteToWideChar(CP_UTF8, 0, src, -1, out, need) == need;
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
// A message that still arrives truncated (e.g. wide chars through a narrow
// writer) also gets a hex prefix of its first bytes for diagnosis.
auto HostfxrErrorWriter(const char* msg) noexcept -> void {
  if (msg == nullptr) {
    std::fprintf(stderr, "[hostfxr] (null)\n");
    return;
  }
  char clean[2048];
  std::size_t n = 0;
  const char* p = msg;
  for (; *p != '\0' && n + 1 < sizeof(clean); ++p) {
    if (*p == '\r' || *p == '\n') {
      if (n > 0 && clean[n - 1] != ' ') {
        clean[n++] = ' ';
      }
    } else {
      clean[n++] = *p;
    }
  }
  clean[n] = '\0';
  // Hex prefix: reveals encoding surprises (e.g. UTF-16 through char* shows
  // as "T" with 54 00 68 00 ...). Cap at 64 bytes to keep the log readable.
  // If the bytes look like UTF-16LE (NUL every odd byte), decode the ASCII
  // range so the true message is readable instead of a single "T".
  bool looks_utf16 = true;
  std::size_t ulen = 0;
  // Bounded over-read: msg comes from hostfxr's long-lived trace buffer
  // (SSO/heap slack in practice); only runs on the error path. First probe
  // 32 pairs: if the whole prefix is UTF-16-ASCII with no terminator, the
  // string is proven long, so extending to 256 pairs is safe.
  for (; ulen < 32; ++ulen) {
    const unsigned char lo = static_cast<unsigned char>(msg[ulen * 2]);
    const unsigned char hi =
        static_cast<unsigned char>(msg[ulen * 2 + 1]);
    if (lo == 0 && hi == 0) {
      break;
    }
    if (hi != 0 || lo == 0 || lo >= 0x80) {
      looks_utf16 = (ulen == 0);
      break;
    }
  }
  if (looks_utf16 && ulen > 1) {
    if (ulen == 32) {
      // No terminator in the first 32 pairs: the string is proven long,
      // extend the scan (same UTF-16-ASCII validity rules).
      for (; ulen < 256; ++ulen) {
        const unsigned char lo =
            static_cast<unsigned char>(msg[ulen * 2]);
        const unsigned char hi =
            static_cast<unsigned char>(msg[ulen * 2 + 1]);
        if (lo == 0 && hi == 0) {
          break;
        }
        if (hi != 0 || lo == 0 || lo >= 0x80) {
          break;
        }
      }
    }
    // Raw bytes past the scan stop (bounded): distinguishes a truly
    // truncated hostfxr message from a scan-stop on non-ASCII data.
    // The runtimeconfig message mixes wide prefix + narrow path tail, so
    // dump generously (the string is proven long — error path only).
    {
      char tail[256 * 3 + 1];
      std::size_t tn = 0;
      for (std::size_t i = 0; i < 256; ++i) {
        const unsigned char c =
            static_cast<unsigned char>(msg[(ulen * 2) + i]);
        if (c == 0) {
          break;
        }
        const char* digits = "0123456789abcdef";
        tail[tn++] = digits[(c >> 4) & 0xF];
        tail[tn++] = digits[c & 0xF];
        tail[tn++] = ' ';
      }
      tail[tn] = '\0';
      std::fprintf(stderr, "[hostfxr] (narrow tail: %s)\n", tail);
    }
    char wide[512];
    std::size_t wn = 0;
    for (std::size_t i = 0; i < ulen && wn + 1 < sizeof(wide); ++i) {
      wide[wn++] = static_cast<char>(msg[i * 2]);
    }
    wide[wn] = '\0';
    std::fprintf(stderr, "[hostfxr] %s (utf16-decoded, %u chars)\n", wide,
                 static_cast<unsigned>(ulen));
    return;
  }
  char hex[64 * 3 + 1];
  std::size_t hn = 0;
  for (std::size_t i = 0; i < 64 && msg[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(msg[i]);
    const char* digits = "0123456789abcdef";
    hex[hn++] = digits[(c >> 4) & 0xF];
    hex[hn++] = digits[c & 0xF];
    hex[hn++] = ' ';
  }
  hex[hn] = '\0';
  std::fprintf(stderr, "[hostfxr] %s (bytes: %s)\n", clean, hex);
}

auto InstallErrorWriter(LibHandle fxr) noexcept -> void {
#if defined(_WIN32)
  // hostfxr's writer takes const wchar_t* on Windows; convert to UTF-8 and
  // forward to the shared sanitizer so Windows diagnostics read cleanly
  // instead of arriving as single-"T" truncated narrow bytes.
  struct WideForwarder {
    static auto Write(const wchar_t* msg) noexcept -> void {
      if (msg == nullptr) {
        std::fprintf(stderr, "[hostfxr] (null)\n");
        return;
      }
      char narrow[2048];
      const int n = ::WideCharToMultiByte(
          CP_UTF8, 0, msg, -1, narrow, static_cast<int>(sizeof(narrow)),
          nullptr, nullptr);
      if (n <= 0) {
        std::fprintf(stderr, "[hostfxr] (wide message, conversion failed)\n");
        return;
      }
      HostfxrErrorWriter(narrow);
    }
  };
  using SetWriterFnW = void* (*)(void (*)(const wchar_t*));
  auto set_writer = reinterpret_cast<SetWriterFnW>(  // NOLINT
      reinterpret_cast<void*>(GetProcAddress(fxr, "hostfxr_set_error_writer")));
  if (set_writer == nullptr) {
    return;
  }
  set_writer(&WideForwarder::Write);
#else
  using SetWriterFn = void* (*)(void (*)(const char*));
  auto set_writer = reinterpret_cast<SetWriterFn>(  // NOLINT
      dlsym(fxr, "hostfxr_set_error_writer"));
  if (set_writer == nullptr) {
    return;
  }
  set_writer(&HostfxrErrorWriter);
#endif
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
  void* init_sym = PlatformDlsym(impl->fxr, "hostfxr_initialize_for_runtime_config");
  auto get_del = reinterpret_cast<HostfxrGetDelegateFn>(  // NOLINT
      PlatformDlsym(impl->fxr, "hostfxr_get_runtime_delegate"));
  auto close_fn = reinterpret_cast<HostfxrCloseFn>(  // NOLINT
      PlatformDlsym(impl->fxr, "hostfxr_close"));
  (void)close_fn;
  if (init_sym == nullptr || get_del == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "hostfxr symbols missing (init=%p get=%p)",
                  init_sym, reinterpret_cast<void*>(get_del));  // NOLINT
    return false;
  }
#if defined(_WIN32)
  // Wide entry point on Windows (see the char_t note at the typedefs).
  auto init = reinterpret_cast<HostfxrInitForConfigFnW>(init_sym);  // NOLINT
#else
  auto init = reinterpret_cast<HostfxrInitForConfigFn>(init_sym);  // NOLINT
#endif
  // The error-writer registration is per-thread: this lifecycle thread must
  // (re-)register before calling init, or diagnostics stay silent.
  InstallErrorWriter(impl->fxr);
  void* ctx = nullptr;
  // Windows: hostfxr's pal helpers split paths on '\\' only. A CMake-style
  // forward-slash runtimeconfig path opens fine with fopen but breaks
  // hostfxr's derived-path lookups (deps.json, apphost dir), surfacing as
  // "The specified runtimeconfig.json [...] does not exist"
  // (rc=0x80008093). Normalize to native separators before calling in.
  // Same for the assembly path in dotnet_host_get_system_pointer below.
#if defined(_WIN32)
  char native_config[4096];
  {
    std::size_t i = 0;
    for (; runtime_config_path[i] != '\0' && i + 1 < sizeof(native_config);
         ++i) {
      native_config[i] =
          (runtime_config_path[i] == '/') ? '\\' : runtime_config_path[i];
    }
    native_config[i] = '\0';
  }
  // UTF-16 for hostfxr's wide char_t (narrow bytes would be read as UTF-16
  // and the file "not found" — rc=0x80008093). Log the narrow form: LogBoot
  // takes const char*.
  wchar_t wconfig[4096];
  if (!NarrowToWide(native_config, wconfig,
                    sizeof(wconfig) / sizeof(wconfig[0]))) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "runtimeconfig path not convertible to UTF-16");
    return false;
  }
  LogBoot("init path", native_config);
  const wchar_t* init_path = wconfig;
#else
  const char* init_path = runtime_config_path;
  LogBoot("init path", init_path);
#endif
  // hostfxr uses negative rc for failures; treat only rc < 0 as failure
  // (a zero rc with null ctx is still unusable). On failure with a live
  // ctx, close it via the resolved close_fn before returning.
  // Retry with backoff: the fixture runtimeconfig was written seconds ago
  // by the fixture build step; on Windows a fresh file can be transiently
  // unreadable (real-time AV scan / indexer exclusive hold). The failure is
  // deterministic in CI precisely because test 7 starts ~30ms after the
  // build ends — every run hits the same in-flight scan. Retries (error
  // path only) let the transient clear; a persistent failure still fails
  // after ~6s with all rcs logged.
  int rc = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) {
#if defined(_WIN32)
      ::Sleep(2000);
#else
      ::usleep(2000000);
#endif
      std::fprintf(stderr, "[dotnet_host] init retry %d/2\n", attempt);
    }
    rc = init(init_path, nullptr, &ctx);
    if (rc >= 0 && ctx != nullptr) {
      break;
    }
    std::fprintf(stderr, "[dotnet_host] init attempt %d rc=%d\n", attempt, rc);
    if (ctx != nullptr && close_fn != nullptr) {
      close_fn(ctx);
      ctx = nullptr;
    }
  }
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
  // Relevant host env (affects framework resolution; log before init).
  {
    const char* keys[] = {"DOTNET_ROOT", "DOTNET_ROOT_X64", "DOTNET_ROOT_X86",
                          "DOTNET_ROLL_FORWARD", "DOTNET_ROLL_FORWARD_TO_PRERELEASE",
                          "DOTNET_MULTILEVEL_LOOKUP"};
    for (const char* k : keys) {
      char val[1024];
      if (EnvGet(k, val, sizeof(val))) {
        std::fprintf(stderr, "[dotnet_host] env %s=%s\n", k, val);
      } else {
        std::fprintf(stderr, "[dotnet_host] env %s=(unset)\n", k);
      }
    }
  }
  // Log the first bytes of the runtimeconfig so a malformed fixture
  // (e.g. a platform-specific `dotnet build` emitting an unexpected
  // framework/rollForward) is visible in CI without extra steps.
  {
    FILE* probe = nullptr;
#if defined(_WIN32)
    (void)fopen_s(&probe, runtime_config_path, "rb");
#else
    probe = std::fopen(runtime_config_path, "rb");
#endif
    if (probe != nullptr) {
      char head[512];
      const std::size_t got = std::fread(head, 1, sizeof(head) - 1, probe);
      head[got] = '\0';
      for (std::size_t i = 0; i < got; ++i) {
        if (head[i] == '\r' || head[i] == '\n') {
          head[i] = ' ';
        }
      }
      LogBoot("runtimeconfig head", head);
      std::fclose(probe);
    } else {
      LogBoot("runtimeconfig unreadable", runtime_config_path);
    }
  }
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
  // Native separators on Windows (see BindAndInit): load_assembly resolves
  // the assembly path through the same pal helpers. Wide char_t like init:
  // type/method names convert alongside the path; the -1 UCO sentinel is
  // bit-identical wide or narrow, so it passes through unconverted.
#if defined(_WIN32)
  char native_asm[4096];
  {
    std::size_t i = 0;
    for (; assembly_path[i] != '\0' && i + 1 < sizeof(native_asm); ++i) {
      native_asm[i] = (assembly_path[i] == '/') ? '\\' : assembly_path[i];
    }
    native_asm[i] = '\0';
  }
  wchar_t wasm[4096];
  wchar_t wtype[1024];
  wchar_t wmethod[1024];
  if (!NarrowToWide(native_asm, wasm, sizeof(wasm) / sizeof(wasm[0])) ||
      !NarrowToWide(type_name, wtype, sizeof(wtype) / sizeof(wtype[0])) ||
      !NarrowToWide(method_name, wmethod,
                    sizeof(wmethod) / sizeof(wmethod[0]))) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "assembly/type/method not convertible to UTF-16");
    LogBoot("bind FAILED", impl->error);
    return nullptr;
  }
  // impl->load_fn holds the delegate address (type-erased): the true
  // signature on Windows is LoadAssemblyFnW.
  auto load_w = reinterpret_cast<LoadAssemblyFnW>(  // NOLINT
      reinterpret_cast<void*>(impl->load_fn));     // NOLINT
  const int rc = load_w(wasm, wtype, wmethod,
                        reinterpret_cast<const void*>(  // NOLINT
                            kUnmanagedCallersOnly),
                        nullptr, &fn);
#else
  const int rc = impl->load_fn(assembly_path, type_name, method_name,
                               kUnmanagedCallersOnly, nullptr, &fn);
#endif
  if (rc != 0 || fn == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error),
                  "load_assembly_and_get_function_pointer rc=%d", rc);
    LogBoot("bind FAILED", impl->error);
    return nullptr;
  }
  return fn;
}

}  // namespace engine::bridge
