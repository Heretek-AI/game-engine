// test_height_fog.cpp — P4-11 CPU contract: height fog settings +
// ordered PostProcess dispatch.
//
// Covers: header size asserts, HeightFogValidate (null/NaN/clamp),
// HeightFogFactorCpu (below/at/above height, toggle-off, disabled),
// HeightFogMixCpu (identity at 0, full replace at >= 1, clamping), and
// the ordered-list dispatch: the PostProcess `effect_ids` array runs in
// slot order — fog applies iff its id appears in the live prefix,
// wherever it sits in the order.
//
// Runs on stub builds too (no GPU, no window — pure frame math). The
// GPU golden (fog through the PostProcess ordering) is owned by
// t_6eb1aabe; this TU proves the CPU-side contract it builds on.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/renderer/height_fog.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto close(float got, float want) -> bool {
  const float d = got - want;
  const float a = (d < 0.0f) ? -d : d;
  const float tol = 1e-5f * ((want < 0.0f ? -want : want) + 1.0f);
  return a <= tol;
}

}  // namespace

// Minimal ordered-dispatch model: walk the live prefix of the
// PostProcess slot list in array order; when the fog id appears, apply
// HeightFogMixCpu at the fragment height. Array order IS execution
// order (no priority field — see ecs_render_components.hpp).
static auto run_post_stack(const engine::ecs::PostProcess& stack,
                           const engine::renderer::HeightFog& fog, float hdr,
                           float fog_color, float y) -> float {
  float out = hdr;
  const std::uint32_t n =
      (stack.effect_count <= engine::ecs::kPostProcessMaxEffects)
          ? stack.effect_count
          : engine::ecs::kPostProcessMaxEffects;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (stack.effect_ids[i] == engine::renderer::kHeightFogEffectId) {
      const float f = engine::renderer::HeightFogFactorCpu(fog, y);
      out = engine::renderer::HeightFogMixCpu(out, fog_color, f);
    }
  }
  return out;
}

auto main() -> int {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  using engine::renderer::HeightFog;
  using engine::renderer::HeightFogFactorCpu;
  using engine::renderer::HeightFogMixCpu;
  using engine::renderer::HeightFogPush;
  using engine::renderer::HeightFogValidate;
  using engine::renderer::kHeightFogEffectId;

  // Layout: settings 16B align 4, push 32B; effect id is nonzero so id
  // 0 keeps meaning "empty slot".
  check(sizeof(HeightFog) == 16, "fog: settings 16B");
  check(alignof(HeightFog) == 4, "fog: settings align 4");
  check(sizeof(HeightFogPush) == 32, "fog: push 32B");
  check(kHeightFogEffectId != 0, "fog: effect id nonzero");
  check(sizeof(engine::ecs::PostProcess) == 36, "fog: PostProcess 36B");

  // Defaults: density 0 = toggle-off; falloff 0.1; flags 0 (disabled).
  const HeightFog dflt{};
  check(dflt.density == 0.0f, "fog: default density 0 (toggle-off)");
  check(dflt.flags == 0u, "fog: default disabled");

  // Validate: null fails, NaN fails, negatives clamp to 0.
  check(!HeightFogValidate(nullptr).ok(), "fog: null rejected");
  HeightFog bad{};
  bad.density = std::numeric_limits<float>::quiet_NaN();  // deliberate NaN
  check(!HeightFogValidate(&bad).ok(), "fog: NaN rejected");
  HeightFog clamp_me{};
  clamp_me.density = -1.0f;
  clamp_me.falloff = -2.0f;
  check(HeightFogValidate(&clamp_me).ok(), "fog: clamp ok");
  check(clamp_me.density == 0.0f, "fog: negative density clamped");
  check(clamp_me.falloff == 0.0f, "fog: negative falloff clamped");

  // Factor: disabled flags -> 0 even with density set (toggle-off);
  // enabled + below/at height -> full density; above -> exponential.
  HeightFog fog{};
  fog.density = 0.5f;
  fog.falloff = 0.1f;
  fog.height = 10.0f;
  fog.flags = 0u;
  check(HeightFogFactorCpu(fog, 5.0f) == 0.0f, "fog: disabled is no-op");
  fog.flags = 1u;
  check(close(HeightFogFactorCpu(fog, 5.0f), 0.5f),
        "fog: below height is full density");
  check(close(HeightFogFactorCpu(fog, 10.0f), 0.5f),
        "fog: at height is full density");
  const float want_above =
      0.5f * std::exp(-0.1f * (15.0f - 10.0f));  // mirror of the formula
  check(close(HeightFogFactorCpu(fog, 15.0f), want_above),
        "fog: above height falls off exponentially");
  HeightFog off{};
  off.flags = 1u;
  check(HeightFogFactorCpu(off, 0.0f) == 0.0f, "fog: density 0 is no-op");

  // Mix: factor 0 is identity, >= 1 is full replace, out-of-range
  // factors clamp.
  check(close(HeightFogMixCpu(0.8f, 0.2f, 0.0f), 0.8f),
        "fog: mix factor 0 is identity");
  check(close(HeightFogMixCpu(0.8f, 0.2f, 1.0f), 0.2f),
        "fog: mix factor 1 fully fogs");
  check(close(HeightFogMixCpu(0.8f, 0.2f, 5.0f), 0.2f),
        "fog: mix factor > 1 clamps");
  check(close(HeightFogMixCpu(0.8f, 0.2f, -1.0f), 0.8f),
        "fog: mix factor < 0 clamps");
  // Toggle-off end to end: density 0 leaves HDR untouched.
  check(close(HeightFogMixCpu(0.8f, 0.2f, HeightFogFactorCpu(off, 0.0f)),
              0.8f),
        "fog: toggle-off leaves HDR untouched");

  // Ordered dispatch: fog applies through the PostProcess slot list in
  // array order, wherever its id sits; absent id = untouched HDR;
  // slots past effect_count are ignored.
  engine::ecs::PostProcess stack{};
  stack.effect_count = 1;
  stack.effect_ids[0] = kHeightFogEffectId;
  check(close(run_post_stack(stack, fog, 0.8f, 0.2f, 5.0f),
              HeightFogMixCpu(0.8f, 0.2f, 0.5f)),
        "fog: slot 0 applies through ordering");
  engine::ecs::PostProcess late{};
  late.effect_count = 3;
  late.effect_ids[0] = 7u;  // foreign effect ids (sibling slices)
  late.effect_ids[1] = 9u;
  late.effect_ids[2] = kHeightFogEffectId;
  check(close(run_post_stack(late, fog, 0.8f, 0.2f, 5.0f),
              HeightFogMixCpu(0.8f, 0.2f, 0.5f)),
        "fog: later slot applies through ordering");
  engine::ecs::PostProcess none{};
  none.effect_count = 2;
  none.effect_ids[0] = 7u;
  none.effect_ids[1] = 9u;
  check(close(run_post_stack(none, fog, 0.8f, 0.2f, 5.0f), 0.8f),
        "fog: absent id leaves HDR untouched");
  engine::ecs::PostProcess stale{};
  stale.effect_count = 0;
  stale.effect_ids[0] = kHeightFogEffectId;  // live id, dead prefix
  check(close(run_post_stack(stale, fog, 0.8f, 0.2f, 5.0f), 0.8f),
        "fog: slots past effect_count ignored");

  std::printf("[%s] engine_height_fog (%d failures)\n",
              failures == 0 ? "PASS" : "FAIL", failures);
  return (failures == 0) ? 0 : 1;
}
