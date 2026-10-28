// dotnet_host.cpp — M1-05 implementation; see dotnet_host.hpp.
//
// Resolution order for libhostfxr (first hit wins, all noexcept/dlerror):
//   1. DOTNET_ROOT env (host/fxr/<ver>/libhostfxr.so) — hermetic CI/dev.
//   2. nethost get_hostfxr_path (linked statically from the .NET pack).
// Failure anywhere -> create() returns nullptr; the Benchmark1M runner
// reports which stage failed (no silent fallback to a wrong runtime).
#include "engine/bridge/dotnet_host.hpp"

#include <dirent.h>
#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// nethost ships as a static lib in the Microsoft.NETCore.App.Host pack;
// link it via the bridge target's compile include + link lib below. When
// the pack is unavailable (offline configure), fall back to a minimal local
// declaration of get_hostfxr_path — the implementation lives in
// libnethost.a / the runtime fallback in ScanFxDir still applies.
#if __has_include("nethost.h")
#include "nethost.h"
#else
using char_t = char;
struct get_hostfxr_parameters {
  size_t size;
  const char* assembly_path;
  const char* dotnet_root;
};
extern "C" int get_hostfxr_path(char_t* buffer, size_t* buffer_size,
                                const struct get_hostfxr_parameters* parameters);
#ifndef NETHOST_USE_AS_STATIC
#define NETHOST_USE_AS_STATIC
#endif
#endif

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

constexpr int kDelegateLoadAssembly = 5;  // hdt_load_assembly... (hostfxr.h)

// UNMANAGEDCALLERSONLY_METHOD sentinel (coreclr_delegates.h): tells the
// loader the target is [UnmanagedCallersOnly] so no delegate type is needed.
// (-1 as a pointer is not a constant expression under -Werror, so this is
// a plain const, not constexpr.)
const char* const kUnmanagedCallersOnly =
    reinterpret_cast<const char*>(static_cast<uintptr_t>(-1));  // NOLINT

struct HostImpl {
  void* fxr{nullptr};
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
auto HostfxrErrorWriter(const char* msg) noexcept -> void {
  std::fprintf(stderr, "[hostfxr] %s\n", msg != nullptr ? msg : "(null)");
}

auto InstallErrorWriter(void* fxr) noexcept -> void {
  using SetWriterFn = void* (*)(void (*)(const char*));
  auto set_writer = reinterpret_cast<SetWriterFn>(  // NOLINT
      dlsym(fxr, "hostfxr_set_error_writer"));
  if (set_writer == nullptr) {
    return;
  }
  set_writer(&HostfxrErrorWriter);
}

auto TryDlopen(const char* path, HostImpl* impl) noexcept -> bool {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (h == nullptr) {
    std::snprintf(impl->error, sizeof(impl->error), "%.200s", dlerror());
    return false;
  }
  impl->fxr = h;
  InstallErrorWriter(h);
  return true;
}

// DOTNET_ROOT/host/fxr/<ver>/libhostfxr.so: scan the fxr dir for the
// lexicographically greatest version entry (stable SemVer sorts correctly).
auto ScanFxDir(HostImpl* impl) noexcept -> bool {
  const char* root = std::getenv("DOTNET_ROOT");
  if (root == nullptr || root[0] == '\0') {
    return false;
  }
  std::string fxr_dir = std::string(root) + "/host/fxr";
  DIR* d = opendir(fxr_dir.c_str());
  if (d == nullptr) {
    return false;
  }
  // Pick the lexicographically greatest version dir (SemVer sorts
  // correctly as plain strings for stable .NET releases: 10.0.14 wins).
  std::string best;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    std::string cand = fxr_dir + "/" + e->d_name + "/libhostfxr.so";
    FILE* probe = std::fopen(cand.c_str(), "rb");
    if (probe != nullptr) {
      std::fclose(probe);
      if (best.empty() || cand > best) {
        best = cand;
      }
    }
  }
  closedir(d);
  if (best.empty()) {
    return false;
  }
  LogBoot("hostfxr candidate", best.c_str());
  return TryDlopen(best.c_str(), impl);
}

auto TryNethost(HostImpl* impl) noexcept -> bool {
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
}

auto BindAndInit(HostImpl* impl, const char* runtime_config_path) noexcept
    -> bool {
  auto init = reinterpret_cast<HostfxrInitForConfigFn>(  // NOLINT
      dlsym(impl->fxr, "hostfxr_initialize_for_runtime_config"));
  auto get_del = reinterpret_cast<HostfxrGetDelegateFn>(  // NOLINT
      dlsym(impl->fxr, "hostfxr_get_runtime_delegate"));
  auto close_fn = reinterpret_cast<HostfxrCloseFn>(  // NOLINT
      dlsym(impl->fxr, "hostfxr_close"));
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
  const int rc = init(runtime_config_path, nullptr, &ctx);
  if (rc != 0 || ctx == nullptr) {
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
        dlsym(impl->fxr, "hostfxr_close"));
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
  if (dotnet_root != nullptr && dotnet_root[0] != '\0') {
    (void)setenv("DOTNET_ROOT", dotnet_root, 1);
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
      dlclose(impl->fxr);
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
        dlsym(impl->fxr, "hostfxr_close"));
    if (close != nullptr) {
      close(impl->ctx);
    }
  }
  if (impl->fxr != nullptr) {
    dlclose(impl->fxr);
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
