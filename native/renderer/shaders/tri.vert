// tri.vert — P1-06: fullscreen-free triangle, no vertex input.
//
// Positions and colors come from constant arrays indexed by gl_VertexIndex
// (spec §4.7 pattern). Vertices (NDC):
//   v0 top-center    -> red   (1, 0, 0)
//   v1 bottom-left   -> green (0, 1, 0)
//   v2 bottom-right  -> blue  (0, 0, 1)
// Using gl_VertexIndex needs shaderDrawParameters, already enabled by
// VulkanDevice (M3-01 feature set, see vulkan_device.cpp).
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o /tmp/tri_vert.spv native/renderer/shaders/tri.vert
//   python3 native/renderer/shaders/embed_spv.py /tmp/tri_vert.spv native/renderer/shaders/tri_vert_spv.inc
#version 460

layout(location = 0) out vec3 v_color;

const vec2 kPositions[3] = vec2[3](
    vec2(0.0, -0.8),   // top-center (Vulkan NDC: -Y is up)
    vec2(-0.8, 0.8),   // bottom-left
    vec2(0.8, 0.8)     // bottom-right
);

const vec3 kColors[3] = vec3[3](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
  v_color = kColors[gl_VertexIndex];
  gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
}
