// mesh_skinned.vert — P5-02: indexed mesh vertex shader with 4-weight GPU skinning.
//
// Vertex input (binding 0 interleaved, SDK-side struct MeshVertex):
//   location 0: in_pos    vec3 (offset 0)
//   location 1: in_normal  vec3 (offset 12)
//   location 2: in_uv      vec2 (offset 24)
//
// Skin input (binding 1 interleaved, SDK-side struct SkinnedVertexExtra):
//   location 3: in_joints  uvec4 joint indices (offset 0)
//   location 4: in_weights vec4  blend weights (offset 16)
//
// Joint matrices ride a uniform buffer (set 0, binding 0 — NOT push
// constants: 8 joints x 64B = 512B overflows the 128B minimum
// maxPushConstantsSize, and the legacy limit caps MeshPush at 160B).
// Skinning math:
//   skinned = sum_i (w_i * joint[i] * vec4(in_pos,1)); normal likewise
//   with mat3(joint[i]); both normalized by the live weight sum.
// Fallback: when the total live weight == 0 the vertex keeps its bind
// pose (unskinned).
//
// Dedicated skinned variant: the legacy mesh.vert has no skin interface
// at all (separate SPIR-V), so neither pipeline depends on specialization
// constants to drop statically-used inputs — MoltenVK included.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o mesh_skinned_vert.spv native/renderer/shaders/mesh_skinned.vert
//   python3 native/renderer/shaders/embed_spv.py mesh_skinned_vert.spv native/renderer/shaders/mesh_skinned_vert_spv.inc
#version 460

// P5-02: hard cap on skinned joints per draw. Uniform buffers guarantee
// >= 64KiB (maxUniformBufferRange), so 64 x 64B = 4KiB fits everywhere.
const uint SKIN_MAX_JOINTS = 64u;

// Joint palette: column-major model-space joint matrices. Only the first
// SKIN_JOINT_COUNT entries are read; the rest is padding so the UBO stays
// SKIN_MAX_JOINTS-sized regardless of skin size.
layout(set = 0, binding = 0) uniform Joints {
  mat4 joints[64];
} u_joints;

layout(constant_id = 0) const uint SKIN_JOINT_COUNT = 0u;

layout(push_constant) uniform Push {
  mat4 mvp;
  mat4 model;
  vec4 light_dir;
  vec4 base_color;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
// Skin attributes (binding 1). Only the skinned pipeline binds buffer 1
// with 5 attributes; the legacy pipeline uses mesh.vert instead.
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_base;

void main() {
  vec4 skinned_pos = vec4(in_pos, 1.0);
  vec3 skinned_nrm = in_normal;
  if (SKIN_JOINT_COUNT > 0u) {
    // P5-02 4-weight linear blend skinning: weighted sum of joint
    // transforms, normalized by the live weight sum. Weights that index
    // out of the baked palette are skipped (corrupt-vertex fail-safe that
    // keeps one bad vertex from collapsing the whole mesh).
    vec4 acc_p = vec4(0.0);
    vec3 acc_n = vec3(0.0);
    float w_sum = 0.0;
    for (uint k = 0u; k < 4u; ++k) {
      uint j = in_joints[k];
      float w = in_weights[k];
      if (w > 0.0 && j < SKIN_JOINT_COUNT && j < SKIN_MAX_JOINTS) {
        acc_p += w * (u_joints.joints[j] * vec4(in_pos, 1.0));
        acc_n += w * (mat3(u_joints.joints[j]) * in_normal);
        w_sum += w;
      }
    }
    if (w_sum > 0.0) {
      skinned_pos = acc_p / w_sum;
      skinned_nrm = acc_n / w_sum;
    } else {
      // Unskinned fallback: no live weight -> bind pose.
      skinned_pos = vec4(in_pos, 1.0);
      skinned_nrm = in_normal;
    }
  }
  v_normal = normalize(mat3(pc.model) * skinned_nrm);
  v_base = pc.base_color.rgb;
  // mvp already folds the model matrix (MeshPush.mvp = pv * model), so the
  // skinned model-space position feeds it directly — no second model apply.
  gl_Position = pc.mvp * skinned_pos;
}
