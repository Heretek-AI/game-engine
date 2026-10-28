// dotnet_host.hpp — M1-05: CoreCLR embedding via hostfxr (nethost).
//
// Owns one CoreCLR runtime instance inside libengine_core:
//   get_hostfxr_path (nethost) -> dlopen libhostfxr.so ->
//   hostfxr_initialize_for_runtime_config (Benchmark1M.runtimeconfig.json) ->
//   hostfxr_get_runtime_delegate (hdt_load_assembly_and_get_function_pointer).
//
// Threading: create/destroy/load on ONE thread (M1-05 serializes
// lifecycle); the loaded entry point may run managed systems on any thread.
// noexcept throughout: an exception crossing the C ABI is UB. No malloc/new
// on the frame path — the host is lifecycle-only; per-chunk callbacks never
// touch it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::bridge {

// Opaque CoreCLR host handle. Create with dotnet_host_create(), destroy
// with dotnet_host_destroy() (idempotent on nullptr).
struct DotNetHost;

// Handle to libhostfxr's load_assembly_and_get_function_pointer delegate.
// Matches coreclr_delegates.h (cdecl on non-Windows).
using LoadAssemblyFn = int (*)(const char* assembly_path, const char* type_name,
                               const char* method_name,
                               const char* delegate_type_name, void* reserved,
                               void** delegate);

// Create + initialize a host from a .runtimeconfig.json path. Returns
// nullptr on any failure (fxr missing, runtime missing, init failed).
// `dotnet_root` may be nullptr (hostfxr resolves the install itself).
// Exported from engine_core (Windows: the test binary links the DLL
// import lib; Linux/macOS: default visibility).
[[nodiscard]] ENGINE_CORE_API auto dotnet_host_create(const char* runtime_config_path,
                                      const char* dotnet_root) noexcept
    -> DotNetHost*;

// Shut down the host (hostfxr_close + dlclose). Idempotent on nullptr.
ENGINE_CORE_API auto dotnet_host_destroy(DotNetHost* host) noexcept -> void;

// Resolve the load_assembly_and_get_function_pointer delegate, or nullptr
// if the host failed to initialize. Valid until dotnet_host_destroy().
[[nodiscard]] ENGINE_CORE_API auto dotnet_host_load_fn(DotNetHost* host) noexcept
    -> LoadAssemblyFn;

// Load `assembly_path`, find static `type_name`.`method_name` marked
// [UnmanagedCallersOnly], and return its raw function pointer (or nullptr
// on failure). The managed method must be compatible with the UCO calling
// convention (load_assembly passes UNMANAGEDCALLERSONLY_METHOD as the
// delegate type).
[[nodiscard]] ENGINE_CORE_API auto dotnet_host_get_system_pointer(
    DotNetHost* host, const char* assembly_path, const char* type_name,
    const char* method_name) noexcept -> void*;

}  // namespace engine::bridge
