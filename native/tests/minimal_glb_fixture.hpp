// minimal_glb_fixture.hpp — shared M5-01/P3-08 test fixture.
//
// One canonical generator for the minimal valid GLB (1 mesh / 1 triangle
// primitive / 2 nodes + BIN chunk with POSITION + indices, hand-built
// little-endian). test_asset_pipeline.cpp and test_project_drop.cpp both
// use it: a single definition keeps Sonar's new-code duplication gate
// green and guarantees both suites stage byte-identical fixtures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace engine::tests {

// Minimal valid GLB: 1 mesh (1 triangle primitive), 2 nodes, BIN chunk
// with POSITION (3x vec3f) + indices. Hand-built little-endian.
inline auto WriteMinimalGlb(const std::string& path) -> bool {
  const char* json =
      "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primitives\":[{"
      "\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
      "\"nodes\":[{\"mesh\":0},{}],\"scenes\":[{\"nodes\":[0,1]}],"
      "\"scene\":0,\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
      "\"count\":3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":"
      "5123,\"count\":3,\"type\":\"SCALAR\"}],\"bufferViews\":[{"
      "\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,"
      "\"byteOffset\":36,\"byteLength\":6}],\"buffers\":[{\"byteLength\":44}]}";
  std::size_t json_len = std::strlen(json);
  std::size_t json_pad = (4 - (json_len % 4)) % 4;
  // BIN: 3 positions (0,0,0)(1,0,0)(0,1,0) + 3 u16 indices (0,1,2) + pad.
  float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::uint16_t idx[3] = {0, 1, 2};
  std::size_t bin_len = 36 + 6;
  std::size_t bin_pad = (4 - (bin_len % 4)) % 4;
  std::size_t total = 12 + 8 + json_len + json_pad + 8 + bin_len + bin_pad;

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
  w32(static_cast<std::uint32_t>(total));
  w32(static_cast<std::uint32_t>(json_len + json_pad));
  w32(0x4E4F534Au);  // JSON
  f.write(json, static_cast<std::streamsize>(json_len));
  for (std::size_t i = 0; i < json_pad; ++i) {
    f.put(' ');
  }
  w32(static_cast<std::uint32_t>(bin_len + bin_pad));
  w32(0x004E4942u);  // BIN
  f.write(reinterpret_cast<const char*>(pos), 36);
  f.write(reinterpret_cast<const char*>(idx), 6);
  for (std::size_t i = 0; i < bin_pad; ++i) {
    f.put('\0');
  }
  f.flush();
  return static_cast<bool>(f);
}

}  // namespace engine::tests
