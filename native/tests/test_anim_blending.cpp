// test_anim_blending.cpp — P5-02 acceptance: two-clip blending + golden.
//
// Covers per-joint lerp (T/S) / slerp (R) blending by weight, the
// weight-0/1 endpoints, clamp + fail-closed gates, PoseToJointMatrices
// (hierarchy compose, inverse-bind multiply), and the slice golden:
// two two-bone clips sampled at t=0.5s, blended at w=0.5, flattened to
// joint matrices — joint 1 must equal the hand-derived TRS matrix.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/assets/anim_blending.hpp"
#include "engine/assets/anim_sampling.hpp"

#include "anim_test_helpers.hpp"

namespace {

using engine::test::anim::check;
using engine::test::anim::Near;
using engine::test::anim::NearArr;

using engine::assets::AnimChannel;
using engine::assets::AnimClip;
using engine::assets::AnimInterpolation;
using engine::assets::AnimPose;
using engine::assets::AnimPoseReset;
using engine::assets::BlendAnimPoses;
using engine::assets::PoseToJointMatrices;
using engine::assets::SampleAnimClip;
using engine::assets::AnimTargetPath;

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

// Two-bone clip: joint 1 rotates, joint 0 untouched. `end` is the
// joint-1 rotation key at t=1 (x,y,z,w); the key at t=0 is identity.
// Sampled LINEAR at t=0.5 this yields the halfway slerp.
auto TwoBoneRotClip(std::vector<float> end) -> AnimClip {
  AnimClip clip;
  clip.name = "clip";
  clip.duration = 1.0F;
  clip.channels.push_back(MakeChannel(
      AnimTargetPath::kRotation, AnimInterpolation::kLinear, {0.0F, 1.0F},
      {0, 0, 0, 1, end[0], end[1], end[2], end[3]}, 1));
  return clip;
}

auto SampleInto(const AnimClip& clip, float t, std::vector<float>& tr,
                std::vector<float>& ro, std::vector<float>& sc,
                std::vector<std::uint8_t>& set) -> AnimPose {
  AnimPose pose{tr.data(), ro.data(), sc.data(), set.data(), 2};
  static_cast<void>(SampleAnimClip(clip, t, pose));
  return pose;
}

// Golden TRS: translation (0.5,0,0), rotation slerp(45degY, 45degX, 0.5),
// scale 1. Column-major; hand-derived (python3, math module):
//   q = normalize(qA + qB), qA=(0,s,0,c), qB=(s,0,0,c),
//   s=sin(22.5deg), c=cos(22.5deg).
//   q = (0.19875685, 0.19875685, 0, 0.95968298), R from q, T in col 3.
static constexpr float kGoldenJoint1[16] = {
    0.92099143F, 0.07900857F, -0.38148714F, 0.0F,   //
    0.07900857F, 0.92099143F, 0.38148714F,  0.0F,   //
    0.38148714F, -0.38148714F, 0.84198285F, 0.0F,   //
    0.5F,        0.0F,         0.0F,         1.0F,   //
};

}  // namespace

auto main() -> int {
  int failures = 0;

  // --- blend basics: lerp T, slerp R --------------------------------------
  {
    std::vector<float> ta = {0, 0, 0}, sa = {1, 1, 1};
    std::vector<float> ra = {0, 0, 0, 1};
    std::vector<float> tb = {2, 0, 0}, sb = {1, 1, 1};
    const float s45 = 0.70710678F;
    std::vector<float> rb = {0, s45, 0, s45};  // 90deg Y
    std::uint8_t fa[1] = {1}, fb[1] = {0};
    const AnimPose pa{ta.data(), ra.data(), sa.data(), fa, 1};
    const AnimPose pb{tb.data(), rb.data(), sb.data(), fb, 1};
    std::vector<float> to = {9, 9, 9}, so = {9, 9, 9};
    std::vector<float> ro = {9, 9, 9, 9};
    std::uint8_t fo[1] = {9};
    BlendAnimPoses(pa, pb, 0.5F, AnimPose{to.data(), ro.data(), so.data(),
                                          fo, 1});
    const float want_t[3] = {1, 0, 0};
    // slerp(qI, 90degY, 0.5) = 45deg Y.
    const float s225 = 0.38268343F;
    const float c225 = 0.92387953F;
    const float want_r[4] = {0, s225, 0, c225};
    const float want_s[3] = {1, 1, 1};
    failures +=
        check(NearArr(to.data(), want_t, 3), "blend lerps translation at w=0.5");
    failures += check(NearArr(ro.data(), want_r, 4, 1e-4F),
                      "blend slerps rotation at w=0.5 (45deg Y)");
    failures +=
        check(NearArr(so.data(), want_s, 3), "blend keeps unit scale");
    failures += check(fo[0] == 1, "blend ORs set flags");

    // Endpoints reproduce the inputs.
    BlendAnimPoses(pa, pb, 0.0F,
                   AnimPose{to.data(), ro.data(), so.data(), fo, 1});
    failures +=
        check(NearArr(to.data(), ta.data(), 3) &&
                  NearArr(ro.data(), ra.data(), 4),
              "blend w=0 reproduces pose A");
    BlendAnimPoses(pa, pb, 1.0F,
                   AnimPose{to.data(), ro.data(), so.data(), fo, 1});
    failures +=
        check(NearArr(to.data(), tb.data(), 3) &&
                  NearArr(ro.data(), rb.data(), 4),
              "blend w=1 reproduces pose B");

    // Weight clamps; non-finite weight leaves out untouched.
    BlendAnimPoses(pa, pb, 7.0F,
                   AnimPose{to.data(), ro.data(), so.data(), fo, 1});
    failures += check(NearArr(to.data(), tb.data(), 3),
                      "blend clamps weight > 1");
    BlendAnimPoses(pa, pb, -7.0F,
                   AnimPose{to.data(), ro.data(), so.data(), fo, 1});
    failures += check(NearArr(to.data(), ta.data(), 3),
                      "blend clamps weight < 0");
    to[0] = 42.0F;
    BlendAnimPoses(pa, pb, std::nanf(""),
                   AnimPose{to.data(), ro.data(), so.data(), fo, 1});
    failures += check(to[0] == 42.0F, "blend NaN weight leaves out untouched");
  }

  // --- PoseToJointMatrices: hierarchy + inverse bind -----------------------
  {
    // Two joints: j0 identity, j1 translated +1 X, parented under j0.
    std::vector<float> tr = {0, 0, 0, 1, 0, 0};
    std::vector<float> ro = {0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<float> sc = {1, 1, 1, 1, 1, 1};
    const AnimPose pose{tr.data(), ro.data(), sc.data(), nullptr, 2};
    const std::int32_t parents[2] = {-1, 0};
    std::vector<float> out(32, 9.0F);
    failures += check(PoseToJointMatrices(pose, parents, nullptr, out.data(),
                                          2) == 2,
                      "matrices writes 2 joints");
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                             0, 0, 1, 0, 0, 0, 0, 1};
    failures += check(NearArr(out.data(), ident, 16),
                      "matrices joint0 is identity");
    const float want_j1[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 1, 0, 1, 0, 0, 1};
    failures += check(NearArr(out.data() + 16, want_j1, 16, 2e-6F),
                      "matrices joint1 carries translation");
    // Inverse bind cancels it: IBM = translate -1 X.
    const float ibm[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 1};
    failures += check(PoseToJointMatrices(pose, parents, ibm, out.data(), 2) ==
                          2,
                      "matrices with inverse bind writes 2 joints");
    failures += check(NearArr(out.data() + 16, ident, 16, 2e-6F),
                      "matrices inverse bind cancels translation");
    // Regression: non-identity PARENT inverse bind must not leak into the
    // child. Parent j0 translated +1 X with IBM translate -1 X; child j1
    // identity local parented under j0 with identity IBM. World j1 = +1 X,
    // so out j1 must carry exactly +1 X (old fold-during-walk code gave 0).
    {
      std::vector<float> tr2 = {1, 0, 0, 0, 0, 0};
      const AnimPose pose2{tr2.data(), ro.data(), sc.data(), nullptr, 2};
      const float ibm2[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                              -1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0,
                              0, 0, 1, 0, 0, 0, 0, 1};
      failures += check(PoseToJointMatrices(pose2, parents, ibm2, out.data(),
                                            2) == 2,
                        "matrices non-identity parent IBM writes 2 joints");
      failures += check(NearArr(out.data(), ident, 16, 2e-6F),
                        "matrices parent IBM cancels parent translation");
      const float want_child[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                    0, 0, 1, 0, 1, 0, 0, 1};
      failures += check(NearArr(out.data() + 16, want_child, 16, 2e-6F),
                        "matrices child unaffected by parent inverse bind");
    }
    // Fail-closed gates.
    out[0] = 42.0F;
    failures += check(PoseToJointMatrices(pose, nullptr, nullptr, out.data(),
                                          2) == 0 && out[0] == 42.0F,
                      "matrices null parents fails closed");
    failures +=
        check(PoseToJointMatrices(pose, parents, nullptr, nullptr, 2) == 0,
              "matrices null out fails closed");
    failures += check(PoseToJointMatrices(pose, parents, nullptr, out.data(),
                                          0) == 0,
                      "matrices zero count fails closed");
  }

  // --- golden: bent arm at t=0.5s ------------------------------------------------
  // clip A: joint1 45deg-Y at t=0.5 (sampled LINEAR -> 45deg Y exactly).
  // clip B: joint1 rotated 45deg-X at t=0.5, translated (1,0,0) STEP-held.
  // Blend w=0.5: T = (1,0,0)*0.5, R = slerp(45degY, 45degX, 0.5).
  // Joint 0 stays identity in both clips (set=0), joint 1 blended.
  {
    const float s45 = 0.70710678F;
    AnimClip clip_a = TwoBoneRotClip({0, s45, 0, s45});  // 90deg Y mid
    AnimClip clip_b = TwoBoneRotClip({s45, 0, 0, s45});  // 90deg X mid
    // clip B additionally translates joint 1 +X from t=0 (STEP holds it).
    clip_b.channels.push_back(
        MakeChannel(AnimTargetPath::kTranslation, AnimInterpolation::kStep,
                    {0.0F, 1.0F}, {1, 0, 0, 1, 0, 0}, 1));

    std::vector<float> ta(6, 9), ra(8, 9), sa(6, 9);
    std::vector<std::uint8_t> fa(2, 9);
    std::vector<float> tb(6, 9), rb(8, 9), sb(6, 9);
    std::vector<std::uint8_t> fb(2, 9);
    AnimPose pa = SampleInto(clip_a, 0.5F, ta, ra, sa, fa);
    AnimPose pb = SampleInto(clip_b, 0.5F, tb, rb, sb, fb);

    // Sampled inputs are what the golden assumes: A = 45deg Y, B = 45deg X.
    const float s225 = 0.38268343F;
    const float c225 = 0.92387953F;
    const float want_a[4] = {0, s225, 0, c225};
    const float want_b[4] = {s225, 0, 0, c225};
    failures += check(NearArr(ra.data() + 4, want_a, 4, 1e-4F),
                      "golden setup: clip A samples 45deg Y at t=0.5");
    failures += check(NearArr(rb.data() + 4, want_b, 4, 1e-4F),
                      "golden setup: clip B samples 45deg X at t=0.5");
    failures += check(tb[3] == 1 && tb[4] == 0 && tb[5] == 0,
                      "golden setup: clip B translation is +X");

    std::vector<float> to(6, 9), ro(8, 9), so(6, 9);
    std::vector<std::uint8_t> fo(2, 9);
    BlendAnimPoses(pa, pb, 0.5F,
                   AnimPose{to.data(), ro.data(), so.data(), fo.data(), 2});
    failures += check(fo[0] == 0 && fo[1] == 1,
                      "golden: set flags (joint0 unset, joint1 set)");
    failures += check(NearArr(to.data() + 3, kGoldenJoint1 + 12, 3, 2e-6F),
                      "golden: blended translation is half +X");

    const std::int32_t parents[2] = {-1, 0};
    std::vector<float> mats(32, 9.0F);
    failures += check(PoseToJointMatrices(
                          AnimPose{to.data(), ro.data(), so.data(), fo.data(),
                                   2},
                          parents, nullptr, mats.data(), 2) == 2,
                      "golden: matrices flatten 2 joints");
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                             0, 0, 1, 0, 0, 0, 0, 1};
    failures += check(NearArr(mats.data(), ident, 16),
                      "golden: joint0 matrix is identity");
    failures += check(NearArr(mats.data() + 16, kGoldenJoint1, 16, 1e-4F),
                      "golden: bent-arm joint1 matrix at t=0.5 matches");
    // The flattened pose is the joint-palette UBO payload: 2 x 64B
    // column-major mat4s, joint 0 = identity, joint 1 = the golden bend.
    failures += check(mats.size() == 32,
                      "golden: 2-joint pose flattens to 2x64B (UBO palette)");
  }

  if (failures == 0) {
    std::printf("ANIM_BLENDING_OK\n");
  } else {
    std::printf("ANIM_BLENDING_FAIL (%d)\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
