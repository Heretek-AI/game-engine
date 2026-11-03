// anim_sampling.hpp — P5-02: CPU clip sampling (linear / step / cubic-spline).
//
// Samples P5-01 AnimClip channels into caller-owned joint-local TRS poses.
// glTF-style semantics per channel interpolation mode:
//
//   STEP:         hold key i's value for t in [t_i, t_{i+1}).
//   LINEAR:      lerp for T/S, normalized slerp for R.
//   CUBIC_SPLINE: Hermite over the [t_i, t_{i+1}] segment with the stored
//                 out-tangent of key i and in-tangent of key i+1, per the
//                 glTF 2.0 sampler spec (tangents are absolute slopes:
//                 m_k * dt scales them into Hermite form). Rotation tangents
//                 interpolate in tangent space and the result normalizes.
//
// Frame-path rules (AGENTS.md): noexcept throughout, no malloc/new on the
// sample path — the caller owns every buffer (AnimPose::Reset writes into
// caller memory only). Quaternions normalize on output; a zero-length input
// yields identity rather than NaN.
//
// Edge semantics: t <= times[0] returns key 0; t >= times[last] returns the
// last key. Empty channels (zero keys) leave the joint's reset value
// untouched and report it as unset. Non-finite `t` fails closed (returns 0
// joints and leaves the pose untouched). Per-key NaN checks are the
// importer's job (P5-01 key rules); the sampler guards only segment math
// (zero/negative dt, zero-length quats).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/assets/engine_assets_export.hpp"
#include "engine/assets/skeletal_import.hpp"

namespace engine::assets {

// Joint-local TRS pose: joint-major flat arrays, caller-allocated for
// `joint_count` joints. `set[i] != 0` when joint i got at least one channel.
// Layout: translation[i*3+k], rotation[i*4+k] (x,y,z,w), scale[i*3+k].
struct AnimPose {
  float* translation = nullptr;  // [joint_count * 3], may be null
  float* rotation = nullptr;     // [joint_count * 4], may be null
  float* scale = nullptr;        // [joint_count * 3], may be null
  std::uint8_t* set = nullptr;   // [joint_count], may be null
  std::size_t joint_count = 0;
};

// Fill every joint with identity TRS (T=0, R=(0,0,0,1), S=1) and clear `set`.
// Null arrays are skipped; a null pose or joint_count 0 is a no-op.
ENGINE_ASSETS_API auto AnimPoseReset(AnimPose pose) noexcept -> void;

// Sample one channel at time `t` (seconds) into `out` (3 floats for T/S,
// 4 floats (x,y,z,w) for R). Returns false (out untouched) on null args,
// zero keys, key/value count mismatch, non-finite t, or a cubicspline value
// blob that is not exactly 3x the key payload.
[[nodiscard]] ENGINE_ASSETS_API auto SampleAnimChannel(
    const AnimChannel& channel, float t, float* out) noexcept -> bool;

// Sample every channel of `clip` at time `t` into `pose` (identity-reset
// first via AnimPoseReset). Returns the number of joints with >= 1 sampled
// channel. Fails closed: null/empty clip, null pose arrays, or non-finite t
// leave the pose untouched and return 0. Channels with an out-of-range
// target_joint or a malformed payload are skipped; later channels overwrite
// earlier ones on the same joint+path (last wins, deterministic).
[[nodiscard]] ENGINE_ASSETS_API auto SampleAnimClip(const AnimClip& clip,
                                                   float t,
                                                   AnimPose pose) noexcept
    -> std::size_t;

}  // namespace engine::assets
