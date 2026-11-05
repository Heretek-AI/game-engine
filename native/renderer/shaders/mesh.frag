// mesh.frag — P1-07: Lambert + 0.1 ambient with in-shader sRGB encode.
//
// Lighting: max(dot(n, -light_dir), 0) * 0.9 + 0.1 (spec P1-07). The
// swapchain target is UNORM, so the shader encodes linear to sRGB
// directly (piecewise transfer function, no hardware sRGB format).
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

// P4-07 part 2 GI probe volume (set 0, bound by record_mesh every draw).
// boxSizeGain.w is the indirect gain (0 = GI off, no visual change);
// the baker pre-scales irradiance, so w is normally 1.
layout(set = 0, binding = 0) uniform sampler3D probeVol;
layout(set = 0, binding = 1) uniform ProbeBox {
  vec4 boxMin;
  vec4 boxSizeGain;
} probeBox;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec3 v_base;
layout(location = 2) in vec3 v_world;

layout(location = 0) out vec4 out_color;

vec3 to_srgb(vec3 lin) {
  vec3 lo = 12.92 * lin;
  vec3 hi = 1.055 * pow(lin, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), lin));
}

void main() {
  vec3 n = normalize(v_normal);
  float ndl = max(dot(n, -pc.light_dir.xyz), 0.0);
  vec3 lin = v_base * (ndl * 0.9 + 0.1);
  // GI indirect: baked irradiance sampled at the fragment world pos.
  // Uniform branch on gain (off by default): zero-cost, bit-identical
  // frame when GI is disabled. textureLod (not texture): the volume is
  // single-mip (NEAREST, maxLod 0), so lod 0 is exact — and explicit-LOD
  // fetches are derivative-free, which MoltenVK requires inside
  // divergent control flow (implicit-derivative texture() fails the MSL
  // transpiler there: undeclared probeVolSmplr).
  float gi_gain = probeBox.boxSizeGain.w;
  if (gi_gain > 0.0) {
    vec3 uvw = (v_world - probeBox.boxMin.xyz) / probeBox.boxSizeGain.xyz;
    vec3 irr =
        textureLod(probeVol, clamp(uvw, vec3(0.0), vec3(1.0)), 0.0).rgb;
    lin += v_base * (irr * (gi_gain / 3.14159265));
  }
  out_color = vec4(to_srgb(clamp(lin, 0.0, 1.0)), 1.0);
}
