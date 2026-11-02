// csm_depth.vert — P4-03 cascade depth prepass vertex stage (GLSL).
//
// Renders scene geometry into one 1024^2 cascade tile of the R32F depth
// atlas. Push constants carry the cascade's snapped light view-proj
// (row-major 4x4, from CascadeShadowMap) + tile origin in texels.
// The vertex shader transforms the fullscreen-triangle's analytic proxy
// geometry: the test draws the ground plane (z=0 world) and the pillar
// box as camera-facing quads positioned by gl_VertexIndex, exactly like
// pbr_shade.vert's fullscreen triangle (no vertex buffers).
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

// Analytic proxy scene (mirrors the CPU occluder + test scene):
// ground plane z=0 over [-8,8]^2 at light-space depth, pillar box
// centered at (0, 0.6, 0.5) with half extents (0.5, 0.5, 0.6).
// The vertex shader emits the quad corners in WORLD space; the
// rasterizer interpolates vLightPos, and the fragment stage applies the
// view-proj. (Doing the transform in the fragment stage keeps the vertex
// stage free of matrix-indexing Sonar complexity.)
void main() {
  // Fullscreen-triangle corners reused as quad corners via VertexIndex:
  // 0 -> (-1,-1), 1 -> (3,-1), 2 -> (-1,3); pillar quad uses the same
  // triangle clipped by the fragment stage to the pillar rect.
  vec2 corner = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                     (gl_VertexIndex == 2) ? 3.0 : -1.0);
  vec3 world;
  if (pc.proxy.x < 0.5) {
    // Ground quad: x in [-8,8], y in [-8,8], z = 0.
    world = vec3(corner.x * 8.0, corner.y * 8.0, 0.0);
  } else {
    // Pillar quad: x in [-0.5,0.5], y in [0.1,1.1], z = 0.5 (front face
    // toward the camera at +z; the CPU occluder matches this plane).
    world = vec3(corner.x * 0.5, 0.6 + corner.y * 0.5, 0.5);
  }
  vLightPos = world;
  // Clip position: map world XZ extent to clip directly (orthographic
  // along light -Z approximated by the CPU fit: the fragment stage does
  // the exact view-proj + tile mapping, this only needs to COVER the
  // tile — full-tile triangle, scissored to the tile rect).
  gl_Position = vec4(corner.x, corner.y, 0.0, 1.0);
}
