// asset_guid.cpp — GUID mint/parse + .meta load/save; see header.
#include "engine/assets/asset_guid.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace engine::assets {
namespace {

auto HexVal(char c) noexcept -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

auto Trim(std::string_view s) noexcept -> std::string_view {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

auto Lower(std::string s) -> std::string {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

auto EqualsCi(std::string_view a, std::string_view b) noexcept -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
// NOLINTNEXTLINE: Win32 entropy, single call, no frame path.
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID b, ULONG len);
auto FillRandom(std::uint8_t* dst, std::size_t n) noexcept -> bool {
  // SystemFunction036 == RtlGenRandom, the OS CSPRNG (advapi32.lib, always
  // linked). Declared locally: ntsecapi.h is unreliable under /permissive-,
  // and BCrypt would need an extra bcrypt.lib link line.
  return SystemFunction036(dst, static_cast<ULONG>(n)) != 0;
}
#elif defined(__linux__) || defined(__APPLE__)
// NOLINTNEXTLINE: os entropy read, no frame path.
auto FillRandom(std::uint8_t* dst, std::size_t n) noexcept -> bool {
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (f == nullptr) {
    return false;
  }
  std::size_t got = std::fread(dst, 1, n, f);
  std::fclose(f);
  return got == n;
}
#else
auto FillRandom(std::uint8_t* dst, std::size_t n) noexcept -> bool {
  // Fallback: mix time + address (only for exotic ports; Linux/macOS/Win
  // cover CI). Still better than rand().
  std::uint64_t t =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(dst));
  for (std::size_t i = 0; i < n; ++i) {
    t = t * 6364136223846793005ULL + 1442695040888963407ULL;
    dst[i] = static_cast<std::uint8_t>((t >> 33) & 0xFFU);
  }
  return true;
}
#endif

}  // namespace

auto MintGuid() noexcept -> AssetGuid {
  AssetGuid g{};
  if (!FillRandom(g.bytes.data(), g.bytes.size())) {
    g.bytes.fill(0);
  }
  // RFC-4122 v4: version nibble 0100, variant bits 10xx.
  g.bytes[6] = static_cast<std::uint8_t>((g.bytes[6] & 0x0FU) | 0x40U);
  g.bytes[8] = static_cast<std::uint8_t>((g.bytes[8] & 0x3FU) | 0x80U);
  return g;
}

auto GuidToString(const AssetGuid& g) -> std::string {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      s.push_back('-');
    }
    s.push_back(kHex[(g.bytes[i] >> 4) & 0xF]);
    s.push_back(kHex[g.bytes[i] & 0xF]);
  }
  return s;
}

auto GuidFromString(std::string_view text, AssetGuid& out) noexcept -> bool {
  // Expect 8-4-4-4-12 (36 chars). Uppercase accepted.
  if (text.size() != 36) {
    return false;
  }
  if (text[8] != '-' || text[13] != '-' || text[18] != '-' ||
      text[23] != '-') {
    return false;
  }
  AssetGuid g{};
  std::size_t bi = 0;
  for (std::size_t i = 0; i < 36;) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1 >= 36 || bi >= 16) {
      return false;
    }
    int hi = HexVal(text[i]);
    int lo = HexVal(text[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    g.bytes[bi++] =
        static_cast<std::uint8_t>((static_cast<unsigned>(hi) << 4) |
                                  static_cast<unsigned>(lo));
    i += 2;
  }
  if (bi != 16) {
    return false;
  }
  out = g;
  return true;
}

auto KindFromExtension(std::string_view path) noexcept -> AssetKind {
  std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) {
    return AssetKind::kUnknown;
  }
  std::string_view ext = path.substr(dot + 1);
  if (EqualsCi(ext, "glb") || EqualsCi(ext, "gltf")) {
    return AssetKind::kModel;
  }
  if (EqualsCi(ext, "png") || EqualsCi(ext, "jpg") ||
      EqualsCi(ext, "jpeg")) {
    return AssetKind::kTexture;
  }
  return AssetKind::kUnknown;
}

auto MetaToString(const AssetMeta& meta) -> std::string {
  std::ostringstream os;
  os << "guid: " << GuidToString(meta.guid) << "\n";
  os << "type: ";
  switch (meta.kind) {
    case AssetKind::kModel:
      os << "model";
      break;
    case AssetKind::kTexture:
      os << "texture";
      break;
    default:
      os << "unknown";
      break;
  }
  os << "\nimporterSettings:\n";
  os << "  generateMipmaps: " << (meta.settings.generate_mipmaps ? "true" : "false")
     << "\n";
  os << "  textureFormat: " << meta.settings.texture_format << "\n";
  os << "  meshOptimize: " << (meta.settings.mesh_optimize ? "true" : "false")
     << "\n";
  os << "  lodRatio: " << meta.settings.lod_ratio << "\n";
  return os.str();
}

auto MetaFromString(std::string_view text, AssetMeta& out) -> bool {
  AssetMeta m{};
  bool have_guid = false;
  bool in_settings = false;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t nl = text.find('\n', pos);
    std::string_view line =
        Trim(text.substr(pos, nl == std::string_view::npos ? nl : nl - pos));
    if (nl == std::string_view::npos) {
      pos = text.size() + 1;
    } else {
      pos = nl + 1;
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::size_t colon = line.find(':');
    std::string_view key = colon == std::string_view::npos
                               ? line
                               : Trim(line.substr(0, colon));
    std::string_view val =
        colon == std::string_view::npos ? "" : Trim(line.substr(colon + 1));
    // Nested settings keys arrive indented; Trim already de-indented them.
    if (EqualsCi(key, "guid")) {
      if (!GuidFromString(val, m.guid)) {
        return false;
      }
      have_guid = true;
      in_settings = false;
    } else if (EqualsCi(key, "type")) {
      if (EqualsCi(val, "model")) {
        m.kind = AssetKind::kModel;
      } else if (EqualsCi(val, "texture")) {
        m.kind = AssetKind::kTexture;
      } else {
        m.kind = AssetKind::kUnknown;
      }
      in_settings = false;
    } else if (EqualsCi(key, "importerSettings")) {
      in_settings = true;
    } else if (in_settings && EqualsCi(key, "generateMipmaps")) {
      m.settings.generate_mipmaps =
          EqualsCi(val, "true") || val == "1" || EqualsCi(val, "yes");
    } else if (in_settings && EqualsCi(key, "textureFormat")) {
      m.settings.texture_format = Lower(std::string(val));
    } else if (in_settings && EqualsCi(key, "meshOptimize")) {
      m.settings.mesh_optimize =
          EqualsCi(val, "true") || val == "1" || EqualsCi(val, "yes");
    } else if (in_settings && EqualsCi(key, "lodRatio")) {
      // Invalid value keeps its default (0.5) instead of failing the whole
      // file: a hand-edit typo must never orphan a stable GUID (CodeRabbit
      // #4419770220, verified real — old code returned false here and
      // EnsureMetaForAsset minted a replacement GUID on every re-import).
      try {
        float v = std::stof(std::string(val));
        if (v > 0.0F && v <= 1.0F) {
          m.settings.lod_ratio = v;
        }
      } catch (...) {
        // keep default
      }
    } else {
      // Unknown keys ignored (forward-compatible).
    }
  }
  if (!have_guid || m.guid.is_nil()) {
    return false;
  }
  out = m;
  return true;
}

auto LoadMetaForAsset(const std::string& asset_path,
                      AssetMeta& out) noexcept -> bool {
  try {
    std::ifstream in(asset_path + ".meta", std::ios::binary);
    if (!in) {
      return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    AssetMeta m{};
    if (!MetaFromString(ss.str(), m)) {
      return false;
    }
    out = m;
    return true;
  } catch (...) {
    return false;
  }
}

auto SaveMetaForAsset(const std::string& asset_path,
                      const AssetMeta& meta) noexcept -> bool {
  try {
    std::string body = MetaToString(meta);
    std::string tmp = asset_path + ".meta.tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      out << body;
      out.flush();
      if (!out) {
        return false;
      }
    }
    std::string dst = asset_path + ".meta";
#if defined(_WIN32)
    std::remove(dst.c_str());
#endif
    return std::rename(tmp.c_str(), dst.c_str()) == 0;
  } catch (...) {
    return false;
  }
}

auto EnsureMetaForAsset(const std::string& asset_path,
                        AssetMeta& out) noexcept -> bool {
  AssetMeta m{};
  if (LoadMetaForAsset(asset_path, m) && !m.guid.is_nil()) {
    out = m;
    return true;
  }
  m.guid = MintGuid();
  m.kind = KindFromExtension(asset_path);
  if (!SaveMetaForAsset(asset_path, m)) {
    // Disk unwritable: still hand back a valid in-memory GUID so the
    // fast path (viewport preview) never blocks on a sidecar write.
    out = m;
    return !m.guid.is_nil();
  }
  out = m;
  return true;
}

}  // namespace engine::assets
