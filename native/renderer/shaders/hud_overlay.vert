// hud_overlay.vert — P5-04-followup: fullscreen textured triangle, no vertex input.
//
// Positions + UVs come from constant arrays indexed by gl_VertexIndex
// (spec §4.7 pattern, same idiom as tri.vert). The triangle covers the
// whole viewport; UVs map the HUD texture 1:1 (v=0 at the top to match
// the CPU backbuffer row order).
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o /tmp/hud_overlay_vert.spv native/renderer/shaders/hud_overlay.vert
//   python3 native/renderer/shaders/embed_spv.py /tmp/hud_overlay_vert.spv native/renderer/shaders/hud_overlay_vert_spv.inc
#version 460

layout(location = 0) out vec2 v_uv;

const vec2 kPositions[3] = vec2[3](
    vec2(-1.0, -1.0),  // top-left (Vulkan NDC: -Y is up)
    vec2(3.0, -1.0),   // top-right (oversized: covers the viewport)
    vec2(-1.0, 3.0)    // bottom-left (oversized: covers the viewport)
);

const vec2 kUvs[3] = vec2[3](
    vec2(0.0, 0.0),  // top-left of the HUD texture
    vec2(2.0, 0.0),  // oversized U (clipped to 1.0 by the viewport)
    vec2(0.0, 2.0)   // oversized V (clipped to 1.0 by the viewport)
);

void main() {
  v_uv = kUvs[gl_VertexIndex];
  gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
}
