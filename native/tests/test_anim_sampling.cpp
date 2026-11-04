// test_anim_sampling.cpp — P5-02 acceptance: CPU clip sampling.
//
// Covers STEP, LINEAR (translation lerp + rotation slerp incl. clamps and
// edges) and CUBIC_SPLINE (Hermite against hand-computed values), plus
// fail-closed gates (null/empty/malformed/NaN-time/out-of-range joint) and
// a cross-check against the P5-01 two-bone fixture values.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "engine/assets/anim_sampling.hpp"

#include "anim_test_helpers.hpp"

namespace {

using engine::test::anim::check;
using engine::test::anim::Near;
using engine::test::anim::NearArr;

static constexpr float kS45 = 0.70710678F;  // sin(45deg): 90deg-about-Y key
static constexpr float kS = 0.70710678F;    // P5-01 fixture mid key
static constexpr float kWant0[] = {0, kS45, 0, kS45};
static constexpr float kWant1[] = {0.15625F, 0, 0};
static constexpr float kWant2[] = {0.5F, 1, 1.5F};
static constexpr float kWant3[] = {0.25F, 0, 0};
static constexpr float kWant4[] = {0, 0, 0, 1};
static constexpr float kWant5[] = {0.5F, 0, 0};
static constexpr float kWant6[] = {0, kS, 0, kS};
static constexpr float kWant7[] = {1, 2, 3};
static constexpr float kWant8[] = {1, 0, 0};
static constexpr float kWant9[] = {1, 1, 1};
static constexpr float kWant10[] = {0, 1, 0};
static constexpr float kWant11[] = {2, 4, 6};
static constexpr float kWant12[] = {0, 0, 0};

using engine::assets::AnimChannel;
using engine::assets::AnimClip;
using engine::assets::AnimInterpolation;
using engine::assets::AnimPose;
using engine::assets::AnimPoseReset;
using engine::assets::AnimTargetPath;
using engine::assets::SampleAnimChannel;
using engine::assets::SampleAnimClip;

auto MakeChannel(AnimTargetPath path, AnimInterpolation interp,
                 std::vector<float> times, std::vector<float> values,
                 std::uint32_t joint = 0) -> AnimChannel {
  AnimChannel ch;
  ch.target_joint = joint;
  ch.target_path = path;
  ch.interpolation = interp;
  ch.times = std::move(times);
  ch.values = std::move(values);
  return ch;
}

}  // namespace

auto main() -> int {
  int failures = 0;

  // --- STEP translation -----------------------------------------------------
  {
    AnimChannel ch = MakeChannel(AnimTargetPath::kTranslation,
                                 AnimInterpolation::kStep, {0.0F, 1.0F},
                                 {0, 0, 0, 0, 1, 0});
    float out[3] = {9, 9, 9};
    failures += check(SampleAnimChannel(ch, -1.0F, out) &&
                          NearArr(out, kWant12, 3),
                      "step clamps before first");
    failures += check(SampleAnimChannel(ch, 0.0F, out) &&
                          NearArr(out, kWant12, 3),
                      "step holds key0 at t=0");
    failures += check(SampleAnimChannel(ch, 0.999F, out) &&
                          NearArr(out, kWant12, 3),
                      "step holds key0 just before t=1");
    failures += check(SampleAnimChannel(ch, 1.0F, out) &&
                          NearArr(out, kWant10, 3),
                      "step takes key1 at t=1");
    failures += check(SampleAnimChannel(ch, 5.0F, out) &&
                          NearArr(out, kWant10, 3),
                      "step clamps after last");
  }

  // --- LINEAR translation ---------------------------------------------------
  {
    AnimChannel ch = MakeChannel(AnimTargetPath::kTranslation,
                                 AnimInterpolation::kLinear, {0.0F, 2.0F},
                                 {0, 0, 0, 2, 4, 6});
    float out[3] = {0, 0, 0};
    failures += check(SampleAnimChannel(ch, 1.0F, out) &&
                          NearArr(out, kWant7, 3),
                      "linear lerps translation midpoint");
    failures += check(SampleAnimChannel(ch, 0.5F, out) &&
                          NearArr(out, kWant2, 3),
                      "linear lerps translation quarter");
    failures += check(SampleAnimChannel(ch, -3.0F, out) &&
                          NearArr(out, kWant12, 3),
                      "linear clamps translation before first");
    failures += check(SampleAnimChannel(ch, 99.0F, out) &&
                          NearArr(out, kWant11, 3),
                      "linear clamps translation after last");
  }

  // --- LINEAR rotation slerp -----------------------------------------------
  // Identity -> 90deg-about-Y at t=0/1: midpoint is 45deg-about-Y.
  {
    const float s45_local = 0.70710678F;  // sin(45deg)
    const float s225_local = 0.38268343F;  // sin(22.5deg)
    const float c225_local = 0.92387953F;  // cos(22.5deg)
    AnimChannel ch = MakeChannel(
        AnimTargetPath::kRotation, AnimInterpolation::kLinear, {0.0F, 1.0F},
        {0, 0, 0, 1, 0, s45_local, 0, s45_local});
    float out[4] = {0, 0, 0, 0};
    const float want_mid[4] = {0, s225_local, 0, c225_local};
    failures += check(SampleAnimChannel(ch, 0.5F, out) &&
                          NearArr(out, want_mid, 4, 1e-4F),
                      "linear slerps rotation midpoint (45deg Y)");
    float n2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2] +
               out[3] * out[3];
    failures += check(Near(n2, 1.0F), "slerp output is unit length");
    failures += check(SampleAnimChannel(ch, 0.0F, out) &&
                          NearArr(out, kWant4, 4),
                      "linear clamps rotation at t=0");
    failures += check(SampleAnimChannel(ch, 1.0F, out) &&
                          NearArr(out, kWant0, 4),
                      "linear reaches end key at t=1");
    // Multi-segment: midpoint of the second segment.
    AnimChannel ch2 = MakeChannel(
        AnimTargetPath::kRotation, AnimInterpolation::kLinear,
        {0.0F, 0.5F, 1.0F},
        {0, 0, 0, 1, 0, s45_local, 0, s45_local, 0, 0, 0, 1});
    failures += check(SampleAnimChannel(ch2, 0.75F, out) &&
                          NearArr(out, want_mid, 4, 1e-4F),
                      "linear picks the right segment (t=0.75)");
  }

  // --- CUBIC_SPLINE translation ---------------------------------------------
  // Keys (0 -> 1) on x with zero tangents: Hermite(0,1,0,0,u) with
  // u=0.25 -> 0.15625.
  {
    AnimChannel ch = MakeChannel(
        AnimTargetPath::kTranslation, AnimInterpolation::kCubicSpline,
        {0.0F, 1.0F},
        {// key0: in-tan, value, out-tan
         9, 9, 9, 0, 0, 0, 0, 0, 0,
         // key1: in-tan, value, out-tan
         0, 0, 0, 1, 0, 0, 7, 7, 7});
    float out[3] = {0, 0, 0};
    failures +=
        check(SampleAnimChannel(ch, 0.25F, out) &&
                  NearArr(out, kWant1, 3),
              "cubicspline hermite zero tangents u=0.25");
    failures += check(SampleAnimChannel(ch, 0.0F, out) &&
                          NearArr(out, kWant12, 3),
                      "cubicspline clamps to key0 value");
    failures += check(SampleAnimChannel(ch, 1.0F, out) &&
                          NearArr(out, kWant8, 3),
                      "cubicspline clamps to key1 value");
    // Non-zero tangents: p0=0,p1=0,m0=1,m1=-1,dt=1,u=0.5 ->
    // h10(0.5)=0.125, h11(0.5)=-0.125 -> 0.125*1 + -0.125*-1 = 0.25.
    AnimChannel ch2 = MakeChannel(
        AnimTargetPath::kTranslation, AnimInterpolation::kCubicSpline,
        {0.0F, 1.0F},
        {0, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    failures += check(SampleAnimChannel(ch2, 0.5F, out) &&
                          NearArr(out, kWant3, 3),
                      "cubicspline honors tangents (dt-scaled)");
    // dt scaling: same tangents over [0,2] at t=1 -> m*dt doubles slope
    // contribution: 0.125*2 + -0.125*-2 = 0.5.
    AnimChannel ch3 = MakeChannel(
        AnimTargetPath::kTranslation, AnimInterpolation::kCubicSpline,
        {0.0F, 2.0F},
        {0, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    failures += check(SampleAnimChannel(ch3, 1.0F, out) &&
                          NearArr(out, kWant5, 3),
                      "cubicspline scales tangents by dt");
  }

  // --- CUBIC_SPLINE rotation (normalizes) ------------------------------------
  {
    AnimChannel ch = MakeChannel(
        AnimTargetPath::kRotation, AnimInterpolation::kCubicSpline,
        {0.0F, 1.0F},
        {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    float out[4] = {0, 0, 0, 0};
    failures += check(SampleAnimChannel(ch, 0.5F, out) &&
                          NearArr(out, kWant4, 4),
                      "cubicspline identity rotation stays identity");
    float n2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2] +
               out[3] * out[3];
    failures += check(Near(n2, 1.0F), "cubicspline rotation is unit length");
  }

  // --- fail-closed gates ------------------------------------------------------
  {
    float out[4] = {1, 2, 3, 4};
    AnimChannel empty;
    failures += check(!SampleAnimChannel(empty, 0.0F, out),
                      "empty channel fails");
    failures += check(out[0] == 1 && out[1] == 2 && out[2] == 3 &&
                          out[3] == 4,
                      "failed sample leaves out untouched");
    AnimChannel good = MakeChannel(AnimTargetPath::kTranslation,
                                   AnimInterpolation::kLinear, {0.0F, 1.0F},
                                   {0, 0, 0, 1, 1, 1});
    failures += check(!SampleAnimChannel(good, 0.5F, nullptr),
                      "null out fails");
    const float nan = std::nanf("");
    failures += check(!SampleAnimChannel(good, nan, out),
                      "NaN time fails closed");
    failures += check(!SampleAnimChannel(good, std::numeric_limits<float>::infinity(), out),
                      "infinite time fails closed");
    AnimChannel bad_count = MakeChannel(AnimTargetPath::kTranslation,
                                        AnimInterpolation::kLinear,
                                        {0.0F, 1.0F}, {0, 0, 0});
    failures += check(!SampleAnimChannel(bad_count, 0.5F, out),
                      "value count mismatch fails");
    AnimChannel bad_spline = MakeChannel(
        AnimTargetPath::kTranslation, AnimInterpolation::kCubicSpline,
        {0.0F, 1.0F}, {0, 0, 0, 0, 0, 0});
    failures += check(!SampleAnimChannel(bad_spline, 0.5F, out),
                      "cubicspline non-3x payload fails");
  }

  // --- P5-01 fixture cross-check ----------------------------------------------
  // wave clip: joint1 rotation LINEAR identity->90degY->identity at
  // t=0/0.5/1, joint1 translation STEP +Y at t=0/1.
  {
    const float s_fixture = 0.70710678F;
    AnimClip clip;
    clip.name = "wave";
    clip.duration = 1.0F;
    clip.channels.push_back(MakeChannel(
        AnimTargetPath::kRotation, AnimInterpolation::kLinear,
        {0.0F, 0.5F, 1.0F},
        {0, 0, 0, 1, 0, s_fixture, 0, s_fixture, 0, 0, 0, 1}, 1));
    clip.channels.push_back(MakeChannel(AnimTargetPath::kTranslation,
                                        AnimInterpolation::kStep,
                                        {0.0F, 1.0F}, {0, 0, 0, 0, 1, 0},
                                        1));
    float t[6] = {9, 9, 9, 9, 9, 9};
    float r[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    float sc[6] = {9, 9, 9, 9, 9, 9};
    std::uint8_t set[2] = {9, 9};
    AnimPose pose{t, r, sc, set, 2};
    const std::size_t n = SampleAnimClip(clip, 0.5F, pose);
    failures += check(n == 1, "wave clip sets exactly joint1");
    failures +=
        check(set[0] == 0 && set[1] == 1, "wave clip set flags joint-major");
    failures += check(NearArr(r + 4, kWant6, 4),
                      "wave rotation at t=0.5 is 90deg Y");
    failures += check(NearArr(t + 3, kWant12, 3),
                      "wave translation at t=0.5 holds key0 (STEP)");
    failures += check(NearArr(t, kWant12, 3),
                      "unset joint0 translation is identity");
    failures += check(NearArr(r, kWant4, 4),
                      "unset joint0 rotation is identity");
    failures += check(NearArr(sc, kWant9, 3),
                      "unset joint0 scale is identity");
    // t=1: translation takes key1 (+Y), rotation returns to identity.
    const std::size_t n1 = SampleAnimClip(clip, 1.0F, pose);
    failures += check(n1 == 1, "wave clip at t=1 still sets joint1");
    failures += check(NearArr(t + 3, kWant10, 3),
                      "wave translation at t=1 is +Y");
    failures += check(NearArr(r + 4, kWant4, 4),
                      "wave rotation at t=1 returns to identity");
    // Out-of-range joint is skipped, malformed channel is skipped.
    AnimClip bad = clip;
    AnimChannel oob = MakeChannel(AnimTargetPath::kTranslation,
                                  AnimInterpolation::kLinear, {0.0F},
                                  {5, 5, 5}, 7);
    bad.channels.push_back(oob);
    AnimChannel malformed = MakeChannel(AnimTargetPath::kTranslation,
                                        AnimInterpolation::kLinear,
                                        {0.0F, 1.0F}, {0, 0, 0}, 0);
    bad.channels.push_back(malformed);
    const std::size_t nb = SampleAnimClip(bad, 0.5F, pose);
    failures += check(nb == 1, "oob + malformed channels skipped");
    // Null pose arrays / non-finite time fail closed, pose untouched.
    AnimPose null_pose{nullptr, nullptr, nullptr, nullptr, 2};
    failures += check(SampleAnimClip(clip, 0.5F, null_pose) == 0,
                      "null pose arrays return 0");
    const float nan = std::nanf("");
    t[3] = 42.0F;
    failures += check(SampleAnimClip(clip, nan, pose) == 0,
                      "NaN time returns 0");
    failures += check(t[3] == 42.0F, "NaN time leaves pose untouched");
    // AnimPoseReset alone.
    float rt[3] = {5, 5, 5};
    float rr[4] = {1, 2, 3, 4};
    float rs[3] = {0, 0, 0};
    std::uint8_t rset[1] = {1};
    AnimPoseReset(AnimPose{rt, rr, rs, rset, 1});
    failures += check(NearArr(rt, kWant12, 3) &&
                          NearArr(rr, kWant4, 4) &&
                          NearArr(rs, kWant9, 3) &&
                          rset[0] == 0,
                      "AnimPoseReset writes identity");
  }

  if (failures == 0) {
    std::printf("ANIM_SAMPLING_OK\n");
  } else {
    std::printf("ANIM_SAMPLING_FAIL (%d)\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
