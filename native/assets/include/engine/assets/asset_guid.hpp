// asset_guid.hpp — M5-01: stable 128-bit asset GUIDs + human-readable .meta sidecars.
//
// A GUID uniquely names one source asset (Assets/foo.glb) for its lifetime.
// The GUID is minted once (first import) and stored in the adjacent
// `<asset>.meta` sidecar; re-imports, moves, and background artifact jobs
// keep pointing at the same GUID, so scenes reference GUIDs, never paths.
//
// .meta format (hand-editable, git-mergeable):
//   guid: 8f3a2c1d-4b5e-4f6a-9c7d-1e2f3a4b5c6d
//   type: model|texture
//   importerSettings:
//     generateMipmaps: true
//     textureFormat: bc7|astc|srgba8
//     meshOptimize: true
//
// Text format rules: first non-blank, non-`#` line MUST be `guid: <uuid>`.
// Unknown keys are ignored (forward-compatible). Malformed .meta files
// fail closed (parse returns false) — the importer then mints a fresh GUID
// in memory and rewrites the sidecar on save.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/assets/engine_assets_export.hpp"

// MSVC C4251 (std:: members in an exported class) is benign here: all
// std:: members are private and both sides build with the same toolchain.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::assets {

// 128-bit stable asset identifier (RFC-4122-style display, raw bytes compare).
struct ENGINE_ASSETS_API AssetGuid {
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] auto operator==(const AssetGuid& o) const noexcept -> bool {
    return bytes == o.bytes;
  }
  [[nodiscard]] auto operator!=(const AssetGuid& o) const noexcept -> bool {
    return bytes != o.bytes;
  }
  [[nodiscard]] auto is_nil() const noexcept -> bool;
};

// Mint a fresh v4-style random GUID (os entropy: /dev/urandom, BCrypt,
// getentropy — never rand()).
[[nodiscard]] auto MintGuid() noexcept -> AssetGuid;

// Canonical lowercase `8-4-4-4-12` hex. Returns false on bad input.
[[nodiscard]] auto GuidToString(const AssetGuid& g) -> std::string;
[[nodiscard]] auto GuidFromString(std::string_view text, AssetGuid& out) noexcept
    -> bool;

// Asset kind recorded in the .meta sidecar.
enum class AssetKind : std::uint8_t {
  kUnknown = 0,
  kModel = 1,    // .glb / .gltf
  kTexture = 2,  // .png / .jpg / .jpeg
};

// Small subset of importer knobs the background jobs honor. Everything has
// a sane default so a hand-written `guid:`-only .meta still imports.
struct ENGINE_ASSETS_API ImporterSettings {
  bool generate_mipmaps = true;
  // "bc7" (desktop), "astc" (mobile), or "srgba8" (fast/uncompressed).
  std::string texture_format = "bc7";
  bool mesh_optimize = true;
  // Simplifier target, fraction of original triangles (0,1].
  float lod_ratio = 0.5F;
};

// Whole .meta sidecar: GUID + kind + settings.
struct ENGINE_ASSETS_API AssetMeta {
  AssetGuid guid{};
  AssetKind kind = AssetKind::kUnknown;
  ImporterSettings settings{};
};

// Kind inferred from a source path's extension ("" unknown -> kUnknown).
[[nodiscard]] auto KindFromExtension(std::string_view path) noexcept
    -> AssetKind;

// Serialize to the canonical text form (ends with '\n').
[[nodiscard]] auto MetaToString(const AssetMeta& meta) -> std::string;

// Parse canonical text (see file header). Returns false on malformed input;
// `out` is untouched on failure.
[[nodiscard]] auto MetaFromString(std::string_view text, AssetMeta& out) -> bool;

// Filesystem helpers (nothrow; false on any I/O error):
//   LoadMetaForAsset("/a/Assets/foo.glb") reads "/a/Assets/foo.glb.meta"
//     (false when missing OR malformed).
//   SaveMetaForAsset writes atomically (tmp + rename).
[[nodiscard]] auto LoadMetaForAsset(const std::string& asset_path,
                                    AssetMeta& out) noexcept -> bool;
[[nodiscard]] auto SaveMetaForAsset(const std::string& asset_path,
                                    const AssetMeta& meta) noexcept -> bool;

// Ensure a sidecar exists: loads when valid, else mints + saves + returns.
// Always returns true with a valid GUID unless the disk is unwritable.
[[nodiscard]] auto EnsureMetaForAsset(const std::string& asset_path,
                                      AssetMeta& out) noexcept -> bool;

}  // namespace engine::assets

#ifdef _MSC_VER
#pragma warning(pop)
#endif
