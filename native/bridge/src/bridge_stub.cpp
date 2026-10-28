// Placeholder TU so `engine_core_bridge` OBJECT lib is never empty
// (CMake rejects add_library with zero sources). Real ABI boundary lands
// in M1-04 (native/bridge/engine_abi.h); hostfxr host in M1-05.
namespace engine::bridge::stub {
auto placeholder() -> int { return 0; }
}  // namespace engine::bridge::stub
