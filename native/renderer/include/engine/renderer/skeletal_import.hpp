// skeletal_import.hpp — P5-01: cgltf skins + animation clips -> .pak blobs.
//
// Import-time conversion (host tooling, background lane): walks the
// cgltf_skin list of an already-parsed cgltf_data and fills one
// SkeletalSkeleton per skin (joints, hierarchy, inverse bind matrices),
// plus one SkeletalClip per cgltf_animation (channels with keyframe
// times + values). SerializeSkeleton/SerializeClip pack the exact
// little-endian ENGS/ENGA layouts the .pak stores; the P5-02 playback
// slice parses these blobs back.
//
// Header-only + cgltf-fwd-declared: including this header does NOT need
// cgltf.h (the TU that calls the import includes cgltf.h itself).
// Compiles SDK-free; no Vulkan, no allocation on success beyond the
// caller's vectors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#include "engine/renderer/engine_status.hpp"

// Forward declarations (cgltf v1.15 layout; definitions live in cgltf.h,
// included by the calling TU, never here).
struct cgltf_data;
struct cgltf_skin;
struct cgltf_animation;

namespace engine::renderer {

// Caps: import rejects larger inputs (kInvalidArgument, no partial
// append). Host tooling, generous but finite.
inline constexpr std::size_t kSkeletalMaxJoints = 256;
inline constexpr std::size_t kSkeletalMaxName = 64;
inline constexpr std::size_t kSkeletalMaxClips = 256;
inline constexpr std::size_t kSkeletalMaxChannelsPerClip = 1024;
inline constexpr std::size_t kSkeletalMaxKeysPerChannel = 65536;

// Animation target path (mirrors cgltf_animation_path_type, stored u8).
enum class SkeletalPath : std::uint8_t {
  kTranslation = 0,
  kRotation = 1,
  kScale = 2,
};

// Sampler interpolation (mirrors cgltf_interpolation_type, stored u8).
enum class SkeletalInterpolation : std::uint8_t {
  kLinear = 0,
  kStep = 1,
  kCubicSpline = 2,
};

// One joint: node index into the glTF node list (u32, kNoneJoint when the
// importer cannot resolve it — never emitted, import rejects instead),
// parent joint index (kNoneJoint for roots), fixed-size name, and the
// inverse bind matrix in glTF column-major order (16 floats).
inline constexpr std::uint32_t kNoneJoint = 0xFFFFFFFFu;
struct SkeletalJoint {
  std::uint32_t node_index = kNoneJoint;
  std::uint32_t parent_joint = kNoneJoint;
  std::array<char, kSkeletalMaxName> name{};
  std::array<float, 16> inverse_bind{};
};

// One skeleton: joints in cgltf skin.joints order.
struct SkeletalSkeleton {
  std::vector<SkeletalJoint> joints;
};

// One animation channel: which joint it drives, which path, how keys
// interpolate, keyframe times (seconds, strictly per glTF input accessor)
// and values (translation/scale: 3 floats per key; rotation: 4 floats
// per key in glTF (x,y,z,w) order; cubic spline: 3x floats per key
// in-tangent/value/out-tangent triples — stored verbatim).
struct SkeletalChannel {
  std::uint32_t joint = kNoneJoint;
  SkeletalPath path = SkeletalPath::kTranslation;
  SkeletalInterpolation interpolation = SkeletalInterpolation::kLinear;
  std::vector<float> times;
  std::vector<float> values;
};

// One clip: channels in cgltf channels order, duration = max key time.
struct SkeletalClip {
  std::array<char, kSkeletalMaxName> name{};
  float duration = 0.0f;
  std::vector<SkeletalChannel> channels;
};

// .pak blob magics ("ENGS" skeleton, "ENGA" animation clip), u32
// version (=1) each.
inline constexpr std::uint32_t kSkeletalMagic = 0x53474E45u;  // "ENGS"
inline constexpr std::uint32_t kSkeletalClipMagic = 0x41474E45u;  // "ENGA"
inline constexpr std::uint32_t kSkeletalVersion = 1;

// ENGS layout (little-endian):
//   magic u32 = kSkeletalMagic, version u32 = 1, joint_count u32,
//   per joint: node_index u32, parent_joint u32, name[64] bytes,
//              inverse_bind[16] f32 (column-major, glTF order).
// ENGA layout (little-endian):
//   magic u32 = kSkeletalClipMagic, version u32 = 1, duration f32,
//   channel_count u32,
//   per channel: joint u32, path u8, interpolation u8, reserved u16,
//                key_count u32, value_floats u32,
//                times[key_count] f32, values[value_floats] f32.
[[nodiscard]] inline auto SkeletalNameView(
    const std::array<char, kSkeletalMaxName>& name) noexcept
    -> std::string_view {
  std::size_t len = 0;
  while (len < name.size() && name[len] != '\0') {
    ++len;
  }
  return {name.data(), len};
}

// Serialize one skeleton / clip into its .pak blob. Null out -> kInvalidArgument.
auto ENGINE_CORE_API SerializeSkeleton(const SkeletalSkeleton* skeleton,
                                       std::vector<std::uint8_t>* out)
    -> Status;
auto ENGINE_CORE_API SerializeClip(const SkeletalClip* clip,
                                   std::vector<std::uint8_t>* out) -> Status;

// Convert all skins in `data` (already parsed + buffers loaded).
// Appends one SkeletalSkeleton per cgltf skin in index order. Null
// data/out -> kInvalidArgument; any corrupt skin (over-cap joints,
// unresolved joint node, bad inverse-bind accessor) -> kInvalidArgument
// with no partial append.
auto ENGINE_CORE_API SkeletalImportSkeletons(const cgltf_data* data,
                                             std::vector<SkeletalSkeleton>* out)
    -> Status;

// Convert a single skin (index into data->skins). OOB index or null
// args -> kInvalidArgument.
auto ENGINE_CORE_API SkeletalImportSkeleton(const cgltf_data* data,
                                            std::size_t skin_index,
                                            SkeletalSkeleton* out) -> Status;

// Convert all animations in `data`. Appends one SkeletalClip per
// cgltf animation in index order. Same null/corrupt contract as skins;
// corrupt = over-cap channels/keys, unknown target path, unresolvable
// target node, non-float accessors, empty time input.
auto ENGINE_CORE_API SkeletalImportClips(const cgltf_data* data,
                                         std::vector<SkeletalClip>* out)
    -> Status;

// Convert a single animation (index into data->animations). OOB index
// or null args -> kInvalidArgument.
auto ENGINE_CORE_API SkeletalImportClip(const cgltf_data* data,
                                        std::size_t anim_index,
                                        SkeletalClip* out) -> Status;

}  // namespace engine::renderer
