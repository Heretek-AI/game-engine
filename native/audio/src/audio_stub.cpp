// Placeholder TU so `engine_core_audio` OBJECT lib is never empty.
// Real audio graph lands in M4 (M4-04: miniaudio spatial graph).
namespace engine::audio::stub {
auto placeholder() -> int { return 0; }
}  // namespace engine::audio::stub
