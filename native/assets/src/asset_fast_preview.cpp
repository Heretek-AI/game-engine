// asset_fast_preview.cpp — bounded header parsers; see header.
#include "engine/assets/asset_fast_preview.hpp"

#include <cstring>

namespace engine::assets {
namespace {

auto ReadU32Le(const std::uint8_t* p) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

auto ReadU16Be(const std::uint8_t* p) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

// Count `"key"` occurrences in the first `len` bytes of JSON that sit at
// brace depth >= 1 and OUTSIDE string literals. Bounded, no allocation:
// the JSON chunk of a .glb header is small (KBs), but a hostile .gltf may
// be megabytes — callers cap `len` (8KB window is plenty for top-level
// "meshes"/"nodes" keys, which glTF writers emit first).
auto CountJsonKey(const char* json, std::size_t len, const char* key) noexcept
    -> std::uint32_t {
  std::size_t key_len = std::strlen(key);
  std::uint32_t n = 0;
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (std::size_t i = 0; i < len; ++i) {
    char c = json[i];
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      // Possible key: must be followed by optional ws + ':' and sit at
      // depth >= 1. Compare inline, no substr.
      bool match = (i + 1 + key_len + 1 <= len);
      if (match) {
        for (std::size_t k = 0; k < key_len; ++k) {
          if (json[i + 1 + k] != key[k]) {
            match = false;
            break;
          }
        }
      }
      if (match && i + 1 + key_len < len && json[i + 1 + key_len] == '"' &&
          depth >= 1) {
        std::size_t j = i + 1 + key_len + 1;
        while (j < len && (json[j] == ' ' || json[j] == '\t' ||
                           json[j] == '\r' || json[j] == '\n')) {
          ++j;
        }
        if (j < len && json[j] == ':' && depth >= 1) {
          ++n;
        }
      }
      in_str = true;
      esc = false;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    }
  }
  return n;
}

// Count objects in a `"key": [ {...}, {...} ]` array: find the key, then
// count top-level `{` until the matching `]` (strings skipped). Returns 0
// when the key/array is absent. Bounded by `len`.
auto CountArrayObjects(const char* json, std::size_t len,
                       const char* key) noexcept -> std::uint32_t {
  std::size_t key_len = std::strlen(key);
  for (std::size_t i = 0; i + key_len + 4 < len; ++i) {
    if (json[i] != '"') {
      continue;
    }
    std::size_t k = 0;
    while (k < key_len && i + 1 + k < len && json[i + 1 + k] == key[k]) {
      ++k;
    }
    if (k != key_len || i + 1 + key_len >= len ||
        json[i + 1 + key_len] != '"') {
      continue;
    }
    std::size_t j = i + 1 + key_len + 1;
    while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' ||
                       json[j] == '\n')) {
      ++j;
    }
    if (j >= len || json[j] != ':') {
      continue;
    }
    ++j;
    while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' ||
                       json[j] == '\n')) {
      ++j;
    }
    if (j >= len || json[j] != '[') {
      continue;
    }
    ++j;
    // Inside the array: count objects at nesting depth 0.
    std::uint32_t n = 0;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (; j < len; ++j) {
      char c = json[j];
      if (in_str) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          in_str = false;
        }
        continue;
      }
      if (c == '"') {
        in_str = true;
      } else if (c == '{' || c == '[') {
        if (c == '{' && depth == 0) {
          ++n;
        }
        ++depth;
      } else if (c == '}' || c == ']') {
        --depth;
        if (depth < 0) {
          return n;  // ']' closed the array
        }
      }
    }
    return n;
  }
  return 0;
}

}  // namespace

auto ParseModelPreview(const std::uint8_t* data, std::size_t size,
                       ModelPreview& out) noexcept -> bool {
  if (data == nullptr || size < 12) {
    return false;
  }
  ModelPreview p{};
  // GLB magic?
  if (size >= 12 && data[0] == 'g' && data[1] == 'l' && data[2] == 'T' &&
      data[3] == 'F') {
    std::uint32_t ver = ReadU32Le(data + 4);
    std::uint32_t len = ReadU32Le(data + 8);
    if (ver != 2 || len > size || len < 20) {
      return false;
    }
    p.is_binary_glb = true;
    // First chunk MUST be JSON (type 0x4E4F534A).
    if (size < 20) {
      return false;
    }
    std::uint32_t chunk_len = ReadU32Le(data + 12);
    std::uint32_t chunk_type = ReadU32Le(data + 16);
    // 32-bit-safe: `len >= 20` proven above, so `len - 20` cannot underflow.
    // (Naive `20 + chunk_len` wraps for hostile chunk_len ~0xFFFFFFF0 and
    // would read past the map — CodeRabbit #4419770211, verified real.)
    if (chunk_type != 0x4E4F534Au || chunk_len > len - 20U) {
      return false;
    }
    const char* json = reinterpret_cast<const char*>(data + 20);
    std::size_t json_len = chunk_len;
    // Cap the scan: counts live in the first KBs; 64KB is generous.
    if (json_len > 65536) {
      json_len = 65536;
    }
    p.mesh_count = CountArrayObjects(json, json_len, "meshes");
    p.primitive_count = CountJsonKey(json, json_len, "primitives");
    p.node_count = CountArrayObjects(json, json_len, "nodes");
    p.accessor_count = CountArrayObjects(json, json_len, "accessors");
    // Second chunk (BIN) when present (size_t arith: no 32-bit wrap).
    std::size_t off = std::size_t{20} + chunk_len;
    if (off + 8 <= len) {
      std::uint32_t bin_len = ReadU32Le(data + off);
      std::uint32_t bin_type = ReadU32Le(data + off + 4);
      if (bin_type == 0x004E4942u) {
        p.bin_chunk_bytes = bin_len;
      }
    }
    out = p;
    return p.mesh_count > 0 || p.node_count > 0;
  }
  // Raw JSON .gltf?
  if (data[0] == '{') {
    std::size_t window = size < 65536 ? size : 65536;
    const char* json = reinterpret_cast<const char*>(data);
    p.is_binary_glb = false;
    p.mesh_count = CountArrayObjects(json, window, "meshes");
    p.primitive_count = CountJsonKey(json, window, "primitives");
    p.node_count = CountArrayObjects(json, window, "nodes");
    p.accessor_count = CountArrayObjects(json, window, "accessors");
    out = p;
    return p.primitive_count > 0 || p.node_count > 0;
  }
  return false;
}

auto ParseImagePreview(const std::uint8_t* data, std::size_t size,
                       ImagePreview& out) noexcept -> bool {
  if (data == nullptr || size < 16) {
    return false;
  }
  ImagePreview p{};
  // PNG: 8-byte magic + IHDR at offset 16 (length+type+data).
  static constexpr std::uint8_t kPngMagic[8] = {137, 80, 78, 71,
                                                13,  10, 26, 10};
  if (size >= 33 && std::memcmp(data, kPngMagic, 8) == 0) {
    std::uint32_t ihdr_len = (static_cast<std::uint32_t>(data[8]) << 24) |
                             (static_cast<std::uint32_t>(data[9]) << 16) |
                             (static_cast<std::uint32_t>(data[10]) << 8) |
                             data[11];
    if (ihdr_len < 13 || data[12] != 'I' || data[13] != 'H' ||
        data[14] != 'D' || data[15] != 'R') {
      return false;
    }
    p.width = (static_cast<std::uint32_t>(data[16]) << 24) |
              (static_cast<std::uint32_t>(data[17]) << 16) |
              (static_cast<std::uint32_t>(data[18]) << 8) | data[19];
    p.height = (static_cast<std::uint32_t>(data[20]) << 24) |
               (static_cast<std::uint32_t>(data[21]) << 16) |
               (static_cast<std::uint32_t>(data[22]) << 8) | data[23];
    p.bit_depth = data[24];
    std::uint8_t color_type = data[25];
    switch (color_type) {
      case 0:
        p.channels = 1;
        break;  // grayscale
      case 2:
        p.channels = 3;
        break;  // RGB
      case 3:
        p.channels = 1;
        break;  // palette
      case 4:
        p.channels = 2;
        p.has_alpha = true;
        break;  // gray+alpha
      case 6:
        p.channels = 4;
        p.has_alpha = true;
        break;  // RGBA
      default:
        return false;
    }
    if (p.width == 0 || p.height == 0) {
      return false;
    }
    out = p;
    return true;
  }
  // JPEG: SOI + segment scan for SOF0/SOF1/SOF2.
  if (data[0] == 0xFF && data[1] == 0xD8) {
    std::size_t off = 2;
    while (off + 9 < size) {
      if (data[off] != 0xFF) {
        return false;
      }
      std::uint8_t marker = data[off + 1];
      if (marker == 0xD8 || marker == 0xD9) {  // SOI / EOI
        off += 2;
        continue;
      }
      if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        off += 2;  // standalone markers
        continue;
      }
      if (off + 4 > size) {
        return false;
      }
      std::uint16_t seg_len = ReadU16Be(data + off + 2);
      if (seg_len < 2 || off + seg_len > size) {
        return false;
      }
      if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {  // SOF
        if (seg_len < 8) {
          return false;
        }
        p.bit_depth = data[off + 4];
        p.height = ReadU16Be(data + off + 5);
        p.width = ReadU16Be(data + off + 7);
        p.channels = data[off + 9];
        p.has_alpha = false;
        if (p.width == 0 || p.height == 0 || p.channels == 0 ||
            p.channels > 4) {
          return false;
        }
        out = p;
        return true;
      }
      off += static_cast<std::size_t>(seg_len) + 2;
      // Bound the scan: SOF lives in the first 64KB of any sane file.
      if (off > 65536 && off < size) {
        // Keep scanning — some files put big APPn/COM first — but cap
        // total work at 1MB to stay in budget.
        if (off > 1048576) {
          return false;
        }
      }
    }
    return false;
  }
  return false;
}

}  // namespace engine::assets
