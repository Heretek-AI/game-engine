// animator_state_machine.cpp — P5-03 JSON graph loader + runtime.
//
// Loader: minimal JSON via the shared engine/assets/json_minimal.hpp reader
// (Value + Parser: objects/arrays/strings/numbers/true/false/null, depth
// cap, no new dependency — same skeleton P2-06 scene files use, shared so
// the two copies don't trip the Sonar duplication gate). Parse allocates
// (load lane only).
//
// Runtime: noexcept, no malloc/new on Update/SampleCurrent — capacities
// fixed at Init from the graph; params addressed by index; trigger bits
// consume-on-fire when a transition using them is taken.
#include "engine/assets/animator_state_machine.hpp"

#include "engine/assets/json_minimal.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace engine::assets {

namespace {

using json_min::Parser;
using json_min::Value;

auto ToDouble(const Value& v, double& out) -> bool {
  if (v.kind != Value::Kind::Num) {
    return false;
  }
  try {
    std::size_t pos = 0;
    out = std::stod(v.str, &pos);
    return pos == v.str.size();
  } catch (...) {
    return false;
  }
}

auto ToFloatFinite(const Value& v, float& out) -> bool {
  double d = 0;
  if (!ToDouble(v, d) || !std::isfinite(d) ||
      d > std::numeric_limits<float>::max() ||
      d < -std::numeric_limits<float>::max()) {
    return false;
  }
  out = static_cast<float>(d);
  return true;
}

// Shared name-index lookup (Sonar dedup): states/params/clips all resolve
// a name to an index with the same linear scan. `name_of` projects the
// element to its comparable name.
template <typename T, typename NameOf>
auto FindNamedIndex(const std::vector<T>& items, std::string_view name,
                    std::size_t& out, NameOf&& name_of) -> bool {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (name_of(items[i]) == name) {
      out = i;
      return true;
    }
  }
  return false;
}

auto FindState(const std::vector<AnimatorState>& states, std::string_view name,
               std::size_t& out) -> bool {
  return FindNamedIndex(states, name, out, [](const AnimatorState& s) {
    return std::string_view{s.name};
  });
}

auto FindParam(const std::vector<AnimatorParam>& params, std::string_view name,
               std::size_t& out) -> bool {
  return FindNamedIndex(params, name, out, [](const AnimatorParam& p) {
    return std::string_view{p.name};
  });
}

auto FindClip(const std::vector<AnimClip>& clips, std::string_view name,
              std::size_t& out) -> bool {
  return FindNamedIndex(clips, name, out, [](const AnimClip& c) {
    return std::string_view{c.name};
  });
}

}  // namespace

// Single float-op comparison (flattened CondsMet helper: one branch level
// instead of a nested 7-way switch — Sonar S134/S3776).
// File scope in engine::assets (internal linkage via static); the anonymous
// namespace above is already closed, so this stays outside it and the
// AnimatorRuntime:: member definitions below are unaffected.
static auto FloatOpHolds(AnimCondOp op, float v, float target) noexcept
    -> bool {
  switch (op) {
    case AnimCondOp::kGreater:
      return v > target;
    case AnimCondOp::kLess:
      return v < target;
    case AnimCondOp::kGreaterEqual:
      return v >= target;
    case AnimCondOp::kLessEqual:
      return v <= target;
    case AnimCondOp::kEqual:
      return v == target;
    case AnimCondOp::kNotEqual:
      return v != target;
    case AnimCondOp::kFired:
      return false;  // loader rejects trigger-op on float
  }
  return false;
}

auto ParseAnimatorGraph(const char* json, const std::vector<AnimClip>* clips,
                        AnimatorGraph* out) -> bool {
  if (json == nullptr || clips == nullptr || out == nullptr) {
    return false;
  }
  std::size_t len = std::strlen(json);
  Parser p;
  p.cur = json;
  p.end = json + len;
  Value root;
  p.parse_value(root);
  p.skip_ws();
  if (!p.ok || p.cur != p.end || root.kind != Value::Kind::Obj) {
    return false;
  }

  AnimatorGraph g;
  g.clips = clips;

  const Value* name_v = root.find("name");
  if (name_v != nullptr && name_v->kind == Value::Kind::Str) {
    g.name = name_v->str;
  }

  // Parameters.
  const Value* params_v = root.find("parameters");
  if (params_v != nullptr) {
    if (params_v->kind != Value::Kind::Arr) {
      return false;
    }
    if (params_v->items.size() > kAnimMaxParams) {
      return false;
    }
    for (const Value& pv : params_v->items) {
      if (pv.kind != Value::Kind::Obj) {
        return false;
      }
      const Value* pn = pv.find("name");
      const Value* pt = pv.find("type");
      if (pn == nullptr || pn->kind != Value::Kind::Str || pn->str.empty() ||
          pn->str.size() > kAnimMaxName || pt == nullptr ||
          pt->kind != Value::Kind::Str) {
        return false;
      }
      std::size_t dup = 0;
      if (FindParam(g.params, pn->str, dup)) {
        return false;  // duplicate parameter name
      }
      AnimatorParam param;
      param.name = pn->str;
      if (pt->str == "float") {
        param.type = AnimParamType::kFloat;
        const Value* dv = pv.find("default");
        if (dv != nullptr && !ToFloatFinite(*dv, param.float_default)) {
          return false;
        }
      } else if (pt->str == "bool") {
        param.type = AnimParamType::kBool;
        const Value* dv = pv.find("default");
        if (dv != nullptr) {
          if (dv->kind == Value::Kind::True) {
            param.bool_default = true;
          } else if (dv->kind == Value::Kind::False) {
            param.bool_default = false;
          } else {
            return false;
          }
        }
      } else if (pt->str == "trigger") {
        param.type = AnimParamType::kTrigger;
      } else {
        return false;
      }
      g.params.push_back(std::move(param));
    }
  }

  // States.
  const Value* states_v = root.find("states");
  if (states_v == nullptr || states_v->kind != Value::Kind::Arr ||
      states_v->items.empty() || states_v->items.size() > kAnimMaxStates) {
    return false;
  }
  for (const Value& sv : states_v->items) {
    if (sv.kind != Value::Kind::Obj) {
      return false;
    }
    const Value* sn = sv.find("name");
    const Value* sc = sv.find("clip");
    if (sn == nullptr || sn->kind != Value::Kind::Str || sn->str.empty() ||
        sn->str.size() > kAnimMaxName || sc == nullptr ||
        sc->kind != Value::Kind::Str) {
      return false;
    }
    std::size_t dup = 0;
    if (FindState(g.states, sn->str, dup)) {
      return false;
    }
    std::size_t clip_idx = 0;
    if (!FindClip(*clips, sc->str, clip_idx)) {
      return false;  // unknown clip: fail-closed
    }
    AnimatorState st;
    st.name = sn->str;
    st.clip_index = clip_idx;
    const Value* loop_v = sv.find("loop");
    if (loop_v != nullptr) {
      if (loop_v->kind == Value::Kind::True) {
        st.loop = true;
      } else if (loop_v->kind == Value::Kind::False) {
        st.loop = false;
      } else {
        return false;
      }
    }
    g.states.push_back(std::move(st));
  }

  const Value* init_v = root.find("initial");
  if (init_v == nullptr || init_v->kind != Value::Kind::Str) {
    return false;
  }
  if (!FindState(g.states, init_v->str, g.initial_state)) {
    return false;
  }

  // Transitions.
  const Value* trans_v = root.find("transitions");
  if (trans_v != nullptr) {
    if (trans_v->kind != Value::Kind::Arr ||
        trans_v->items.size() > kAnimMaxTransitions) {
      return false;
    }
    for (const Value& tv : trans_v->items) {
      if (tv.kind != Value::Kind::Obj) {
        return false;
      }
      const Value* from_v = tv.find("from");
      const Value* to_v = tv.find("to");
      if (from_v == nullptr || from_v->kind != Value::Kind::Str ||
          to_v == nullptr || to_v->kind != Value::Kind::Str) {
        return false;
      }
      AnimatorTransition tr;
      if (!FindState(g.states, from_v->str, tr.from_state) ||
          !FindState(g.states, to_v->str, tr.to_state)) {
        return false;
      }
      const Value* dur_v = tv.find("duration");
      if (dur_v != nullptr) {
        if (!ToFloatFinite(*dur_v, tr.duration) || tr.duration < 0.0F) {
          return false;
        }
      }
      const Value* conds_v = tv.find("conditions");
      if (conds_v != nullptr) {
        if (conds_v->kind != Value::Kind::Arr ||
            conds_v->items.size() > kAnimMaxCondsPerTransition) {
          return false;
        }
        for (const Value& cv : conds_v->items) {
          if (cv.kind != Value::Kind::Obj) {
            return false;
          }
          const Value* pn = cv.find("param");
          const Value* op_v = cv.find("op");
          if (pn == nullptr || pn->kind != Value::Kind::Str ||
              op_v == nullptr || op_v->kind != Value::Kind::Str) {
            return false;
          }
          AnimatorCondition cond;
          if (!FindParam(g.params, pn->str, cond.param_index)) {
            return false;
          }
          const AnimatorParam& param = g.params[cond.param_index];
          const std::string& op = op_v->str;
          if (param.type == AnimParamType::kTrigger) {
            if (op != "fired") {
              return false;
            }
            cond.op = AnimCondOp::kFired;
          } else if (param.type == AnimParamType::kBool) {
            if (op != "==") {
              return false;
            }
            cond.op = AnimCondOp::kEqual;
            const Value* vv = cv.find("value");
            if (vv == nullptr) {
              return false;
            }
            if (vv->kind == Value::Kind::True) {
              cond.bool_value = true;
            } else if (vv->kind == Value::Kind::False) {
              cond.bool_value = false;
            } else {
              return false;
            }
          } else {
            if (op == ">") {
              cond.op = AnimCondOp::kGreater;
            } else if (op == "<") {
              cond.op = AnimCondOp::kLess;
            } else if (op == ">=") {
              cond.op = AnimCondOp::kGreaterEqual;
            } else if (op == "<=") {
              cond.op = AnimCondOp::kLessEqual;
            } else if (op == "==") {
              cond.op = AnimCondOp::kEqual;
            } else if (op == "!=") {
              cond.op = AnimCondOp::kNotEqual;
            } else {
              return false;
            }
            const Value* vv = cv.find("value");
            if (vv == nullptr || !ToFloatFinite(*vv, cond.float_value)) {
              return false;
            }
          }
          tr.conditions.push_back(cond);
        }
      }
      g.transitions.push_back(std::move(tr));
    }
  }

  *out = std::move(g);
  return true;
}

auto AnimatorRuntime::Init(const AnimatorGraph* graph,
                           std::size_t joint_count) noexcept -> bool {
  if (graph == nullptr || joint_count == 0 || graph->states.empty() ||
      graph->initial_state >= graph->states.size() ||
      graph->params.size() > kAnimMaxParams) {
    return false;
  }
  graph_ = graph;
  joint_count_ = joint_count;
  active_ = graph->initial_state;
  time_ = 0.0F;
  blending_ = false;
  blend_weight_ = 0.0F;
  for (std::size_t i = 0; i < graph->params.size(); ++i) {
    float_vals_[i] = graph->params[i].float_default;
    flag_vals_[i] = graph->params[i].bool_default;
  }
  // Scratch buffers sized once here (load lane); Update never grows them.
  // Guard the multiplies against size_t overflow.
  const std::size_t max_count = std::numeric_limits<std::size_t>::max() / 4U;
  if (joint_count > max_count) {
    graph_ = nullptr;
    return false;
  }
  buf_a_t_.assign(joint_count * 3U, 0.0F);
  buf_a_r_.assign(joint_count * 4U, 0.0F);
  buf_a_s_.assign(joint_count * 3U, 0.0F);
  buf_b_t_.assign(joint_count * 3U, 0.0F);
  buf_b_r_.assign(joint_count * 4U, 0.0F);
  buf_b_s_.assign(joint_count * 3U, 0.0F);
  buf_a_set_.assign(joint_count, 0);
  buf_b_set_.assign(joint_count, 0);
  if (buf_a_t_.size() != joint_count * 3U ||
      buf_a_r_.size() != joint_count * 4U ||
      buf_b_t_.size() != joint_count * 3U ||
      buf_b_r_.size() != joint_count * 4U ||
      buf_a_set_.size() != joint_count || buf_b_set_.size() != joint_count) {
    graph_ = nullptr;
    return false;
  }
  return true;
}

auto AnimatorRuntime::FindParam(const char* name) const noexcept -> std::size_t {
  if (graph_ == nullptr || name == nullptr) {
    return kAnimMaxParams;
  }
  for (std::size_t i = 0; i < graph_->params.size(); ++i) {
    if (graph_->params[i].name == name) {
      return i;
    }
  }
  return kAnimMaxParams;  // sentinel: unknown
}

auto AnimatorRuntime::SetFloat(std::size_t param, float value) noexcept
    -> bool {
  if (graph_ == nullptr || param >= graph_->params.size() ||
      graph_->params[param].type != AnimParamType::kFloat ||
      !std::isfinite(value)) {
    return false;
  }
  float_vals_[param] = value;
  return true;
}

auto AnimatorRuntime::SetBool(std::size_t param, bool value) noexcept -> bool {
  if (graph_ == nullptr || param >= graph_->params.size() ||
      graph_->params[param].type != AnimParamType::kBool) {
    return false;
  }
  flag_vals_[param] = value;
  return true;
}

auto AnimatorRuntime::SetTrigger(std::size_t param) noexcept -> bool {
  if (graph_ == nullptr || param >= graph_->params.size() ||
      graph_->params[param].type != AnimParamType::kTrigger) {
    return false;
  }
  flag_vals_[param] = true;
  return true;
}

auto AnimatorRuntime::CondsMet(const AnimatorTransition& tr) const noexcept
    -> bool {
  for (const AnimatorCondition& c : tr.conditions) {
    const AnimatorParam& p = graph_->params[c.param_index];
    if (p.type == AnimParamType::kFloat) {
      if (!FloatOpHolds(c.op, float_vals_[c.param_index], c.float_value)) {
        return false;
      }
    } else if (p.type == AnimParamType::kBool) {
      if (flag_vals_[c.param_index] != c.bool_value) {
        return false;
      }
    } else if (!flag_vals_[c.param_index]) {
      return false;  // trigger: latched bit
    }
  }
  return true;
}

auto AnimatorRuntime::BeginTransition(const AnimatorTransition& tr) noexcept
    -> void {
  // Consume triggers used by this transition (consume-on-fire).
  for (const AnimatorCondition& c : tr.conditions) {
    if (graph_->params[c.param_index].type == AnimParamType::kTrigger) {
      flag_vals_[c.param_index] = false;
    }
  }
  if (tr.duration <= 0.0F) {
    active_ = tr.to_state;  // cut
    time_ = 0.0F;
    blending_ = false;
    blend_weight_ = 0.0F;
    return;
  }
  blending_ = true;
  from_ = active_;
  from_time_ = time_;
  to_ = tr.to_state;
  blend_pos_ = 0.0F;
  blend_dur_ = tr.duration;
  blend_weight_ = 0.0F;
}

auto AnimatorRuntime::CheckTransition() noexcept -> void {
  // First listed eligible transition wins; only from the active state.
  // No self-retrigger while a fade is in flight (the `to` state owns the
  // next decision once the fade completes).
  if (blending_) {
    return;
  }
  for (const AnimatorTransition& tr : graph_->transitions) {
    if (tr.from_state != active_) {
      continue;
    }
    if (CondsMet(tr)) {
      BeginTransition(tr);
      return;
    }
  }
}

auto AnimatorRuntime::Update(float dt) noexcept -> bool {
  if (graph_ == nullptr || !std::isfinite(dt) || dt < 0.0F) {
    return false;
  }
  if (graph_->clips == nullptr) {
    return false;
  }
  const std::vector<AnimClip>& clips = *graph_->clips;
  auto step = [](float t, float dt_in, float dur, bool loop) -> float {
    float nt = t + dt_in;
    if (!(dur > 0.0F)) {
      return 0.0F;
    }
    if (loop) {
      nt = std::fmod(nt, dur);
      if (nt < 0.0F) {
        nt += dur;
      }
      return nt;
    }
    return nt >= dur ? dur : nt;
  };
  if (blending_) {
    const AnimClip& from_clip = clips[graph_->states[from_].clip_index];
    const AnimClip& to_clip = clips[graph_->states[to_].clip_index];
    from_time_ = step(from_time_, dt, from_clip.duration,
                      graph_->states[from_].loop);
    // `to` clip starts at 0 on entry: its local time equals fade elapsed.
    blend_pos_ += dt;
    time_ = step(0.0F, blend_pos_, to_clip.duration,
                 graph_->states[to_].loop);
    if (blend_pos_ >= blend_dur_) {
      blending_ = false;
      active_ = to_;
      blend_weight_ = 0.0F;
    } else {
      blend_weight_ = blend_pos_ / blend_dur_;  // linear ramp
    }
  } else {
    const AnimClip& clip = clips[graph_->states[active_].clip_index];
    float dur = clip.duration;
    float nt = time_ + dt;
    if (!(dur > 0.0F)) {
      time_ = 0.0F;
    } else if (graph_->states[active_].loop) {
      nt = std::fmod(nt, dur);
      if (nt < 0.0F) {
        nt += dur;
      }
      time_ = nt;
    } else {
      time_ = nt >= dur ? dur : nt;
    }
  }
  CheckTransition();
  return true;
}

auto AnimatorRuntime::SampleStateAt(std::size_t state, float t,
                                    AnimPose pose) const noexcept -> void {
  const std::vector<AnimClip>& clips = *graph_->clips;
  (void)SampleAnimClip(clips[graph_->states[state].clip_index], t, pose);
}

auto AnimatorRuntime::SampleCurrent(AnimPose pose) noexcept
    -> std::size_t {
  if (graph_ == nullptr || graph_->clips == nullptr) {
    return 0;
  }
  if (!blending_) {
    return SampleAnimClip(
        (*graph_->clips)[graph_->states[active_].clip_index], time_, pose);
  }
  if (buf_a_t_.size() < joint_count_ * 3U ||
      buf_a_r_.size() < joint_count_ * 4U ||
      buf_b_t_.size() < joint_count_ * 3U ||
      buf_b_r_.size() < joint_count_ * 4U) {
    return 0;
  }
  AnimPose pa{buf_a_t_.data(), buf_a_r_.data(), buf_a_s_.data(),
              buf_a_set_.data(), joint_count_};
  AnimPose pb{buf_b_t_.data(), buf_b_r_.data(), buf_b_s_.data(),
              buf_b_set_.data(), joint_count_};
  (void)SampleAnimClip((*graph_->clips)[graph_->states[from_].clip_index],
                       from_time_, pa);
  (void)SampleAnimClip((*graph_->clips)[graph_->states[to_].clip_index], time_,
                       pb);
  BlendAnimPoses(pa, pb, blend_weight_, pose);
  return joint_count_;
}

auto AnimatorRuntime::StateNameAt(std::size_t i) const noexcept
    -> const char* {
  if (graph_ == nullptr || i >= graph_->states.size()) {
    return nullptr;
  }
  return graph_->states[i].name.c_str();
}

auto AnimatorRuntime::StateCount() const noexcept -> std::size_t {
  return graph_ == nullptr ? 0 : graph_->states.size();
}

}  // namespace engine::assets
