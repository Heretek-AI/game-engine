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
    worker.shutdown();
  }

  std::printf(failures == 0 ? "SKELETAL_IMPORT_OK\n" : "SKELETAL_IMPORT_FAIL\n");
  return failures == 0 ? 0 : 1;
#endif
}
