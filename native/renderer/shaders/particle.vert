// particle.vert — P5-05: billboard expansion for one draw particle.
//
// One vertex per corner (4 verts/particle, triangle strip via index
// buffer): corner offsets (-0.5..0.5) scaled by the age-lerped size and
// the view-space right/up vectors, so quads always face the camera.
// Color = mix(start, end, age/life) with the matching alpha fade.
//
// Push constant: view-projection (64B) + viewport size (8B) + gravity
// echo (unused here, keeps the 80B sprite-style shape).
#version 460

layout(location = 0) in vec3 in_pos;     // particle center (world)
layout(location = 1) in vec4 in_color;   // age-lerped RGBA
layout(location = 2) in float in_size;    // age-lerped size
layout(location = 3) in vec2 in_corner;   // quad corner (-0.5..0.5)

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ParticleVertPush {
  mat4 view_proj;   // 64B column-major, Vulkan clip (Y flipped, depth 0..1)
  vec2 viewport_px; // 8B (unused by math, keeps 80B shape)
  vec2 pad;         // 8B
} g_push;

void main() {
  vec3 right = vec3(g_push.view_proj[0][0], g_push.view_proj[1][0],
                    g_push.view_proj[2][0]);
  vec3 up = vec3(g_push.view_proj[0][1], g_push.view_proj[1][1],
                 g_push.view_proj[2][1]);
  float rl = length(right);
  float ul = length(up);
  right = rl > 1e-6 ? right / rl : vec3(1.0, 0.0, 0.0);
  up = ul > 1e-6 ? up / ul : vec3(0.0, 1.0, 0.0);
  vec3 world = in_pos + (right * in_corner.x + up * in_corner.y) * in_size;
  gl_Position = g_push.view_proj * vec4(world, 1.0);
  out_color = in_color;
}
