// csm_depth.frag — P4-03 cascade depth prepass fragment stage (GLSL).
//
// Applies the cascade's snapped light view-proj (push rows) to the
// interpolated world position and writes light-space depth [0,1] as
// R32F color. Pixels outside the cascade tile rect are discarded (the
// test scissors to the tile anyway; the discard keeps overlapping
// proxy triangles from bleeding across tiles).
//
// Mirrors CascadeShadowMap CPU fit: clip = view_proj * world;
// depth01 = clip.z * 0.5 + 0.5 (OpenGL-style; the CPU test maps its
// analytic depths through the same formula).
#version 460

layout(location = 0) in vec3 vLightPos;
layout(location = 0) out vec4 outDepth;

layout(push_constant) uniform DepthPush {
  vec4 row0;
  vec4 row1;
  vec4 row2;
  vec4 row3;
  vec4 tile;
  vec4 proxy;
}
pc;

void main() {
  vec4 w = vec4(vLightPos, 1.0);
  float cx = dot(pc.row0, w);
  float cy = dot(pc.row1, w);
  float cz = dot(pc.row2, w);
  // Outside the snapped ortho box -> no depth (leave the cleared 1.0).
  if (cx < -1.0 || cx > 1.0 || cy < -1.0 || cy > 1.0) {
    discard;
  }
  float depth01 = cz * 0.5 + 0.5;
  outDepth = vec4(depth01, 0.0, 0.0, 1.0);
}
