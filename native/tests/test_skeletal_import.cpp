// test_skeletal_import.cpp — P5-01 acceptance (native CTest).
//
// CPU-side contract (always built, no GPU needed):
//   1. A two-bone .glb is GENERATED into scratch (3 nodes, 1 skin with
//      2 joints + inverse bind matrices, 1 animation "wave" with 2
//      samplers/channels) and parsed through the real cgltf file path
//      (cgltf_parse_file -> cgltf_load_buffers -> cgltf_validate — the
//      exact P5-00 spike sequence) when ENGINE_ASSETS_HAS_CGLTF.
//   2. SkeletalImportSkeletons: 1 skeleton, joint count 2, hierarchy
//      (joint0 child of the scene root, joint1 child of joint0), joint
//      names, inverse bind matrices (identity + translation(5,0,0)).
//   3. SkeletalImportClips: 1 clip "wave", duration 1.0, channel0 drives
//      joint1 rotation (LINEAR, times [0,0.5,1], identity -> 90degY ->
//      identity quats), channel1 drives joint0 translation (STEP).
//   4. SerializeSkeleton/SerializeClip: ENGS/ENGA magics, version, and a
//      field-level parse-back of the blob bytes.
//   5. .pak round-trip: both blobs pack via PakBuilder and read back
//      byte-exact (CRC-verified) via PakReader.
//   6. Guards: null args, OOB skin/anim index, zero-joint skin, and no
//      partial append on failure.
// Without cgltf the import section reports kUnsupported (offline degrade).
//
// Per-vertex JOINTS_0/WEIGHTS_0 attributes ride the fixture (genuine
// skeletal mesh) but stay in the mesh vertex format owned by P5-02 GPU
// skinning; P5-01 covers skins, joints, IBMs and clips into the .pak.
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "engine/assets/asset_guid.hpp"
#include "engine/assets/asset_pak.hpp"
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/skeletal_import.hpp"

#if defined(ENGINE_ASSETS_HAS_CGLTF)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_WIN32)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#include "cgltf.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_WIN32)
#pragma warning(pop)
#endif
#endif

namespace fs = std::filesystem;

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  if (!ok) {
    ++failures;
  }
}

// Unique scratch dir in cwd (ctest runs with cwd == binary dir).
auto ScratchDir() -> std::string {
#if defined(_WIN32)
  const int pid = _getpid();
#else
  const int pid = static_cast<int>(::getpid());
#endif
  char num[32]{};
  const auto res = std::to_chars(num, num + sizeof(num) - 1, pid);
  std::string p("skeletal_test_scratch_");
  p.append(num, static_cast<std::size_t>(res.ptr - num));
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
#if !defined(_WIN32)
  ::chmod(p.c_str(), 0700);
#endif
  return p;
}

void AppendNum(std::string& s, std::uint32_t v) {
  char buf[16]{};
  const auto res = std::to_chars(buf, buf + sizeof(buf), v);
  s.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

void AppendF32(std::vector<std::uint8_t>& b, float v) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(v);
  b.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFFu));
}

void AppendU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void AppendU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
  b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendU32Raw(std::ofstream& f, std::uint32_t v) {
  char b[4]{static_cast<char>(v & 0xFFu),
            static_cast<char>((v >> 8) & 0xFFu),
            static_cast<char>((v >> 16) & 0xFFu),
            static_cast<char>((v >> 24) & 0xFFu)};
  f.write(b, 4);
}

// Two-bone .glb writer. BIN layout (all offsets 4-aligned):
//   pos 36B @0 | idx 6B+2pad @36 | joints0 24B @44 | weights0 48B @68 |
//   ibm 128B @116 | time 12B @244 | rot 48B @256 | trans 36B @304 (=340B).
// Chunk Length fields count payload bytes only (P5-00 §4.13.3 gotcha).
auto WriteTwoBoneGlb(const std::string& path) -> bool {
  constexpr std::uint32_t kPos = 0;
  constexpr std::uint32_t kIdx = 36;
  constexpr std::uint32_t kJoints = 44;
  constexpr std::uint32_t kWeights = 68;
  constexpr std::uint32_t kIbm = 116;
  constexpr std::uint32_t kTime = 244;
  constexpr std::uint32_t kRot = 256;
  constexpr std::uint32_t kTrans = 304;
  constexpr std::uint32_t kBinLen = 340;

  std::vector<std::uint8_t> bin;
  bin.reserve(kBinLen);
  // positions: one triangle.
  for (float v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
    AppendF32(bin, v);
  }
  // indices u16.
  AppendU16(bin, 0);
  AppendU16(bin, 1);
  AppendU16(bin, 2);
  bin.push_back(0);
  bin.push_back(0);
  // JOINTS_0 u16 vec4 x3: vert0->joint0, vert1/2->joint1.
  for (std::uint16_t v : {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}) {
    AppendU16(bin, v);
  }
  // WEIGHTS_0 float vec4 x3.
  for (float v :
       {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}) {
    AppendF32(bin, v);
  }
  // IBMs: joint0 identity, joint1 translation(5,0,0), column-major.
  for (float v : {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}) {
    AppendF32(bin, v);
  }
  for (float v : {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  1.0f, 0.0f, 5.0f, 0.0f, 0.0f, 1.0f}) {
    AppendF32(bin, v);
  }
  // time input [0, 0.5, 1].
  for (float v : {0.0f, 0.5f, 1.0f}) {
    AppendF32(bin, v);
  }
  // rotation output: identity, 90deg-about-Y, identity (x,y,z,w).
  constexpr float kS = 0.70710678f;
  for (float v :
       {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, kS, 0.0f, kS, 0.0f, 0.0f, 0.0f, 1.0f}) {
    AppendF32(bin, v);
  }
  // translation output: (0,0,0) (0,1,0) (0,2,0).
  for (float v : {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f}) {
    AppendF32(bin, v);
  }
  if (bin.size() != kBinLen) {
    return false;
  }

  std::string j("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],");
  j += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,";
  j += "\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":1}]}],";
  j += "\"nodes\":[{\"children\":[1]},{\"name\":\"joint0\",\"children\":[2]},";
  j += "{\"name\":\"joint1\"}],";
  j += "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":4}],";
  j += "\"animations\":[{\"name\":\"wave\",\"samplers\":[";
  j += "{\"input\":5,\"output\":6,\"interpolation\":\"LINEAR\"},";
  j += "{\"input\":5,\"output\":7,\"interpolation\":\"STEP\"}],";
  j += "\"channels\":[{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"rotation\"}},";
  j += "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"translation\"}}]}],";
  j += "\"accessors\":[";
  j += "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},";
  j += "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},";
  j += "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"},";
  j += "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},";
  j += "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},";
  j += "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},";
  j += "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},";
  j += "{\"bufferView\":7,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],";
  j += "\"bufferViews\":[";
  auto view = [&](std::uint32_t off, std::uint32_t len) {
    j += "{\"buffer\":0,\"byteOffset\":";
    AppendNum(j, off);
    j += ",\"byteLength\":";
    AppendNum(j, len);
    j += "},";
  };
  view(kPos, 36);
  view(kIdx, 6);
  view(kJoints, 24);
  view(kWeights, 48);
  view(kIbm, 128);
  view(kTime, 12);
  view(kRot, 48);
  view(kTrans, 36);
  j.pop_back();
  j += "],\"buffers\":[{\"byteLength\":";
  AppendNum(j, kBinLen);
  j += "}]}";

  const std::size_t json_pad = (4 - (j.size() % 4)) % 4;
  const std::uint32_t total =
      static_cast<std::uint32_t>(12 + 8 + j.size() + json_pad + 8 + kBinLen);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  f.write("glTF", 4);
  AppendU32Raw(f, 2);
  AppendU32Raw(f, total);
  AppendU32Raw(f, static_cast<std::uint32_t>(j.size() + json_pad));
  AppendU32Raw(f, 0x4E4F534Au);
  f.write(j.data(), static_cast<std::streamsize>(j.size()));
  for (std::size_t i = 0; i < json_pad; ++i) {
    f.put(' ');
  }
  AppendU32Raw(f, kBinLen);
  AppendU32Raw(f, 0x004E4942u);
  f.write(reinterpret_cast<const char*>(bin.data()),
          static_cast<std::streamsize>(bin.size()));
  f.flush();
  return static_cast<bool>(f);
}

auto ReadU32(const std::vector<std::uint8_t>& b, std::size_t off)
    -> std::uint32_t {
  return static_cast<std::uint32_t>(b[off]) |
         (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

auto ReadF32(const std::vector<std::uint8_t>& b, std::size_t off) -> float {
  const std::uint32_t bits = ReadU32(b, off);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

}  // namespace

auto main() -> int {
  using namespace engine::renderer;
  using namespace engine::assets;
  const std::string dir = ScratchDir();
  const std::string glb = dir + "/two_bone.glb";
  check(WriteTwoBoneGlb(glb), "fixture glb written");

#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  std::vector<SkeletalSkeleton> skels;
  check(!SkeletalImportSkeletons(nullptr, nullptr).ok(),
        "skeletal: no-cgltf reports unsupported");
  check(!SkeletalImportSkeletons(nullptr, &skels).ok(),
        "skeletal: no-cgltf null data unsupported");
  std::printf("  (offline fetch: skeletal import degrades)\\n");
#else
  // --- parse through the real file path (P5-00 §4.13.3 sequence) ---------
  cgltf_options opts{};
  cgltf_data* data = nullptr;
  check(cgltf_parse_file(&opts, glb.c_str(), &data) == cgltf_result_success &&
            data != nullptr,
        "skeletal: parse_file ok");
  if (data == nullptr) {
    std::printf("SKELETAL_IMPORT_FAIL\\n");
    return 1;
  }
  check(cgltf_load_buffers(&opts, data, glb.c_str()) == cgltf_result_success,
        "skeletal: load_buffers ok");
  check(cgltf_validate(data) == cgltf_result_success, "skeletal: validate ok");
  check(data->nodes_count == 3, "skeletal: 3 nodes");
  check(data->skins_count == 1, "skeletal: 1 skin");
  check(data->animations_count == 1, "skeletal: 1 animation");

  // --- skeletons: joint count + hierarchy + IBMs --------------------------
  std::vector<SkeletalSkeleton> skels;
  check(SkeletalImportSkeletons(data, &skels).ok(), "skeletal: import ok");
  check(skels.size() == 1, "skeletal: 1 skeleton");
  bool skel_ok = skels.size() == 1 && skels[0].joints.size() == 2;
  check(skel_ok, "skeletal: joint count 2");
  if (skel_ok) {
    const SkeletalJoint& j0 = skels[0].joints[0];
    const SkeletalJoint& j1 = skels[0].joints[1];
    check(j0.node_index == 1, "skeletal: joint0 node 1");
    check(j0.parent_joint == kNoneJoint, "skeletal: joint0 root (parent none)");
    check(j1.node_index == 2, "skeletal: joint1 node 2");
    check(j1.parent_joint == 0, "skeletal: joint1 parent joint0");
    check(SkeletalNameView(j0.name) == "joint0", "skeletal: joint0 name");
    check(SkeletalNameView(j1.name) == "joint1", "skeletal: joint1 name");
    bool ibm0_id = true;
    for (std::size_t m = 0; m < 16; ++m) {
      const float want = (m == 0 || m == 5 || m == 10 || m == 15) ? 1.0f : 0.0f;
      if (j0.inverse_bind[m] != want) {
        ibm0_id = false;
      }
    }
    check(ibm0_id, "skeletal: joint0 IBM identity");
    check(j1.inverse_bind[0] == 1.0f && j1.inverse_bind[12] == 5.0f &&
              j1.inverse_bind[15] == 1.0f,
          "skeletal: joint1 IBM translation(5,0,0)");
  }

  // --- clips: keyframe values ---------------------------------------------
  std::vector<SkeletalClip> clips;
  check(SkeletalImportClips(data, &clips).ok(), "skeletal: clips ok");
  bool clips_ok = clips.size() == 1 && clips[0].channels.size() == 2;
  check(clips.size() == 1, "skeletal: 1 clip");
  check(clips_ok, "skeletal: 2 channels");
  if (clips_ok) {
    const SkeletalClip& clip = clips[0];
    check(SkeletalNameView(clip.name) == "wave", "skeletal: clip name wave");
    check(clip.duration == 1.0f, "skeletal: duration 1.0");
    const SkeletalChannel& rot = clip.channels[0];
    check(rot.joint == 2, "skeletal: rot channel joint1");
    check(rot.path == SkeletalPath::kRotation, "skeletal: rot path");
    check(rot.interpolation == SkeletalInterpolation::kLinear,
          "skeletal: rot linear");
    check(rot.times.size() == 3 && rot.times[0] == 0.0f &&
              rot.times[1] == 0.5f && rot.times[2] == 1.0f,
          "skeletal: rot times [0,0.5,1]");
    constexpr float kS = 0.70710678f;
    check(rot.values.size() == 12, "skeletal: rot 12 floats");
    check(rot.values.size() == 12 && rot.values[0] == 0.0f &&
              rot.values[1] == 0.0f && rot.values[2] == 0.0f &&
              rot.values[3] == 1.0f,
          "skeletal: rot key0 identity quat");
    check(rot.values.size() == 12 && rot.values[4] == 0.0f &&
              rot.values[5] == kS && rot.values[6] == 0.0f &&
              rot.values[7] == kS,
          "skeletal: rot key1 90deg-Y quat");
    check(rot.values.size() == 12 && rot.values[8] == 0.0f &&
              rot.values[11] == 1.0f,
          "skeletal: rot key2 identity quat");
    const SkeletalChannel& tra = clip.channels[1];
    check(tra.joint == 1, "skeletal: trans channel joint0");
    check(tra.path == SkeletalPath::kTranslation, "skeletal: trans path");
    check(tra.interpolation == SkeletalInterpolation::kStep,
          "skeletal: trans step");
    check(tra.values.size() == 9 && tra.values[0] == 0.0f &&
              tra.values[4] == 1.0f && tra.values[7] == 2.0f,
          "skeletal: trans keys (0,0,0)(0,1,0)(0,2,0)");
  }

  // --- serialize: ENGS/ENGA field parse-back -------------------------------
  std::vector<std::uint8_t> skel_blob;
  std::vector<std::uint8_t> clip_blob;
  check(SerializeSkeleton(&skels[0], &skel_blob).ok(), "skeletal: serialize ok");
  check(SerializeClip(&clips[0], &clip_blob).ok(), "skeletal: clip ser ok");
  check(ReadU32(skel_blob, 0) == kSkeletalMagic, "skeletal: ENGS magic");
  check(ReadU32(skel_blob, 4) == kSkeletalVersion, "skeletal: ENGS version");
  check(ReadU32(skel_blob, 8) == 2, "skeletal: ENGS joint count");
  check(ReadU32(skel_blob, 12) == 1 && ReadU32(skel_blob, 16) == kNoneJoint,
        "skeletal: ENGS joint0 node/parent");
  {
    std::string n0(reinterpret_cast<const char*>(&skel_blob[20]), 6);
    check(n0 == "joint0", "skeletal: ENGS joint0 name bytes");
  }
  check(ReadF32(skel_blob, 20 + 64) == 1.0f, "skeletal: ENGS IBM00 1.0");
  constexpr std::size_t kJointStride = 8 + 64 + 64;
  check(ReadU32(skel_blob, 12 + kJointStride) == 2 &&
            ReadU32(skel_blob, 16 + kJointStride) == 0,
        "skeletal: ENGS joint1 node/parent");
  check(ReadU32(clip_blob, 0) == kSkeletalClipMagic, "skeletal: ENGA magic");
  check(ReadU32(clip_blob, 4) == kSkeletalVersion, "skeletal: ENGA version");
  check(ReadF32(clip_blob, 8) == 1.0f, "skeletal: ENGA duration");
  check(ReadU32(clip_blob, 12) == 2, "skeletal: ENGA channel count");
  check(ReadU32(clip_blob, 16) == 2 && skel_blob[20] != 0,
        "skeletal: ENGA ch0 joint");
  check(clip_blob[20] == 1 && clip_blob[21] == 0,
        "skeletal: ENGA ch0 rotation/linear");

  // --- .pak round-trip ------------------------------------------------------
  {
    PakBuilder b;
    const AssetGuid sg = MintGuid();
    const AssetGuid cg = MintGuid();
    bool add_ok =
        b.add(sg, AssetKind::kModel, "two_bone.skel", skel_blob.data(),
              skel_blob.size(), true) &&
        b.add(cg, AssetKind::kModel, "wave.clip", clip_blob.data(),
              clip_blob.size(), true);
    check(add_ok, "skeletal: pak add skeleton+clip");
    std::string err;
    const std::string pak = dir + "/skeletal.pak";
    check(b.write(pak, err), "skeletal: pak write");
    PakReader r;
    check(r.open(pak, err), "skeletal: pak open");
    if (r.is_open()) {
      check(r.entry_count() == 2, "skeletal: pak 2 entries");
      std::vector<std::uint8_t> scratch;
      std::span<const std::uint8_t> out;
      bool rd_ok = r.read(sg, scratch, out);
      check(rd_ok && out.size() == skel_blob.size(),
            "skeletal: pak skeleton size");
      bool match = rd_ok && out.size() == skel_blob.size();
      for (std::size_t i = 0; match && i < out.size(); ++i) {
        if (out[i] != skel_blob[i]) {
          match = false;
        }
      }
      check(match, "skeletal: pak skeleton byte-exact");
      rd_ok = r.read(cg, scratch, out);
      check(rd_ok && out.size() == clip_blob.size(), "skeletal: pak clip size");
      match = rd_ok && out.size() == clip_blob.size();
      for (std::size_t i = 0; match && i < out.size(); ++i) {
        if (out[i] != clip_blob[i]) {
          match = false;
        }
      }
      check(match, "skeletal: pak clip byte-exact");
    }
  }

  // --- guards ----------------------------------------------------------------
  check(!SkeletalImportSkeletons(nullptr, &skels).ok(),
        "skeletal: null data rejected");
  check(!SkeletalImportSkeletons(data, nullptr).ok(),
        "skeletal: null out rejected");
  check(!SkeletalImportSkeleton(data, 99, &skels[0]).ok(),
        "skeletal: OOB skin rejected");
  check(!SkeletalImportClips(nullptr, &clips).ok(),
        "skeletal: clips null data rejected");
  {
    SkeletalClip c{};
    check(!SkeletalImportClip(data, 99, &c).ok(), "skeletal: OOB clip rejected");
  }
  check(!SerializeSkeleton(nullptr, &skel_blob).ok(),
        "skeletal: serialize null rejected");
  check(!SerializeClip(&clips[0], nullptr).ok(),
        "skeletal: clip serialize null rejected");
  // No partial append: failing plural leaves the vector at its base size
  // (asserted with the real zero-joint fixture below); a failed single
  // import leaves its out-param untouched.
  {
    SkeletalSkeleton keep{};
    keep.joints.resize(1);
    const bool bad = !SkeletalImportSkeleton(data, 99, &keep).ok();
    check(bad && keep.joints.size() == 1, "skeletal: failed import no touch");
  }
  // Zero-joint skin rejected (parsed from JSON text, no buffers needed —
  // the joint-count gate fires before any accessor read).
  {
    const char* zero_json =
        R"({"asset":{"version":"2.0"},"nodes":[{},{"name":"a"}],"skins":[{"joints":[]}]})";
    cgltf_options zopts{};
    cgltf_data* zdata = nullptr;
    bool parsed = cgltf_parse(&zopts, zero_json, std::strlen(zero_json),
                              &zdata) == cgltf_result_success &&
                  zdata != nullptr;
    check(parsed, "skeletal: zero-joint fixture parses");
    if (parsed) {
      SkeletalSkeleton z{};
      check(!SkeletalImportSkeleton(zdata, 0, &z).ok(),
            "skeletal: zero-joint skin rejected");
      std::vector<SkeletalSkeleton> zl;
      zl.resize(1);
      check(!SkeletalImportSkeletons(zdata, &zl).ok() && zl.size() == 1,
            "skeletal: plural rolls back to base");
      cgltf_free(zdata);
    }
  }

  cgltf_free(data);
#endif

  std::printf(failures == 0 ? "SKELETAL_IMPORT_OK\\n" : "SKELETAL_IMPORT_FAIL\\n");
  return failures == 0 ? 0 : 1;
}
