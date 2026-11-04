// hud_overlay.frag — P5-04-followup: sample the HUD texture verbatim.
//
// The HUD backbuffer holds display-ready sRGB bytes (the CPU golden
// compares them raw), so the shader outputs the texel UNCHANGED — no
// to_srgb encode (unlike mesh.frag/sprite.frag, which encode linear
// lighting). Blending is premultiplied src-over: the fixed-function
// blend state uses ONE/ONE_MINUS_SRC_ALPHA (see hud_overlay.hpp), and
// the shader passes the premultiplied texel straight through.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o /tmp/hud_overlay_frag.spv native/renderer/shaders/hud_overlay.frag
//   python3 native/renderer/shaders/embed_spv.py /tmp/hud_overlay_frag.spv native/renderer/shaders/hud_overlay_frag_spv.inc
#version 460

layout(set = 0, binding = 0) uniform sampler2D u_hud;

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main() {
  out_color = texture(u_hud, v_uv);
}
