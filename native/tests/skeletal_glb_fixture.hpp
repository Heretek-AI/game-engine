// skeletal_glb_fixture.hpp — P5-01 test fixture: two-bone skinned `.glb`.
//
// Generated (not checked in as a binary): 1 triangle mesh with JOINTS_0 /
// WEIGHTS_0, 1 skin with 2 joints (joint1 child of joint0), identity inverse
// bind matrices, and 1 animation clip ("wave", 2 channels): joint1 rotation
// (LINEAR quats: identity -> 90deg-Y -> identity at t=0/0.5/1) and joint1
// translation (STEP: +Y 0 -> 1 at t=0/1).
//
// Layout (all little-endian):
//   nodes:        0 = mesh node (mesh=0, skin=0), 1 = joint0, 2 = joint1
//   accessors:    0 POSITION vec3f x3, 1 indices u16 x3,
//                 2 JOINTS_0 u8 vec4 x3, 3 WEIGHTS_0 f32 vec4 x3,
//                 4 IBM mat4f x2, 5 time f32 x3, 6 rot f32 vec4 x3,
//                 7 time f32 x2, 8 trans f32 vec3 x2
//   bufferViews:  one per accessor, byteOffset/byteLength into buffer 0.
// Chunk Length fields count payload bytes only (no header) — see spec
// §4.13.3 GLB gotcha: writing data+8 makes cgltf_parse_file return
// cgltf_result_data_too_short.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace engine::tests {

// Two-bone skinned GLB: joint_count=2, joint1 child of joint0, one "wave"
// clip with rotation + translation keyframes. Returns false on I/O error.
inline auto WriteSkeletalGlb(const std::string& path) -> bool {
  const char* json =
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":1}]}],"
      "\"nodes\":[{\"mesh\":0,\"skin\":0,\"name\":\"meshNode\"},"
      "{\"name\":\"joint0\",\"children\":[2]},"
      "{\"name\":\"joint1\",\"translation\":[0,1,0]}],"
      "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0,"
      "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":4,"
      "\"name\":\"body\"}],"
      "\"animations\":[{\"name\":\"wave\",\"samplers\":["
      "{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"},"
      "{\"input\":7,\"output\":8,\"interpolation\":\"STEP\"}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":"
      "\"rotation\"}},"
      "{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"translation\"}}]}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":"
      "\"SCALAR\"},"
      "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":"
      "\"SCALAR\"},"
      "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
      "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":"
      "\"SCALAR\"},"
      "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\" "
      "/*placeholder, fixed below*/}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
      "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":104,\"byteLength\":128},"
      "{\"buffer\":0,\"byteOffset\":232,\"byteLength\":12},"
      "{\"buffer\":0,\"byteOffset\":244,\"byteLength\":48},"
      "{\"buffer\":0,\"byteOffset\":292,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":300,\"byteLength\":24}],"
      "\"buffers\":[{\"byteLength\":324}]}";
  // Fix the accessor-8 placeholder: VEC3, not VEC4 (kept inline above only
  // to hold the column width readable).
  std::string js(json);
  const char* bad = "\"type\":\"VEC4\" /*placeholder, fixed below*/";
  std::size_t at = js.find(bad);
  if (at == std::string::npos) {
    return false;
  }
  js.replace(at, std::strlen(bad), "\"type\":\"VEC3\"");
  std::size_t json_len = js.size();
  std::size_t json_pad = (4 - (json_len % 4)) % 4;

  // BIN payload (324 bytes): positions, indices, joints, weights, IBM x2,
  // rot times, rot quats, trans times, trans vecs.
  float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::uint16_t idx[3] = {0, 1, 2};
  // u8 pad to 44 (36 + 6 + 2 pad for u8 joints at 44... joints start at
  // offset 44: joints u8x12 -> 44..56, weights f32x12 -> 56..104.
  std::uint8_t joints[12] = {0, 0, 0, 0,  // v0 -> joint0
                             1, 0, 0, 0,  // v1 -> joint1
                             0, 1, 0, 0};  // v2 -> both (0.5/0.5 below)
  float weights[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0.5F, 0.5F, 0, 0};
  float ibm[32];
  for (int m = 0; m < 2; ++m) {
    for (int i = 0; i < 16; ++i) {
      ibm[m * 16 + i] = (i % 5 == 0) ? 1.0F : 0.0F;
    }
  }
  float rot_times[3] = {0.0F, 0.5F, 1.0F};
  const float s = 0.70710678F;  // sin(45deg): 90deg-about-Y mid key
  float rot[12] = {0, 0, 0, 1, 0, s, 0, s, 0, 0, 0, 1};
  float trans_times[2] = {0.0F, 1.0F};
  float trans[6] = {0, 0, 0, 0, 1, 0};
  const std::size_t bin_len = 324;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  auto w32 = [&](std::uint32_t v) {
    std::uint8_t b[4] = {static_cast<std::uint8_t>(v & 0xFF),
                         static_cast<std::uint8_t>((v >> 8) & 0xFF),
                         static_cast<std::uint8_t>((v >> 16) & 0xFF),
                         static_cast<std::uint8_t>((v >> 24) & 0xFF)};
    f.write(reinterpret_cast<const char*>(b), 4);
  };
  f.write("glTF", 4);
  w32(2);
  w32(static_cast<std::uint32_t>(12 + 8 + json_len + json_pad + 8 + bin_len));
  w32(static_cast<std::uint32_t>(json_len + json_pad));
  w32(0x4E4F534Au);  // JSON
  f.write(js.data(), static_cast<std::streamsize>(json_len));
  for (std::size_t i = 0; i < json_pad; ++i) {
    f.put(' ');
  }
  w32(static_cast<std::uint32_t>(bin_len));
  w32(0x004E4942u);  // BIN
  std::size_t written = 0;
  auto put = [&](const void* p, std::size_t n) {
    f.write(reinterpret_cast<const char*>(p),
            static_cast<std::streamsize>(n));
    written += n;
  };
  put(pos, 36);          // 0..36
  put(idx, 6);           // 36..42
  std::uint8_t pad2[2] = {0, 0};
  put(pad2, 2);          // 42..44
  put(joints, 12);       // 44..56
  put(weights, 48);      // 56..104
  put(ibm, 128);         // 104..232
  put(rot_times, 12);    // 232..244
  put(rot, 48);          // 244..292
  put(trans_times, 8);   // 292..300
  put(trans, 24);        // 300..324
  if (written != bin_len) {
    return false;
  }
  f.flush();
  return static_cast<bool>(f);
}

}  // namespace engine::tests
