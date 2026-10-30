// asset_fast_preview.hpp — M5-01: sub-frame header parse over mapped bytes.
//
// The <16ms gate has no room for a full decode. These parsers read ONLY
// the container headers, directly from the MappedFile, with zero copies:
//
//   .glb  -> JSON chunk header -> mesh/primitive/node counts
//   .gltf -> first 4KB scan -> mesh/primitive counts (quoted strings ignored)
//   .png  -> IHDR (width/height/bit-depth/color-type, CRC skipped)
//   .jpg  -> SOF0/SOF2 scan (width/height/components)
//
// Every entry point is noexcept, allocation-free on success (out-params
// only), and bounded: a truncated file fails closed rather than reading
// past the mapped range. Full decode (vertices, texels) happens later on
// the background import worker.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/assets/engine_assets_export.hpp"

namespace engine::assets {

// Fast preview of a model asset: counts for the viewport placeholder
// (bounding-box + "N meshes" label) without touching buffer data.
struct ENGINE_ASSETS_API ModelPreview {
  std::uint32_t mesh_count = 0;
  std::uint32_t primitive_count = 0;
  std::uint32_t node_count = 0;
  std::uint32_t accessor_count = 0;
  // Byte length of the binary buffer chunk (0 = external .bin / missing).
  std::uint64_t bin_chunk_bytes = 0;
  bool is_binary_glb = false;
};

// Fast preview of an image asset: dimensions for an uncompressed RGBA8
// staging upload (mip 0 shown immediately, lower mips generated async).
struct ENGINE_ASSETS_API ImagePreview {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t channels = 0;  // as-stored (PNG color type / JPEG comps)
  std::uint8_t bit_depth = 8;
  bool has_alpha = false;
};

// Parse a GLB container (magic `glTF`, JSON chunk) or a .gltf JSON text.
// Returns false when the magic/version/chunk is bad or counts are missing.
[[nodiscard]] auto ParseModelPreview(const std::uint8_t* data,
                                     std::size_t size,
                                     ModelPreview& out) noexcept -> bool;

// Parse PNG IHDR or JPEG SOF. Returns false on bad magic/truncation.
[[nodiscard]] auto ParseImagePreview(const std::uint8_t* data,
                                     std::size_t size,
                                     ImagePreview& out) noexcept -> bool;

}  // namespace engine::assets
