// test_animator_state_machine.cpp — P5-03 acceptance: JSON graph + runtime.
//
// Covers the task acceptance: JSON loads, float/bool/trigger conditions
// evaluate, cross-fades blend active states. Uses one-joint translation
// clips (idle y=0, run y=2, jump y=4) so blended poses are exactly
// checkable: during a fade out = from*(1-w) + to*w at the clips' own
// local times (non-looping `to` starts at 0 on entry).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/assets/anim_sampling.hpp"
#include "engine/assets/animator_state_machine.hpp"
#include "engine/assets/skeletal_import.hpp"

#include "anim_test_helpers.hpp"

namespace {

using engine::test::anim::check;
using engine::test::anim::Near;

using engine::assets::AnimChannel;
using engine::assets::AnimClip;
using engine::assets::AnimInterpolation;
using engine::assets::AnimParamType;
using engine::assets::AnimPose;
using engine::assets::AnimPoseReset;
using engine::assets::AnimatorGraph;
using engine::assets::AnimatorRuntime;
using engine::assets::AnimTargetPath;
using engine::assets::ParseAnimatorGraph;
using engine::assets::kAnimMaxParams;

auto FlatClip(const char* name, float y_at_1, float duration = 2.0F)
    -> AnimClip {
  AnimClip clip;
  clip.name = name;
  clip.duration = duration;
  AnimChannel ch;
  ch.target_joint = 0;
  ch.target_path = AnimTargetPath::kTranslation;
  ch.interpolation = AnimInterpolation::kLinear;
  ch.times = {0.0F, duration};
  ch.values = {0.0F, 0.0F, 0.0F, 0.0F, y_at_1, 0.0F};
  clip.channels.push_back(std::move(ch));
  return clip;
}

auto SampleY(AnimatorRuntime& rt) -> float {
  float t[3] = {0, 0, 0};
  float r[4] = {0, 0, 0, 1};
  float s[3] = {1, 1, 1};
  std::uint8_t set[1] = {0};
  AnimPose pose{t, r, s, set, 1};
  AnimPoseReset(pose);
  if (rt.SampleCurrent(pose) == 0) {
    return std::nanf("");
  }
  return t[1];
}

constexpr const char* kGraphJson = R"json(
{
  "name": "Locomotion",
  "initial": "Idle",
  "parameters": [
    {"name": "speed", "type": "float", "default": 0.0},
    {"name": "grounded", "type": "bool", "default": true},
    {"name": "jump", "type": "trigger"}
  ],
  "states": [
    {"name": "Idle", "clip": "idle", "loop": true},
    {"name": "Run", "clip": "run", "loop": true},
    {"name": "Jump", "clip": "jump", "loop": false}
  ],
  "transitions": [
    {"from": "Idle", "to": "Run",
     "duration": 0.25,
     "conditions": [{"param": "speed", "op": ">", "value": 0.5}]},
    {"from": "Run", "to": "Idle",
     "duration": 0.2,
     "conditions": [{"param": "speed", "op": "<=", "value": 0.5}]},
    {"from": "Run", "to": "Jump",
     "duration": 0.1,
     "conditions": [
       {"param": "jump", "op": "fired"},
       {"param": "grounded", "op": "==", "value": true}]}
  ]
}
)json";

auto StateName(const AnimatorRuntime& rt) -> std::string {
  std::size_t i = rt.ActiveState();
  const char* n = rt.StateNameAt(i);
  return n != nullptr ? std::string(n) : std::string("<bad>");
}

}  // namespace

auto main() -> int {
  int fails = 0;

  std::vector<AnimClip> clips;
  clips.push_back(FlatClip("idle", 0.0F));
  clips.push_back(FlatClip("run", 2.0F));
  clips.push_back(FlatClip("jump", 4.0F));

  // --- JSON loads ------------------------------------------------------------
  AnimatorGraph graph;
  bool ok = ParseAnimatorGraph(kGraphJson, &clips, &graph);
  fails += check(ok, "json loads");
  fails += check(graph.name == "Locomotion", "graph name");
  fails += check(graph.params.size() == 3, "3 params");
  fails += check(graph.params[0].type == AnimParamType::kFloat, "speed float");
  fails += check(graph.states.size() == 3, "3 states");
  fails += check(graph.initial_state == 0, "initial Idle");
  fails += check(graph.transitions.size() == 3, "3 transitions");

  // --- fail-closed loads ------------------------------------------------------
  AnimatorGraph bad;
  fails += check(!ParseAnimatorGraph(nullptr, &clips, &bad), "null json");
  fails += check(!ParseAnimatorGraph("{bad json", &clips, &bad), "malformed");
  fails += check(!ParseAnimatorGraph("{\"states\":[]}", &clips, &bad),
                 "empty states");
  fails += check(!ParseAnimatorGraph(
                     "{\"initial\":\"Idle\",\"states\":[{\"name\":\"Idle\","
                     "\"clip\":\"nope\"}]}",
                     &clips, &bad),
                 "unknown clip");
  fails += check(!ParseAnimatorGraph(
                     "{\"initial\":\"Idle\",\"states\":[{\"name\":\"Idle\","
                     "\"clip\":\"idle\"}],\"transitions\":[{\"from\":\"Idle\","
                     "\"to\":\"Idle\",\"conditions\":[{\"param\":\"nope\","
                     "\"op\":\">\",\"value\":1}]}]}",
                     &clips, &bad),
                 "unknown param");
  fails += check(!ParseAnimatorGraph(
                     "{\"initial\":\"Missing\",\"states\":[{\"name\":\"Idle\","
                     "\"clip\":\"idle\"}]}",
                     &clips, &bad),
                 "missing initial");
  fails += check(!ParseAnimatorGraph(
                     "{\"initial\":\"Idle\",\"states\":[{\"name\":\"Idle\","
                     "\"clip\":\"idle\"}],\"transitions\":[{\"from\":\"Idle\","
                     "\"to\":\"Idle\",\"duration\":-1}]}",
                     &clips, &bad),
                 "negative duration");

  // --- runtime: float condition + cross-fade -----------------------------------
  AnimatorRuntime rt;
  fails += check(rt.Init(&graph, 1), "init");
  fails += check(StateName(rt) == "Idle", "starts Idle");
  fails += check(Near(SampleY(rt), 0.0F), "idle pose y=0");

  std::size_t speed = rt.FindParam("speed");
  std::size_t jump = rt.FindParam("jump");
  std::size_t grounded = rt.FindParam("grounded");
  fails += check(speed == 0 && grounded == 1 && jump == 2, "param indices");
  fails += check(rt.FindParam("nope") == kAnimMaxParams,
                 "unknown param sentinel");
  fails += check(!rt.SetFloat(jump, 1.0F), "type gate float-on-trigger");

  // speed below threshold: no transition.
  fails += check(rt.SetFloat(speed, 0.2F), "set speed 0.2");
  fails += check(rt.Update(0.05F), "update");
  fails += check(StateName(rt) == "Idle", "stays Idle");

  // Cross the threshold: fade Idle->Run over 0.25s. Update steps of 0.05:
  // first update fires the transition (weight 0), then 5 steps complete it.
  fails += check(rt.SetFloat(speed, 1.0F), "set speed 1.0");
  fails += check(rt.Update(0.05F), "update fires");
  fails += check(rt.IsBlending(), "blending after fire");
  fails += check(Near(rt.BlendWeight(), 0.0F), "weight 0 at fire");
  fails += check(StateName(rt) == "Idle", "active still Idle in fade");

  // Mid-fade: weight 0.4 (0.1/0.25) blends idle y=0 and run y=1.0
  // (run clip: y 0->2 over 2s, sampled at local fade time 0.1 = y 0.1).
  fails += check(rt.Update(0.05F), "fade step 1");
  fails += check(rt.Update(0.05F), "fade step 2");
  fails += check(rt.IsBlending(), "still blending");
  fails += check(Near(rt.BlendWeight(), 0.4F, 1e-4F), "weight 0.4 mid-fade");
  {
    float y = SampleY(rt);
    // from = idle(t) = 0; to = run(0.1) = 0.1; out = 0*0.6 + 0.1*0.4.
    fails += check(Near(y, 0.04F, 1e-4F), "mid-fade blended y");
  }

  fails += check(rt.Update(0.05F), "fade step 3");
  fails += check(rt.Update(0.05F), "fade step 4");
  fails += check(rt.Update(0.05F), "fade step 5 completes");
  fails += check(!rt.IsBlending(), "fade done");
  fails += check(StateName(rt) == "Run", "active Run after fade");

  // --- bool + trigger AND -------------------------------------------------------
  // jump while grounded=false: no fire (AND fails).
  fails += check(rt.SetBool(grounded, false), "airborne");
  fails += check(rt.SetTrigger(jump), "set jump trigger");
  fails += check(rt.Update(0.05F), "update airborne");
  fails += check(StateName(rt) == "Run", "no jump while airborne");
  // Trigger was NOT consumed (transition never fired) — still latched.
  fails += check(rt.SetBool(grounded, true), "landed");
  fails += check(rt.Update(0.05F), "update fires jump");
  fails += check(rt.IsBlending(), "jump fade starts");
  fails += check(rt.Update(0.05F), "jump fade step 1");
  fails += check(rt.IsBlending(), "jump still fading at 0.05/0.1");
  fails += check(rt.Update(0.05F), "jump fade step 2 completes (0.1s)");
  fails += check(!rt.IsBlending(), "jump fade done");
  fails += check(StateName(rt) == "Jump", "active Jump");
  // Trigger consumed on fire: a second Update must not re-fire anything.
  // (Jump has no outgoing transitions; Run->Jump needs the trigger again.)
  fails += check(rt.SetFloat(speed, 1.0F), "speed still high");
  fails += check(rt.Update(0.5F), "settle");
  fails += check(StateName(rt) == "Jump", "trigger consumed, stays Jump");

  // --- non-looping clip clamps -----------------------------------------------------
  // Jump `to` time at fade end = fade elapsed (0.10s: the fire update's dt
  // runs on the `from` clock; the `to` clip starts at 0 on entry, so only
  // the 2 blend steps count) + settle 0.5s = 0.60s.
  fails += check(Near(rt.ActiveTime(), 0.6F, 1e-4F), "jump time advances");
  fails += check(rt.Update(5.0F), "overshoot");
  fails += check(Near(rt.ActiveTime(), 2.0F, 1e-4F), "non-loop clamps");

  // --- cut transition (duration 0) ----------------------------------------------------
  constexpr const char* kCutJson = R"json(
{"initial": "A",
 "states": [{"name": "A", "clip": "idle"}, {"name": "B", "clip": "run"}],
 "transitions": [{"from": "A", "to": "B", "duration": 0,
   "conditions": [{"param": "go", "op": "fired"}]}],
 "parameters": [{"name": "go", "type": "trigger"}]}
)json";
  AnimatorGraph cut;
  fails += check(ParseAnimatorGraph(kCutJson, &clips, &cut), "cut graph loads");
  AnimatorRuntime cut_rt;
  fails += check(cut_rt.Init(&graph, 0) == false, "zero joints rejected");
  fails += check(cut_rt.Init(&cut, 1), "cut init");
  std::size_t go = cut_rt.FindParam("go");
  fails += check(cut_rt.SetTrigger(go), "set go");
  fails += check(cut_rt.Update(0.016F), "cut update");
  fails += check(!cut_rt.IsBlending(), "cut never blends");
  fails += check(cut_rt.ActiveState() == 1, "cut lands on B");

  // --- fail-closed runtime ------------------------------------------------------
  AnimatorRuntime dead;
  fails += check(!dead.Update(0.016F), "uninit update fails");
  fails += check(const_cast<AnimatorRuntime&>(dead).SampleCurrent(AnimPose{nullptr, nullptr, nullptr,
                                             nullptr, 0}) == 0,
                 "uninit sample 0");
  fails += check(!rt.Update(std::nanf("")), "NaN dt rejected");
  fails += check(!rt.SetFloat(speed, std::nanf("")), "NaN param rejected");
  fails += check(!rt.SetBool(speed, true), "type gate bool-on-float");

  if (fails == 0) {
    std::printf("ANIMATOR_STATE_MACHINE_OK\n");
  }
  return fails == 0 ? 0 : 1;
}
