// csm_shade.frag — P4-03 shadowed shade pass (GLSL).
//
// Analytic pillar + ground scene lit by ONE directional light with 4
// cascades, PCF 3x3, per-light shadow toggle. Mirrors
// CascadeShadowMap::pcf_shadow / shade_factor term-for-term:
//
//   cascade = select by view depth (cascade far planes in push)
//   uv = light view-proj * world -> tile rect in the 1024-tile atlas
//   factor = mean over the 3x3 around uv of
//       (receiver_depth <= texel + bias) ? 1 : 0
//   shadows_enabled == 0 -> factor = 1.0 exactly
//
// Scene (analytic, matches the csm_depth proxy + CPU test):
//   ground plane z=0 (albedo 0.5 grey), pillar front face z=0.5 plane
//   rect x in [-0.5,0.5], y in [0.1,1.1] (albedo 0.8).
// Camera: orthographic top view at height H (push cam_pos.z), looking
// down -Z; world xy = (uv - 0.5) * frame_size. View depth = H - z.
// Light: direction (push light_dir, normalized), intensity * color.
//
// Output: linear-HDR radiance (direct * shadow + emissive 0) for the
// post pass to tonemap — same contract as pbr_shade.frag.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 hdr;

// Depth atlas: R32F storage image (texelFetch, no sampler — exact
// texels, no filtering; PCF is the explicit 3x3 below).
layout(set = 1, binding = 0, r32f) uniform readonly image2D depthAtlas;

layout(push_constant) uniform ShadePush {
  // Camera: world xy center (x, y), height H (z), frame W world (w).
  vec4 cam;
  // Active cascade snapped ortho rows (row-major view-proj; the test
  // renders cascade 0 as the active cascade for the golden).
  vec4 row0;
  vec4 row1;
  vec4 row2;
  // Active cascade tile origin in texels (x, y), atlas side (z),
  // receiver bias (w).
  vec4 tile;
  // Light direction (x, y, z) + shadows_enabled flag (w: 1/0).
  vec4 light;
  // Light color (x, y, z) + intensity (w). Frame height world in
  // frameH (x). Total: 8 vec4 = 128B (push limit).
  vec4 lightCol;
  vec4 frameH;
}
pc;

void main() {
  float frameW = pc.cam.w;
  float frameH = pc.frameH.x;
  vec2 worldXy = (vUv - vec2(0.5)) * vec2(frameW, frameH) + pc.cam.xy;

  // Pillar rect test (front face plane z=0.5, albedo 0.8; else ground
  // z=0, albedo 0.5).
  bool pillar = abs(worldXy.x) <= 0.5 && worldXy.y >= 0.1 && worldXy.y <= 1.1;
  float surfZ = pillar ? 0.5 : 0.0;
  vec3 albedo = pillar ? vec3(0.8) : vec3(0.5);

  // NOTE: cascade index selection lives on the CPU (cascade_index);
  // this golden renders cascade 0 as the active cascade. Receiver
  // depth is light-space (view depth only documents the convention).

  // World -> light clip via the active cascade matrix.
  vec4 w = vec4(worldXy.x, worldXy.y, surfZ, 1.0);
  float cx = dot(pc.row0, w);
  float cy = dot(pc.row1, w);
  float cz = dot(pc.row2, w);
  float receiver = cz * 0.5 + 0.5;

  // Clip [-1,1] -> tile texels: tile.xy origin, tile 1024 wide.
  vec2 tileUv = vec2(cx * 0.5 + 0.5, cy * 0.5 + 0.5);
  vec2 texelF = pc.tile.xy + tileUv * 1024.0;
  ivec2 base = ivec2(floor(texelF));

  // PCF 3x3: mean of lit comparisons (matches pcf_shadow: receiver <=
  // texel + bias). texelFetch outside the atlas returns 1.0 (far).
  float lit = 0.0;
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      ivec2 t = base + ivec2(ox, oy);
      float stored = 1.0;
      if (t.x >= 0 && t.y >= 0 && t.x < int(pc.tile.z) && t.y < int(pc.tile.z)) {
        stored = imageLoad(depthAtlas, t).x;
      }
      if (receiver <= stored + pc.tile.w) {
        lit += 1.0;
      }
    }
  }
  float factor = lit / 9.0;
  if (pc.light.w < 0.5) {
    factor = 1.0;
  }

  // NdotL with the fixed sun: light dir points FROM the sun (downward
  // tilted); surfaces face +Z (up toward camera/sun side).
  vec3 l = normalize(-pc.light.xyz);
  float nDotL = max(l.z, 0.0);
  vec3 direct = albedo * pc.lightCol.xyz * pc.lightCol.w * nDotL * factor;
  hdr = vec4(direct, 1.0);
}
