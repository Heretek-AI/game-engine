// cluster_shade.frag — P4-04 clustered-forward shade pass (GLSL, TEST-LOCAL).
//
// Same fullscreen-triangle shape as pbr_shade.frag (vUv in, linear-HDR
// out) but the light loop reads the P4-04 cluster grid instead of the
// forward+ tile grid: 16x9 tiles x 24 depth slices (3456 clusters), each
// an (offset, count) record into the packed light-index list. Light
// records carry range + spot-cone lanes the P4-01 blob lacks:
//
//   lightRows[li*4+0] = pos.xyz + range
//   lightRows[li*4+1] = color.xyz + intensity
//   lightRows[li*4+2] = spot dir.xyz + cone_cos (outer; <= -1 = point)
//   lightRows[li*4+3] = (unused, zero)
//
// Per light: range attenuation is the smooth quadratic window
// (1-(d/range)^2)^2 shared with ClusterRangeWindow (C1 at d = range,
// so cluster-boundary binning never pops); spot lights multiply by the
// cone falloff smoothstep(cos_outer, cos_outer+0.05, cos_angle) with a
// hard cut below cos_outer. Diffuse-only BRDF (albedo/pi, two-sided
// normal from the sphere record) keeps the golden about LIGHTS, not
// materials: every instance renders mid-grey (0.5) so the 64 colored
// point lights read as a clean hue grid.
//
// Depth slices split NDC z [0,1] uniformly (slice = floor(ndc*24)); the
// test scene is orthographic-flat (vis CPU upload) with ndc baked per
// instance in sceneRows[sBase+4].w... the sphere record stays
// (cx, cy, cz, radius) in PIXEL space (GoldenFillSceneRecord) and depth
// comes from push.depthPerInstance? No — depth is uniform per test
// (flat card at ndc 0.5): the push carries ndcZ so slice math stays
// honest without a per-instance channel.
//
// Bindings mirror the P4-01 test-owned set-1 shape (GoldenBuildSet1
// covers the module/layout half; the TU declares its own 6-binding
// set-1 layout: vis image + albedo-dummy + lights/grid/index):
//   0 vis image (rg32ui STORAGE_IMAGE), 2 albedo dummy sampler2D array
//   (unused, kept so the layout matches), 3 lights SSBO (vec4 rows),
//   4 cluster grid SSBO (offset/count u32 pairs), 5 index SSBO.
// Set 0 = bindless scene table (bindings 0 scene + 1 material SSBOs).
// Push PbrShadePush (64B) reused: frameSize.xy = W/H for the tile math
// (16x9 tiles), lightCount, maxLightsPerTile cap the per-cluster loop.
//
// Tiles: tile = floor(px * (16,9) / (W,H)); cluster =
// (slice*9 + tileY)*16 + tileX. Compiled OFFLINE with glslangValidator
// (M3-02 precedent); SPIR-V checked in as cluster_shade_frag_spv.inc.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 hdr;

layout(set = 0, binding = 0) readonly buffer Scene {
  uvec4 sceneRows[];
};
layout(set = 0, binding = 1) readonly buffer Materials {
  vec4 matRows[];
};
layout(set = 1, binding = 0, rg32ui) uniform readonly uimage2D visImg;
layout(set = 1, binding = 3) readonly buffer Lights {
  vec4 lightRows[];
};
layout(set = 1, binding = 4) readonly buffer LightGrid {
  uint grid[];
};
layout(set = 1, binding = 5) readonly buffer LightIndex {
  uint indices[];
};

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

const float kPi = 3.14159265;

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  uvec2 vis = imageLoad(visImg, px).rg;
  if (vis.x == 0xFFFFFFFFu) {
    hdr = vec4(0.02, 0.03, 0.05, 1.0);
    return;
  }
  uint sBase = vis.x * 6u;
  vec4 sphere = uintBitsToFloat(sceneRows[sBase + 4u]);
  // Per-pixel sphere surface (same disc math as pbr_shade.frag): pixels
  // missing the disc keep sky; survivors get a true normal + position.
  vec2 dc = (gl_FragCoord.xy - sphere.xy) / max(sphere.w, 1e-4);
  if (dot(dc, dc) > 1.0) {
    hdr = vec4(0.02, 0.03, 0.05, 1.0);
    return;
  }
  vec3 surf =
      vec3(gl_FragCoord.xy, sphere.z + sphere.w * sqrt(1.0 - dot(dc, dc)));
  vec3 n = normalize(surf - sphere.xyz);
  vec3 albedo = vec3(0.5);
  // 16x9 cluster tiles over the framebuffer (spec P4-04 grid).
  uvec2 tile = uvec2(uint(float(px.x) * 16.0 / pc.frameSize.x),
                     uint(float(px.y) * 9.0 / pc.frameSize.y));
  tile.x = min(tile.x, 15u);
  tile.y = min(tile.y, 8u);
  // Flat-card scene: uniform NDC depth from the camera push lane
  // (cameraPos.w carries ndcZ for this test: orthographic card).
  float ndcZ = clamp(pc.cameraPos.w, 0.0, 1.0);
  uint slice = min(uint(ndcZ * 24.0), 23u);
  uint cluster = (slice * 9u + tile.y) * 16u + tile.x;
  uint base = grid[cluster * 2u];
  uint range = grid[cluster * 2u + 1u];
  vec3 acc = vec3(0.0);
  uint steps = min(range, pc.maxLightsPerTile);
  for (uint i = 0u; i < steps; ++i) {
    uint li = indices[base + i];
    if (li >= pc.lightCount) {
      break;
    }
    vec4 posR = lightRows[li * 4u];
    vec4 colI = lightRows[li * 4u + 1u];
    vec4 spotD = lightRows[li * 4u + 2u];
    vec3 l = posR.xyz - surf;
    float dist = length(l);
    float lr = max(posR.w, 1e-4);
    if (dist >= lr) {
      continue;  // range cutoff (matches ClusterRangeWindow == 0)
    }
    l = l / max(dist, 1e-4);
    float t = dist / lr;
    float w = 1.0 - t * t;
    float atten = colI.w * w * w;
    // Spot cone: point when cone_cos <= -1; else hard cut + 0.05-cos
    // smoothstep edge (matches ClusterSpotTouchesSphere acceptance).
    if (spotD.w > -1.0) {
      vec3 toSurf = surf - posR.xyz;
      float d = length(toSurf);
      vec3 axis = normalize(spotD.xyz + vec3(1e-6, 0.0, 0.0));
      float cosA = dot(toSurf / max(d, 1e-4), axis);
      if (cosA < spotD.w) {
        continue;
      }
      float edge = smoothstep(spotD.w, min(spotD.w + 0.05, 1.0), cosA);
      atten *= edge;
    }
    float nDotL = abs(dot(n, l));
    acc += albedo / kPi * (colI.xyz * atten) * nDotL;
  }
  hdr = vec4(acc, 1.0);
}
