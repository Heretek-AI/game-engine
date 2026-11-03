// anim_blending.hpp — P5-02: two-clip pose blending + joint-matrix upload.
//
// Blends two sampled AnimPose TRS poses (see anim_sampling.hpp) into one
// caller-owned output pose, and flattens a blended pose into joint matrices
// (joint-local TRS composed with the skin hierarchy, times inverse bind)
// ready to upload into the mesh.vert joint-palette UBO (gpu_mesh.hpp).
//
// Frame-path rules (AGENTS.md): noexcept throughout, no malloc/new on the
// blend path — every buffer is caller-owned (AnimPoseReset-style). Weight
// outside [0,1] clamps. Non-finite weight fails closed (out untouched).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/assets/anim_sampling.hpp"
#include "engine/assets/engine_assets_export.hpp"

namespace engine::assets {

// Blend poses `a` and `b` into `out` (caller-owned, joint_count joints):
//   out = a * (1 - w) + b * w — slerp for rotation, lerp for T/S.
// `set[]` is OR-ed (joint marked when either input marks it); unmarked
// joints still blend (both sides identity-reset on miss). Reads all three
// poses' joint_count as the minimum (ragged inputs blend the overlap).
// Null arrays, joint_count 0, or non-finite w leave `out` untouched.
ENGINE_ASSETS_API auto BlendAnimPoses(const AnimPose& a, const AnimPose& b,
                                      float weight, AnimPose out) noexcept
    -> void;

// Flatten blended pose `pose` into column-major joint matrices `out`
// (joint_count x 16 floats, caller-owned): local = T*R*S per joint,
// world = local composed parent-first via `parents` (parent_index per
// joint, -1 = root; forward/out-of-range parents are treated as roots),
// out = world * inverse_bind[j]. `inverse_bind` holds joint_count x 16
// column-major floats (null = identity). Returns the joint count written;
// fails closed (0, out untouched) on null arrays, joint_count 0, or a null
// `parents` pointer.
ENGINE_ASSETS_API auto PoseToJointMatrices(const AnimPose& pose,
                                           const std::int32_t* parents,
                                           const float* inverse_bind,
                                           float* out,
                                           std::size_t joint_count) noexcept
    -> std::size_t;

}  // namespace engine::assets
