// csm_depth.vert — P4-03 cascade depth prepass vertex stage (GLSL).
//
// Renders scene geometry into one 1024^2 cascade tile of the R32F depth
// atlas. Push constants carry the cascade's snapped light view-proj
// (row-major 4x4, from CascadeShadowMap) + tile origin in texels.
// The vertex shader builds the analytic proxy geometry from
// gl_VertexIndex (no vertex buffers) and projects it through the cascade
// matrix.
//
// Depth convention: light-space [0,1], 1.0 = far plane (atlas cleared
// to 1.0). The fragment stage writes depth as R32F color (manual depth,
// no depth attachment — keeps the golden to color-only dynamic
// rendering like the P4-01/P4-02 goldens).
#version 460

layout(location = 0) out vec3 vLightPos;

layout(push_constant) uniform DepthPush {
  // Row-major light view-proj (16 floats from CascadeShadowMap).
  vec4 row0;
  vec4 row1;
  vec4 row2;
  vec4 row3;
  // Tile origin in texels (x, y) + atlas side in texels (z) + pad (w).
  vec4 tile;
  // Proxy geometry selector: 0 = ground quad, 1 = pillar quad.
  vec4 proxy;
}
pc;

// Analytic proxy scene (matches csm_shade.frag and the CPU test):
//   proxy 0: ground plane z=0 over [-8,8]^2 (6 vertices, 2 triangles)
//   proxy 1: pillar box x in [-0.5,0.5], y in [0.1,1.1], z in [0,0.5]:
//            top + 4 sides (30 vertices). Faces turned away from the sun
//            collapse to a point, so the sun-facing faces never overlap
//            in light space and last-writer-wins needs no depth test.
// Every vertex is projected through the cascade's light view-proj; the
// test sets the viewport to the cascade tile, so clip x/y land on the
// same texels csm_shade.frag reads.
const vec2 kQuad[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1),
                              vec2(0, 0), vec2(1, 1), vec2(0, 1));

vec3 boxCorner(int face, vec2 q, out vec3 n) {
  const vec3 lo = vec3(-0.5, 0.1, 0.0);
  const vec3 hi = vec3(0.5, 1.1, 0.5);
  vec3 p;
  if (face == 0) {  // top
    n = vec3(0, 0, 1);
    p = vec3(mix(lo.x, hi.x, q.x), mix(lo.y, hi.y, q.y), hi.z);
  } else if (face == 1) {  // -x
    n = vec3(-1, 0, 0);
    p = vec3(lo.x, mix(lo.y, hi.y, q.x), mix(lo.z, hi.z, q.y));
  } else if (face == 2) {  // +x
    n = vec3(1, 0, 0);
    p = vec3(hi.x, mix(lo.y, hi.y, q.x), mix(lo.z, hi.z, q.y));
  } else if (face == 3) {  // -y
    n = vec3(0, -1, 0);
    p = vec3(mix(lo.x, hi.x, q.x), lo.y, mix(lo.z, hi.z, q.y));
  } else {  // +y
    n = vec3(0, 1, 0);
    p = vec3(mix(lo.x, hi.x, q.x), hi.y, mix(lo.z, hi.z, q.y));
  }
  return p;
}

void main() {
  vec2 q = kQuad[gl_VertexIndex % 6];
  vec3 world;
  bool keep = true;
  if (pc.proxy.x < 0.5) {
    world = vec3(mix(-8.0, 8.0, q.x), mix(-8.0, 8.0, q.y), 0.0);
  } else {
    vec3 n;
    world = boxCorner(gl_VertexIndex / 6, q, n);
    // Row 2 is -toward_sun * inv_d, so -row2.xyz points at the sun.
    keep = dot(n, -pc.row2.xyz) > 0.0;
  }
  vLightPos = world;
  vec4 w = vec4(world, 1.0);
  float cz = dot(pc.row2, w);
  gl_Position = keep ? vec4(dot(pc.row0, w), dot(pc.row1, w), cz * 0.5 + 0.5, 1.0)
                     : vec4(0.0, 0.0, 2.0, 1.0);  // degenerate: culled
}
