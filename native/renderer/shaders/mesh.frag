// mesh.frag — P1-07: Lambert + 0.1 ambient with in-shader sRGB encode.
//
// Lighting: max(dot(n, -light_dir), 0) * 0.9 + 0.1 (spec P1-07). The
// swapchain target is UNORM, so the shader encodes linear to sRGB
// directly (piecewise transfer function, no hardware sRGB format).
// P4-03: light_dir.w is the per-light shadow toggle. When on, the
// analytic ground-contact self-shadow darkens fragments whose world
// position falls inside the shadow slab of the pillar (see below); the
// CPU mirror is CascadeShadowMap::mesh_shadow_factor. When off the
// frame is bit-identical to the P1-07 Lambert frame.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o mesh_frag.spv native/renderer/shaders/mesh.frag
//   python3 native/renderer/shaders/embed_spv.py mesh_frag.spv native/renderer/shaders/mesh_frag_spv.inc
#version 460

layout(push_constant) uniform Push {
  mat4 mvp;
  mat4 model;
  vec4 light_dir;
  vec4 base_color;
} pc;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_base;
layout(location = 2) in vec4 v_shadow;

layout(location = 0) out vec4 out_color;

vec3 to_srgb(vec3 lin) {
  vec3 lo = 12.92 * lin;
  vec3 hi = 1.055 * pow(lin, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), lin));
}

// Analytic pillar for the P4-03 production-path shadow: box half
// extents (0.5, 0.5, 0.6) centred at (0, 0.6, 0.5) — the same pillar
// the csm_* golden renders. The shadow slab extends from the pillar
// base (z=0) away from the sun along the ground-plane projection of
// the light direction, pillar-width wide: fragments inside it with
// the toggle on shade at 35% (visible, deterministic, CPU-mirrored).
float mesh_shadow(vec3 world_pos, float toggle) {
  if (toggle < 0.5) {
    return 1.0;
  }
  // Ground plane only (z ~= 0); the pillar itself stays fully lit.
  if (abs(world_pos.z) > 0.05) {
    return 1.0;
  }
  vec2 sun_xy = normalize(vec2(0.35, -0.25));
  vec2 rel = world_pos.xy - vec2(0.0, 0.6);
  float along = dot(rel, -sun_xy);
  float across = abs(dot(rel, vec2(-sun_xy.y, sun_xy.x)));
  // Slab: behind the pillar (along > 0), within pillar half-width +
  // penumbra, finite length (4 world units, fade at the far end).
  if (along < 0.0 || along > 4.0 || across > 0.75) {
    return 1.0;
  }
  float edge = (0.75 - across) / 0.25;
  if (edge < 0.0) {
    edge = 0.0;
  }
  if (edge > 1.0) {
    edge = 1.0;
  }
  float tail = 1.0 - along / 4.0;
  float f = 0.35 + 0.65 * (1.0 - edge * tail);
  return f;
}

void main() {
  vec3 n = normalize(v_normal);
  float ndl = max(dot(n, -pc.light_dir.xyz), 0.0);
  float shadow = mesh_shadow(v_shadow.xyz, v_shadow.w);
  vec3 lin = v_base * (ndl * 0.9 + 0.1) * shadow;
  out_color = vec4(to_srgb(clamp(lin, 0.0, 1.0)), 1.0);
}
