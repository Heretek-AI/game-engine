// skeletal_import.hpp — P5-01: glTF skins + animation clips -> .pak blobs.
//
// Import-time conversion (host tooling, background lane): walks the skin /
// animation lists of an already-parsed cgltf_data and fills plain POD
// records. Serialization helpers emit the versioned little-endian
// `.skel` / `.anim` artifact layouts (below); the reader side (P5-02)
// parses them back with bounds checks.
//
// Layouts (all integers LE, floats IEEE-754 LE):
//   .skel  magic "ENGS" u32 version(=1) u32 skin_count
//     per skin: u32 joint_count
//       per joint: i32 parent_index (-1 = root joint) f32 inverse_bind[16]
//       u32 name_len + name bytes (UTF-8, no NUL; empty = unnamed)
//   .anim  magic "ENGA" u32 version(=1) u32 clip_count
//     per clip: u32 name_len + name, f32 duration, u32 channel_count
//       per channel: u32 target_joint u8 target_path (0=T,1=R,2=S —
//         cgltf_animation_path_type enum order: invalid/translation/
//         rotation/scale/weights; weights channels are skipped at import)
//         u8 interpolation (0=linear,1=step,2=cubicspline —
//         cgltf_interpolation_type enum order) u16 reserved(0)
//         u32 key_count f32 times[key_count]
//         f32 values[key_count * component_count] (T/S=3, R=4; cubicspline
//         stores in/out/value tangents interleaved per glTF)
//
// Header-only + cgltf-fwd-declared: including this header does NOT need
// cgltf.h (the TU that calls the import includes cgltf.h itself).
// Compiles SDK-free; no Vulkan; the frame path never touches this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/assets/engine_assets_export.hpp"

// Forward declarations (cgltf v1.15 layout; definitions live in cgltf.h,
// included by the calling TU, never here).
struct cgltf_data;

namespace engine::assets {

// One joint in bind pose: parent index into the same skin's joint array
// (-1 for a root joint), inverse bind matrix (column-major, glTF order),
// joint node name (may be empty).
struct SkeletalJoint {
  std::int32_t parent_index = -1;
  float inverse_bind[16] = {};
  std::string name;
};

// One skin: joints resolve to cgltf_node* at import; parents are joint
// indices (-1 = root of this skin's hierarchy). Empty name = unnamed.
struct SkeletalSkin {
  std::string name;
  std::vector<SkeletalJoint> joints;
};

// Target path / interpolation mirror the cgltf enum order so the importer
// can static_cast directly.
enum class AnimTargetPath : std::uint8_t {
  kTranslation = 1,
  kRotation = 2,
  kScale = 3,
};

enum class AnimInterpolation : std::uint8_t {
  kLinear = 0,
  kStep = 1,
  kCubicSpline = 2,
};

// One animated node-property track. times[] ascending seconds;
// values[] packed: translation/scale = 3 floats/key, rotation = 4
// (x,y,z,w); cubicspline = 3x that (in-tangent, value, out-tangent).
struct AnimChannel {
  std::uint32_t target_joint = 0;  // index into the clip's skin joint array
  AnimTargetPath target_path = AnimTargetPath::kTranslation;
  AnimInterpolation interpolation = AnimInterpolation::kLinear;
  std::vector<float> times;
  std::vector<float> values;
};

// One clip: channels target joints of skin_index (first skin when the
// source has exactly one; see ImportAnimationClips).
struct AnimClip {
  std::string name;
  float duration = 0.0F;
  std::uint32_t skin_index = 0;
  std::vector<AnimChannel> channels;
};

// Import all skins in `data` (already parsed + buffers loaded).
// Appends one SkeletalSkin per cgltf skin in index order; a file with no
// skins appends none (valid: static mesh). Null args -> false.
// Corrupt skin (null joint, bad IBM accessor, non-MAT4 IBM) -> false with
// no partial append of that skin.
[[nodiscard]] auto ENGINE_ASSETS_API
ImportSkeletalSkins(const cgltf_data* data, std::vector<SkeletalSkin>* out)
    -> bool;

// Import all animation clips in `data`. target_joint indices are resolved
// against skin `skin_index` (default 0): channels whose target node is not
// a joint of that skin are skipped; clips left with zero channels are
// skipped. A file with no animations appends none (valid). Null args ->
// false; unsupported channel data (non-float accessors, weights path,
// unknown interpolation) skips that channel, never fails the clip.
// Returns false only on null args or corrupt sampler accessors.
[[nodiscard]] auto ENGINE_ASSETS_API ImportAnimationClips(
    const cgltf_data* data, std::vector<AnimClip>* out,
    std::uint32_t skin_index = 0) -> bool;

// Serialize one-or-more skins / clips to the artifact blob. Empty input
// still emits a valid header with count 0.
[[nodiscard]] auto ENGINE_ASSETS_API
WriteSkeletonArtifact(const std::vector<SkeletalSkin>& skins,
                      std::vector<std::uint8_t>* out) -> bool;
[[nodiscard]] auto ENGINE_ASSETS_API
WriteAnimationArtifact(const std::vector<AnimClip>& clips,
                       std::vector<std::uint8_t>* out) -> bool;

// Parse an artifact blob back (bounds-checked, fails closed). Null args
// or bad magic/version/truncation -> false, `out` untouched.
[[nodiscard]] auto ENGINE_ASSETS_API
ReadSkeletonArtifact(const std::uint8_t* data, std::size_t size,
                     std::vector<SkeletalSkin>* out) -> bool;
[[nodiscard]] auto ENGINE_ASSETS_API
ReadAnimationArtifact(const std::uint8_t* data, std::size_t size,
                      std::vector<AnimClip>* out) -> bool;

}  // namespace engine::assets
