// pbr_shade.frag — P4-01 forward+ fullscreen shade pass (GLSL).
//
// Reads the visibility buffer (instance, primitive), fetches the
// instance's 64B SceneMaterial record from the bindless material SSBO,
// then evaluates the metallic-roughness PBR BRDF (GGX + Schlick +
// Smith, mirrored from pbr_common.slang) over the forward+ per-tile
// light list. Writes linear-HDR radiance; the post pass tonemaps.
//
// Why GLSL, not the M3-03 slang source: slangc 2026.20 (the P4-00-pinned
// compiler) emits SPIR-V with physical-addressing OpStore that lavapipe's
// spirv-val rejects (VUID-08737 "not a logical pointer" — even the
// UNMODIFIED M3-03 blob recompiled with 2026.20 fails; the checked-in
// blobs predate it). The M3-02 precedent (cull.comp/hiz.comp via
// glslangValidator) compiles clean on the same driver, so the shade and
// post stages move to GLSL while the visibility pass stays slang.
// Math parity with pbr_common.slang is asserted by the CPU mirror in
// test_pbr_materials.cpp (brdf_reference vs checked ACES/sRGB values).
//
// Bindings (set 0 = bindless scene table from M3-02):
//   0 scene SSBO (96B records: world matrix + sphere + material_slot),
//   1 material SSBO (P4-01: 64B SceneMaterial records),
//   2 visibility texture (R32G32_UINT: instance, primitive),
//   3 lights SSBO, 4 light grid SSBO, 5 light index SSBO.
// Set 1 (P4-01 texture set):
//   2 albedo texture array (sampled images), 3 sampler array.
// Push PbrShadePush (64B: camera pos, frame size, light count, flags);
// layout matches pbr_pipeline.hpp byte-for-byte.
//
// Forward+ tiling: 16px tiles; grid record = (offset, count) into the
// light-index list. Untextured materials (albedo slot 0xFFFF) shade from
// the base_color factor; bound albedo textures modulate it.
#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 hdr;

layout(set = 0, binding = 0) readonly buffer Scene {
  // SceneInstance record (scene_ssbo.hpp, 96B): world matrix (64B floats),
  // sphere (16B floats), ids (16B UINT32: mesh, material_slot, flags, pad).
  // Declared as uvec4 rows: float lanes convert via uintBitsToFloat, id
  // lanes read as exact uints. (A vec4 declaration would reinterpret the
  // id bits as denormal floats — uint(denormal) is 0, silently pinning
  // every instance to material 0.)
  uvec4 sceneRows[];
};
layout(set = 0, binding = 1) readonly buffer Materials {
  vec4 matRows[];
};
// Visibility image: test-owned set-1 binding 0 (SAMPLED_IMAGE; the pass
// texel-fetches it, no sampler). Set-0 binding 2 stays the bindless
// COMBINED array for the P4-06 frame wiring (unused by this golden).
// Visibility: STORAGE_IMAGE (texel-fetch, no sampler). A sampled-image
// declaration compiles to COMBINED under this glslang (usampler2D scalar
// or array alike); the storage-image path keeps the descriptor type exact.
layout(set = 1, binding = 0, rg32ui) uniform readonly uimage2D visImg;
// Light data rides set 1 (test-owned layout): the bindless set-0 only
// carries scene (0) + material (1) SSBOs; bindings 3/4/5 do not exist
// there (2 = texture array, 3 = sampler array).
layout(set = 1, binding = 3) readonly buffer Lights {
  vec4 lightRows[];
};
layout(set = 1, binding = 4) readonly buffer LightGrid {
  uint grid[];
};
layout(set = 1, binding = 5) readonly buffer LightIndex {
  uint indices[];
};
layout(set = 1, binding = 2) uniform sampler2D albedoTex[];

layout(push_constant) uniform ShadePush {
  vec4 cameraPos;
  vec4 frameSize;
  vec4 invFrameSize;
  uint lightCount;
  uint flags;
  uint maxLightsPerTile;
  uint pad;
}
pc;

const uint kNoTexture = 0xFFFFu;
const float kPi = 3.14159265;

// GGX normal-distribution function (Trowbridge-Reitz).
float dGgx(float nDotH, float roughness) {
  float a = roughness * roughness;
  float a2 = a * a;
  float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
  return a2 / (kPi * denom * denom + 1e-6);
}

// Schlick-GGX geometry term (single direction).
float g1(float nDotV, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return nDotV / (nDotV * (1.0 - k) + k + 1e-6);
}

// Metallic-roughness BRDF: diffuse Lambert scaled by (1-metallic) +
// specular GGX. Mirrors PbrBrdf in pbr_common.slang term-for-term.
vec3 brdf(vec3 albedo, float metallic, float roughness, vec3 n, vec3 v,
          vec3 l, vec3 lightColor) {
  vec3 h = normalize(v + l);
  float nDotL = max(dot(n, l), 0.0);
  float nDotV = max(dot(n, v), 0.0);
  float nDotH = max(dot(n, h), 0.0);
  float vDotH = max(dot(v, h), 0.0);
  vec3 f0 = mix(vec3(0.04), albedo, metallic);
  float d = dGgx(nDotH, roughness);
  float g = g1(nDotV, roughness) * g1(nDotL, roughness);
  vec3 f = f0 + (vec3(1.0) - f0) * pow(1.0 - vDotH, 5.0);
  vec3 spec = (d * g) * f / max(4.0 * nDotV * nDotL, 1e-4);
  vec3 kd = (1.0 - metallic) * (vec3(1.0) - f) * (albedo / kPi);
  return (kd + spec) * lightColor * nDotL;
}

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  uvec2 vis = imageLoad(visImg, px).rg;
  // Sky: no instance wrote this pixel (0xFFFFFFFF sentinel from the
  // visibility clear value).
  if (vis.x == 0xFFFFFFFFu) {
    hdr = vec4(0.02, 0.03, 0.05, 1.0);
    return;
  }
  // Scene record: 6 uvec4 rows (world 4 + sphere 1 + ids 1). Float lanes
  // (sphere) convert via uintBitsToFloat; the ids row (mesh, material
  // slot, flags, pad) reads as exact uints — no float reinterpretation.
  uint sBase = vis.x * 6u;
  vec4 sphere = uintBitsToFloat(sceneRows[sBase + 4u]);
  uint materialSlot = sceneRows[sBase + 5u].y;
  // Material record: 4 vec4 rows (baseColor | metal/rough | u32lanes |
  // emissive). The u32 lanes (albedo slot, sampler, flags) ride as
  // bit-exact uints — floatBitsToUint, never float conversion.
  uint mBase = materialSlot * 4u;
  vec3 albedo = matRows[mBase].xyz;
  vec2 mr = matRows[mBase + 1u].xy;
  uvec4 lanes = floatBitsToUint(vec4(matRows[mBase + 2u].xy,
                                     matRows[mBase + 2u].zw));
  uint albedoSlot = lanes.x;
  if (albedoSlot != kNoTexture) {
    vec2 shadeUv = gl_FragCoord.xy * pc.invFrameSize.xy;
    albedo *= texture(albedoTex[nonuniformEXT(albedoSlot)], shadeUv).xyz;
  }
  float metallic = mr.x;
  float roughness = clamp(mr.y, 0.04, 1.0);
  // Per-pixel sphere surface: the instance record holds the sphere center
  // (xy), depth (z), and radius (w) in image space. Intersect the view ray
  // (orthographic along +z from the pixel) with the sphere; pixels missing
  // the disc keep the sky color. This gives a true per-pixel normal, view
  // vector, and light vector — a constant normal would black out every
  // sphere half whose fixed normal faces away from the light.
  vec2 dc = (gl_FragCoord.xy - sphere.xy) / max(sphere.w, 1e-4);
  if (dot(dc, dc) > 1.0) {
    hdr = vec4(0.02, 0.03, 0.05, 1.0);
    return;
  }
  vec3 surf =
      vec3(gl_FragCoord.xy, sphere.z + sphere.w * sqrt(1.0 - dot(dc, dc)));
  vec3 n = normalize(surf - sphere.xyz);
  vec3 v = normalize(pc.cameraPos.xyz - surf);
  uvec2 tile = uvec2(px) / 16u;
  uint tilesX = (uint(pc.frameSize.x) + 16u - 1u) / 16u;
  uint gridIdx = tile.y * tilesX + tile.x;
  uint base = grid[gridIdx * 2u];
  uint range = grid[gridIdx * 2u + 1u];
  vec3 acc = vec3(0.0);
  uint steps = min(range, pc.maxLightsPerTile);
  for (uint i = 0u; i < steps; ++i) {
    uint li = indices[base + i];
    if (li >= pc.lightCount) {
      break;
    }
    vec4 posR = lightRows[li * 2u];
    vec4 colI = lightRows[li * 2u + 1u];
    vec3 l = posR.xyz - surf;
    float dist = length(l);
    l = l / max(dist, 1e-4);
    float atten = colI.w / max(dist * dist, 1e-4);
    acc += brdf(albedo, metallic, roughness, n, v, l, colI.xyz * atten);
  }
  hdr = vec4(acc, 1.0);
}
