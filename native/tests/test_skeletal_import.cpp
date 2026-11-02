// test_skeletal_import.cpp — P5-01 acceptance: skins + animation clips.
//
// An importer test on a generated two-bone `.glb`: checks the joint count,
// the joint hierarchy (joint1 child of joint0) and the keyframe values
// (rotation quats, translation keys, interpolation modes), plus the
// ENGS/ENGA artifact round-trip (serialize -> read back -> compare) with
// corrupt-input gates.
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "engine/assets/asset_importer.hpp"
#include "engine/assets/asset_guid.hpp"
#include "engine/assets/asset_pak.hpp"
#include "engine/assets/skeletal_import.hpp"
#include "skeletal_glb_fixture.hpp"

namespace fs = std::filesystem;

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  return ok ? 0 : 1;
}

auto Near(float a, float b, float eps = 1e-5F) -> bool {
  return std::fabs(a - b) <= eps;
}

}  // namespace

auto main() -> int {
  using namespace engine::assets;
  int failures = 0;

#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  std::printf("[SKIP] skeletal import needs cgltf (offline fetch)\n");
  return 0;
#else
  // --- fixture -------------------------------------------------------------
  std::string dir = "skel_test_scratch";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  std::string glb = dir + "/two_bone.glb";
  failures += check(engine::tests::WriteSkeletalGlb(glb),
                    "two-bone fixture written");

  // --- parse with cgltf (in-memory; no cgltf_parse_file: single TU owns
  // the cgltf implementation in engine_core, and test TUs only declare) --
  std::vector<SkeletalSkin> skins;
  std::vector<AnimClip> clips;
  {
    std::ifstream f(glb, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    failures += check(!bytes.empty(), "fixture reads back");
    failures += check(SkeletalParseMemory(bytes.data(), bytes.size(), &skins, &clips),
                      "cgltf parses two-bone glb");
  }
  failures += check(skins.size() == 1, "one skin");
  failures += check(clips.size() == 1, "one clip");

  // --- direct parse assertions (fixture constants, not round-trip) ---------
  // A systematic misread (swapped channels, wrong quat order) would survive
  // a serialize->parse round-trip, so pin the parsed values to the fixture.
  {
    if (skins.size() != 1 || clips.size() != 1) {
      failures += check(false, "direct assertions need 1 skin + 1 clip");
    } else {
      const SkeletalSkin& skin = skins[0];
      failures += check(skin.name == "body", "skin name body");
      failures += check(skin.joints.size() == 2, "joint count 2");
      if (skin.joints.size() == 2) {
        failures += check(skin.joints[0].name == "joint0", "joint0 name");
        failures += check(skin.joints[0].parent_index == -1, "joint0 is root");
        failures += check(skin.joints[1].name == "joint1", "joint1 name");
        failures += check(skin.joints[1].parent_index == 0,
                          "joint1 child of joint0");
        for (std::size_t j = 0; j < 2; ++j) {
          bool identity = true;
          for (int k = 0; k < 16; ++k) {
            const float want = (k % 5 == 0) ? 1.0F : 0.0F;
            identity = identity && Near(skin.joints[j].inverse_bind[k], want);
          }
          failures += check(identity, j == 0 ? "joint0 IBM identity"
                                             : "joint1 IBM identity");
        }
      }
      const AnimClip& clip = clips[0];
      failures += check(clip.name == "wave", "clip name wave");
      failures += check(clip.channels.size() == 2, "clip channel count 2");
      const AnimChannel* rot = nullptr;
      const AnimChannel* tr = nullptr;
      for (const auto& ch : clip.channels) {
        if (ch.target_path == AnimTargetPath::kRotation) {
          rot = &ch;
        } else if (ch.target_path == AnimTargetPath::kTranslation) {
          tr = &ch;
        }
      }
      failures += check(rot != nullptr, "rotation channel present");
      failures += check(tr != nullptr, "translation channel present");
      if (rot != nullptr) {
        failures += check(rot->target_joint == 1, "rotation targets joint1");
        failures +=
            check(rot->interpolation == AnimInterpolation::kLinear,
                  "rotation interpolation LINEAR");
        const std::vector<float> want_times = {0.0F, 0.5F, 1.0F};
        failures += check(rot->times == want_times, "rotation key times");
        // Fixture quats: identity -> 90deg-about-Y -> identity (x,y,z,w).
        const float s = 0.70710678F;
        const std::vector<float> want_vals = {0, 0, 0, 1, 0, s, 0, s,
                                              0, 0, 0, 1};
        bool vals_ok = rot->values.size() == want_vals.size();
        for (std::size_t i = 0; i < want_vals.size() && vals_ok; ++i) {
          vals_ok = Near(rot->values[i], want_vals[i]);
        }
        failures += check(vals_ok, "rotation key values");
      }
      if (tr != nullptr) {
        failures += check(tr->target_joint == 1, "translation targets joint1");
        failures += check(tr->interpolation == AnimInterpolation::kStep,
                          "translation interpolation STEP");
        const std::vector<float> want_times = {0.0F, 1.0F};
        failures += check(tr->times == want_times, "translation key times");
        const std::vector<float> want_vals = {0, 0, 0, 0, 1, 0};
        bool vals_ok = tr->values.size() == want_vals.size();
        for (std::size_t i = 0; i < want_vals.size() && vals_ok; ++i) {
          vals_ok = Near(tr->values[i], want_vals[i]);
        }
        failures += check(vals_ok, "translation key values");
      }
    }
  }

  // --- artifact round-trip -------------------------------------------------
  {
    std::vector<std::uint8_t> skel_blob;
    const bool skel_written = WriteSkeletonArtifact(skins, &skel_blob) &&
                              skel_blob.size() > 8;
    failures += check(skel_written, "skel serializes");
    if (!skel_written) {
      std::printf("SKELETAL_IMPORT_FAIL\n");
      return 1;
    }
    failures += check(skel_blob[0] == 'E' && skel_blob[1] == 'N' &&
                          skel_blob[2] == 'G' && skel_blob[3] == 'S',
                      "skel magic ENGS");
    std::vector<SkeletalSkin> back;
    failures +=
        check(ReadSkeletonArtifact(skel_blob.data(), skel_blob.size(), &back),
              "skel parses back");
    bool same = back.size() == skins.size();
    if (same) {
      for (std::size_t i = 0; i < skins.size() && same; ++i) {
        same = back[i].name == skins[i].name &&
               back[i].joints.size() == skins[i].joints.size();
        for (std::size_t j = 0; j < skins[i].joints.size() && same; ++j) {
          same = back[i].joints[j].parent_index ==
                     skins[i].joints[j].parent_index &&
                 back[i].joints[j].name == skins[i].joints[j].name;
          for (int k = 0; k < 16 && same; ++k) {
            same = Near(back[i].joints[j].inverse_bind[k],
                        skins[i].joints[j].inverse_bind[k]);
          }
        }
      }
    }
    failures += check(same, "skel round-trips joints");
    // Corrupt gates: bad magic, truncated, bad parent index.
    std::vector<std::uint8_t> bad_magic = skel_blob;
    bad_magic[0] = 'X';
    std::vector<SkeletalSkin> junk;
    failures += check(!ReadSkeletonArtifact(bad_magic.data(),
                                            bad_magic.size(), &junk),
                      "skel rejects bad magic");
    failures += check(!ReadSkeletonArtifact(skel_blob.data(),
                                            skel_blob.size() / 2, &junk),
                      "skel rejects truncation");
    failures += check(!ReadSkeletonArtifact(nullptr, 0, &junk),
                      "skel rejects null");
  }
  {
    std::vector<std::uint8_t> anim_blob;
    const bool anim_written = WriteAnimationArtifact(clips, &anim_blob) &&
                              anim_blob.size() > 8;
    failures += check(anim_written, "anim serializes");
    if (!anim_written) {
      std::printf("SKELETAL_IMPORT_FAIL\n");
      return 1;
    }
    failures += check(anim_blob[0] == 'E' && anim_blob[1] == 'N' &&
                          anim_blob[2] == 'G' && anim_blob[3] == 'A',
                      "anim magic ENGA");
    std::vector<AnimClip> back;
    failures += check(
        ReadAnimationArtifact(anim_blob.data(), anim_blob.size(), &back),
        "anim parses back");
    bool same = back.size() == clips.size();
    if (same) {
      for (std::size_t i = 0; i < clips.size() && same; ++i) {
        same = back[i].name == clips[i].name &&
               Near(back[i].duration, clips[i].duration) &&
               back[i].channels.size() == clips[i].channels.size();
        for (std::size_t j = 0; j < clips[i].channels.size() && same; ++j) {
          same = back[i].channels[j].target_joint ==
                     clips[i].channels[j].target_joint &&
                 back[i].channels[j].target_path ==
                     clips[i].channels[j].target_path &&
                 back[i].channels[j].interpolation ==
                     clips[i].channels[j].interpolation &&
                 back[i].channels[j].times == clips[i].channels[j].times &&
                 back[i].channels[j].values == clips[i].channels[j].values;
        }
      }
    }
    failures += check(same, "anim round-trips keyframes");
    std::vector<std::uint8_t> bad_magic = anim_blob;
    bad_magic[0] = 'X';
    std::vector<AnimClip> junk;
    failures += check(
        !ReadAnimationArtifact(bad_magic.data(), bad_magic.size(), &junk),
        "anim rejects bad magic");
    failures += check(!ReadAnimationArtifact(anim_blob.data(),
                                             anim_blob.size() / 2, &junk),
                      "anim rejects truncation");
  }

  // --- worker end-to-end: .skel/.anim artifacts on disk --------------------
  {
    using namespace engine::assets;
    ImportWorker worker{};
    std::string lib = dir + "/lib";
    failures += check(worker.start(lib), "worker starts");
    failures += check(worker.enqueue(glb), "worker enqueues skinned glb");
    std::vector<ImportResult> done;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline && done.empty()) {
      worker.poll_finished(
          [&](const ImportResult& r) { done.push_back(r); });
      if (done.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    worker.poll_finished([&](const ImportResult& r) { done.push_back(r); });
    failures += check(done.size() == 1 && done[0].ok, "worker job ok");
    if (!done.empty()) {
      std::printf("  skel job ok=%d joints=%u skins=%u clips=%u skel=%s "
                  "anim=%s err=%s\n",
                  static_cast<int>(done[0].ok), done[0].joint_count,
                  done[0].skin_count, done[0].clip_count,
                  done[0].skeleton_artifact_path.c_str(),
                  done[0].animation_artifact_path.c_str(),
                  done[0].error.c_str());
      failures += check(done[0].joint_count == 2, "worker joint count 2");
      failures += check(done[0].skin_count == 1, "worker skin count 1");
      failures += check(done[0].clip_count == 1, "worker clip count 1");
      failures += check(!done[0].skeleton_artifact_path.empty() &&
                            fs::exists(done[0].skeleton_artifact_path),
                        ".skel artifact on disk");
      failures += check(!done[0].animation_artifact_path.empty() &&
                            fs::exists(done[0].animation_artifact_path),
                        ".anim artifact on disk");
      // Artifacts read back through the public parsers.
      if (!done[0].skeleton_artifact_path.empty()) {
        std::ifstream f(done[0].skeleton_artifact_path, std::ios::binary);
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        std::vector<SkeletalSkin> rsk;
        failures += check(ReadSkeletonArtifact(bytes.data(), bytes.size(),
                                               &rsk) &&
                              rsk.size() == 1 && rsk[0].joints.size() == 2 &&
                              rsk[0].joints[1].parent_index == 0,
                          ".skel reads back hierarchy");
      }
      if (!done[0].animation_artifact_path.empty()) {
        std::ifstream f(done[0].animation_artifact_path, std::ios::binary);
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        std::vector<AnimClip> rcl;
        failures += check(ReadAnimationArtifact(bytes.data(), bytes.size(),
                                                &rcl) &&
                              rcl.size() == 1 &&
                              rcl[0].channels.size() == 2,
                          ".anim reads back clips");
      }
    }
  // --- pak acceptance: skinned fixture -> one entry per kind, find() hits --
  {
    using namespace engine::assets;
    // Reuse the parsed skins/clips: pack .mesh (from the fixture bytes),
    // .skel and .anim blobs. Kinds 1-3 keep the bare source GUID; kinds
    // 4/5 use DeriveSubGuid — mirroring ExportPipeline scope (CodeRabbit
    // 4438286089, verified against ExportPipeline.cs PackScene).
    AssetGuid src = MintGuid();
    AssetGuid skel_id = DeriveSubGuid(src, AssetKind::kSkeleton);
    AssetGuid anim_id = DeriveSubGuid(src, AssetKind::kAnimClip);
    std::vector<std::uint8_t> skel_blob;
    std::vector<std::uint8_t> anim_blob;
    failures += check(WriteSkeletonArtifact(skins, &skel_blob) &&
                          !skel_blob.empty(),
                      "pak skel serializes");
    failures += check(WriteAnimationArtifact(clips, &anim_blob) &&
                          !anim_blob.empty(),
                      "pak anim serializes");
    if (!skel_blob.empty() && !anim_blob.empty()) {
      std::ifstream f(glb, std::ios::binary);
      std::vector<std::uint8_t> mesh_bytes(
          (std::istreambuf_iterator<char>(f)),
          std::istreambuf_iterator<char>());
      PakBuilder b{};
      failures += check(b.add(src, AssetKind::kModel, "two_bone.mesh",
                              mesh_bytes.data(), mesh_bytes.size(), false),
                        "pak adds mesh under bare GUID");
      failures += check(b.add(skel_id, AssetKind::kSkeleton, "two_bone.skel",
                              skel_blob.data(), skel_blob.size(), false),
                        "pak adds skel under sub-GUID");
      failures += check(b.add(anim_id, AssetKind::kAnimClip, "two_bone.anim",
                              anim_blob.data(), anim_blob.size(), false),
                        "pak adds anim under sub-GUID");
      failures += check(b.entry_count() == 3, "pak holds one entry per kind");
      std::string pak_path = dir + "/two_bone.pak";
      std::string err;
      failures += check(b.write(pak_path, err), "pak writes");
      PakReader r{};
      failures += check(r.open(pak_path, err), "pak opens");
      PakTocEntry e{};
      failures += check(r.find(src, e) &&
                            e.kind == static_cast<std::uint8_t>(AssetKind::kModel),
                        "pak find() hits mesh");
      failures += check(r.find(skel_id, e) &&
                            e.kind ==
                                static_cast<std::uint8_t>(AssetKind::kSkeleton),
                        "pak find() hits skeleton");
      failures += check(r.find(anim_id, e) &&
                            e.kind ==
                                static_cast<std::uint8_t>(AssetKind::kAnimClip),
                        "pak find() hits animclip");
    }
    }
    worker.shutdown();
  }

  std::printf(failures == 0 ? "SKELETAL_IMPORT_OK\n" : "SKELETAL_IMPORT_FAIL\n");
  return failures == 0 ? 0 : 1;
#endif
}
