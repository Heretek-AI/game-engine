// height_fog.hpp — P4-11: height fog as a PostProcess effect.
//
// Height fog attenuates toward a fog color based on the fragment's world
// height: fragments below `height` fade toward `color` with strength
// `density`, falling off exponentially with distance above `height` at
// rate `falloff`. Effect id `kHeightFogEffectId` plugs the effect into
// the ECS `engine::ecs::PostProcess` ordered slot list (slot order IS
// execution order; see ecs_render_components.hpp).
//
// CPU reference (HeightFogCpu) mirrors height_fog.frag term-for-term so
// the GPU pass and the contract test agree by construction. Toggle-off:
// density 0 returns the input color unchanged (provable skip — no
// separate bypass path needed).
//
// Compiles SDK-free (plain data + math, no GPU); GPU pass wiring stays
// in the test TU next to the P4-08 frame wiring.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// P4-11 effect id slot for height fog in the `engine::ecs::PostProcess`
// `effect_ids` array. NOTE: this constant is claimed by height fog;
// sibling P4-11 slices (LUT/vignette, t_4266178f) must take DISTINCT ids
// (see docs/notes/P4-11.md effect-id table) so the ordered list never
// runs two effects under one id.
inline constexpr std::uint32_t kHeightFogEffectId = 1;

// ECS-side settings twin (blittable, 16B, 4-byte aligned like the other
// render components — NOT alignas(64): only built-in transform columns
// get 64B per repo invariants).
//   density: fog strength at/below `height` (0 = toggle-off: no-op).
//   falloff: exponential falloff rate above `height` (per world unit).
//   height: world-space height of full-density fog.
//   flags: reserved (bit 0 = enable; 0 keeps density-0 semantics even
//     when density > 0, so a disabled component never fogs).
struct HeightFog {
  float density{0.0f};
  float falloff{0.1f};
  float height{0.0f};
  std::uint32_t flags{0};
};
static_assert(sizeof(HeightFog) == 16, "height fog settings are 16B");
static_assert(alignof(HeightFog) == 4, "height fog is 4-byte aligned");

// Fog color + factor push mirror (must match height_fog.frag
// byte-for-byte; <= 128B).
//   color[3]: linear-HDR fog color; density/falloff/height mirror the
//     component fields above.
struct HeightFogPush {
  float color[3]{0.5f, 0.6f, 0.7f};
  float density{0.0f};
  float falloff{0.1f};
  float height{0.0f};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(HeightFogPush) == 32, "height fog push is 32B");

// Clamp + validate one settings struct in place (NaN/garbage fails
// closed with kInvalidArgument; negative density/falloff clamp to 0).
// Null -> kInvalidArgument.
auto ENGINE_CORE_API HeightFogValidate(HeightFog* fog) noexcept -> Status;

// CPU reference: fog factor for one fragment at world height `y`
// (mirrors height_fog.frag term-for-term):
//   factor = density                     when y <= height,
//   factor = density * exp(-falloff * (y - height))   otherwise.
// Returns 0 when density is 0 (toggle-off) or the component is
// disabled (flags bit 0 clear).
[[nodiscard]] inline auto HeightFogFactorCpu(const HeightFog& fog,
                                            float y) noexcept -> float {
  if ((fog.flags & 1u) == 0u || fog.density <= 0.0f) {
    return 0.0f;
  }
  if (y <= fog.height) {
    return fog.density;
  }
  const float d = y - fog.height;
  const float fall = (fog.falloff > 0.0f) ? fog.falloff : 0.0f;
  return fog.density * std::exp(-fall * d);
}

// CPU reference: mix one linear-HDR texel toward the fog color
// (mirrors height_fog.frag: out = mix(hdr, fog_color, clamp(f,0,1))).
[[nodiscard]] inline auto HeightFogMixCpu(float hdr, float fog_color,
                                         float factor) noexcept -> float {
  const float f = (factor < 0.0f) ? 0.0f : ((factor > 1.0f) ? 1.0f : factor);
  return hdr + (fog_color - hdr) * f;
}

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif
