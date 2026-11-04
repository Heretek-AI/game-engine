// animator_state_machine.hpp — P5-03: JSON state-machine asset + runtime.
//
// Native-owned per-entity animator: parses the P5-03 JSON graph (states,
// float/bool/trigger parameters, AND-ed transition conditions, cross-fade
// durations), evaluates conditions per Update, and samples/blends clips via
// the P5-02 sampler (SampleAnimClip) + blender (BlendAnimPoses).
//
// Frame-path rules (AGENTS.md): load allocates (ParseAnimatorGraph); the
// update path is noexcept with no malloc/new — all storage is reserved at
// parse time (fixed capacities), params/conditions addressed by index,
// clip lookup by index. Triggers are consume-on-fire (auto-reset).
//
// Clip model: AnimatorGraph holds NON-OWNING pointers to caller-owned
// AnimClips (imported P5-01/P5-02 set, must outlive the graph). The loader
// resolves each state's `clip` name to an index once, at parse time.
//
// Easing: linear weight ramp (documented choice; smoothstep is a
// follow-up if acceptance wants it — recorded in docs/notes/P5-03.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/assets/anim_sampling.hpp"
#include "engine/assets/anim_blending.hpp"
#include "engine/assets/engine_assets_export.hpp"
#include "engine/assets/skeletal_import.hpp"

namespace engine::assets {

// Fixed capacities (parse-time reservation, update path never grows).
inline constexpr std::size_t kAnimMaxParams = 64;
inline constexpr std::size_t kAnimMaxStates = 64;
inline constexpr std::size_t kAnimMaxTransitions = 256;
inline constexpr std::size_t kAnimMaxCondsPerTransition = 8;
inline constexpr std::size_t kAnimMaxName = 64;

enum class AnimParamType : std::uint8_t {
  kFloat = 0,
  kBool = 1,
  kTrigger = 2,
};

enum class AnimCondOp : std::uint8_t {
  kGreater = 0,       // float >
  kLess = 1,          // float <
  kGreaterEqual = 2,  // float >=
  kLessEqual = 3,     // float <=
  kEqual = 4,         // float == | bool ==
  kNotEqual = 5,      // float !=
  kFired = 6,         // trigger fired
};

struct AnimatorParam {
  std::string name;
  AnimParamType type = AnimParamType::kFloat;
  float float_default = 0.0F;
  bool bool_default = false;
};

struct AnimatorCondition {
  std::size_t param_index = 0;  // index into graph.params
  AnimCondOp op = AnimCondOp::kEqual;
  float float_value = 0.0F;
  bool bool_value = false;
};

struct AnimatorState {
  std::string name;
  std::size_t clip_index = 0;  // resolved index into clips[]
  bool loop = true;
};

struct AnimatorTransition {
  std::size_t from_state = 0;
  std::size_t to_state = 0;
  float duration = 0.0F;  // seconds; 0 = cut
  std::vector<AnimatorCondition> conditions;
};

struct AnimatorGraph {
  std::string name;
  std::vector<AnimatorParam> params;
  std::vector<AnimatorState> states;
  std::vector<AnimatorTransition> transitions;
  std::size_t initial_state = 0;
  // Non-owning: caller-owned clips the state clip names resolved against.
  const std::vector<AnimClip>* clips = nullptr;
};

// Parse the JSON state-machine graph; resolves state clip names against
// `clips` (must outlive the graph). Fail-closed: false on any malformed
// input (bad JSON, unknown clip/param, missing initial, over capacity,
// negative duration, non-finite number); `out` untouched.
[[nodiscard]] ENGINE_ASSETS_API auto ParseAnimatorGraph(
    const char* json, const std::vector<AnimClip>* clips, AnimatorGraph* out)
    -> bool;

// Runtime: native-owned frame state. Backed by the graph (borrowed, must
// outlive the runtime). Param storage + scratch pose buffers sized at
// construction from the graph; Update never allocates.
//
// MSVC note: std::vector members in an exported class trigger C4251
// (needs dll-interface) under /WX. Same suppression pattern as
// renderer/gpu_cull.hpp + slang_shader_cache.hpp: push/disable around the
// export-bearing include only (the class + ENGINE_ASSETS_API decl).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class ENGINE_ASSETS_API AnimatorRuntime {
 public:
  AnimatorRuntime() noexcept = default;
  ~AnimatorRuntime() = default;
  AnimatorRuntime(const AnimatorRuntime&) = delete;
  auto operator=(const AnimatorRuntime&) -> AnimatorRuntime& = delete;
  AnimatorRuntime(AnimatorRuntime&&) noexcept = default;
  auto operator=(AnimatorRuntime&&) noexcept -> AnimatorRuntime& = default;

  // Bind to `graph` (borrowed) with `joint_count` joints. Returns false
  // (state untouched) on null graph, zero joints, or empty states.
  auto Init(const AnimatorGraph* graph, std::size_t joint_count) noexcept
      -> bool;
  [[nodiscard]] auto IsInit() const noexcept -> bool { return graph_ != nullptr; }

  auto SetFloat(std::size_t param, float value) noexcept -> bool;
  auto SetBool(std::size_t param, bool value) noexcept -> bool;
  auto SetTrigger(std::size_t param) noexcept -> bool;
  [[nodiscard]] auto FindParam(const char* name) const noexcept -> std::size_t;

  // Advance clip times + cross-fade by `dt` seconds. Fail-closed: false
  // (state untouched) when uninitialised or dt is non-finite/negative.
  auto Update(float dt) noexcept -> bool;

  // Sample the current active pose into caller-owned `pose` (AnimPoseReset
  // semantics: sample from-state + to-state during a fade, blend by the
  // eased weight; single sample when settled). Returns joints written.
  auto SampleCurrent(AnimPose pose) noexcept -> std::size_t;

  // Active-state readback for the acceptance test: index, plus whether a
  // cross-fade is in flight and its 0..1 weight.
  [[nodiscard]] auto ActiveState() const noexcept -> std::size_t {
    return active_;
  }
  [[nodiscard]] auto StateNameAt(std::size_t i) const noexcept -> const char*;
  [[nodiscard]] auto StateCount() const noexcept -> std::size_t;
  [[nodiscard]] auto IsBlending() const noexcept -> bool { return blending_; }
  [[nodiscard]] auto BlendWeight() const noexcept -> float {
    return blend_weight_;
  }
  [[nodiscard]] auto ActiveTime() const noexcept -> float { return time_; }

 private:
  const AnimatorGraph* graph_ = nullptr;
  std::size_t joint_count_ = 0;
  std::size_t active_ = 0;
  float time_ = 0.0F;  // local time in the active state's clip
  // Cross-fade: both clips sampled at own local times.
  bool blending_ = false;
  std::size_t from_ = 0;
  std::size_t to_ = 0;
  float from_time_ = 0.0F;
  float blend_pos_ = 0.0F;    // seconds elapsed in the fade
  float blend_dur_ = 0.0F;    // fade duration seconds
  float blend_weight_ = 0.0F;  // eased 0..1 weight of the `to` pose
  // Param storage (parallel to graph_->params).
  float float_vals_[kAnimMaxParams] = {};
  bool flag_vals_[kAnimMaxParams] = {};
  // Scratch pose buffers (caller-owned pattern turned inward: owned here,
  // sized at Init, reused every SampleCurrent — never on the frame path).
  // Not mutable: blend sampling takes a non-const runtime (Update and
  // SampleCurrent both mutate fade state), so no const/mutex issue by
  // construction. Single-threaded by contract like all Flecs component data.
  std::vector<float> buf_a_t_, buf_a_r_, buf_a_s_;
  std::vector<float> buf_b_t_, buf_b_r_, buf_b_s_;
  std::vector<std::uint8_t> buf_a_set_, buf_b_set_;

  auto CheckTransition() noexcept -> void;
  [[nodiscard]] auto CondsMet(const AnimatorTransition& tr) const noexcept
      -> bool;
  auto BeginTransition(const AnimatorTransition& tr) noexcept -> void;
  auto SampleStateAt(std::size_t state, float t, AnimPose pose) const noexcept
      -> void;
};

#ifdef _MSC_VER
#pragma warning(pop)  // C4251 scoped to AnimatorRuntime
#endif

}  // namespace engine::assets
