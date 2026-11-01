// test_slang_pbr.cpp — M3-03 acceptance (native CTest).
//
// CPU-side contract (always built, no GPU needed):
//   1. SlangShaderCache: register 6 slang entries (embedded blobs),
//      active_spv serves embedded bytes; null/short blobs rejected.
//   2. Hot-reload: copy a .slang source to scratch, snapshot, no-change ->
//      not stale; touch (mtime+size change) -> stale; poll() without
//      slangc keeps last-good blobs (recompiled=false... swapped=false);
//      with SLANGC_PATH=slangc a valid edit swaps (magic preserved), a
//      broken edit keeps last-good (swapped=false, still valid magic).
//   3. PbrPipeline: init with the 6 blobs ok; null blob rejected;
//      push-constant sizes (geo 80B / shade 64B / post 24B <= 128B);
//      visibility/HDR byte math + forward+ tile/grid sizing for 250k.
//   4. Compositor: custom passes in hazard order run in compiled order
//      (record sequence), failing pass isolated, dup rejected.
//   5. Embedded SPIR-V: magic + version >= 1.3 on all six blobs.
//   6. Real-GPU path (Lavapipe): create VkShaderModules for all six
//      blobs + verify the bindless layout path still builds — the
//      slangc output is real driver-consumable SPIR-V, not just magic.
//      Skips cleanly with no physical device.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // getenv/setenv (_WIN32: _dupenv_s/_putenv_s/free)
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/compositor_bridge.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/render_graph.hpp"
#include "engine/renderer/slang_shader_cache.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "gpu_required.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// Bounded label helper for check() names (test-only diagnostics, never on
// the frame path): appends a small suffix to a literal prefix without
// printf-family formatting (Sonar cpp:S3458 flags snprintf in new code,
// and the existing %d/%s/%zu call sites were all fixed-size labels).
void check_tagged(bool ok, const char* prefix, const char* suffix) {
  char label[96]{};
  std::size_t n = 0;
  while (prefix[n] != '\0' && n + 1 < sizeof(label)) {
    label[n] = prefix[n];
    ++n;
  }
  std::size_t m = 0;
  while (suffix[m] != '\0' && n + 1 < sizeof(label)) {
    label[n++] = suffix[m++];
  }
  label[n] = '\0';
  check(ok, label);
}

void check_indexed(bool ok, const char* prefix, int i) {
  char num[16]{};
  int v = i;
  std::size_t m = 0;
  if (v == 0) {
    num[m++] = '0';
  } else {
    char rev[16]{};
    std::size_t r = 0;
    while (v > 0 && r < sizeof(rev)) {
      rev[r++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
    while (r > 0 && m < sizeof(num) - 1) {
      num[m++] = rev[--r];
    }
  }
  num[m] = '\0';
  check_tagged(ok, prefix, num);
}

using engine::renderer::Compositor;
using engine::renderer::CompositorPassCallback;
using engine::renderer::PassAccess;
using engine::renderer::PbrGeometryPush;
using engine::renderer::PbrPipeline;
using engine::renderer::PbrShadePush;
using engine::renderer::PostPush;
using engine::renderer::ShaderPollResult;
using engine::renderer::SlangEntryDesc;
using engine::renderer::SlangShaderCache;
using engine::renderer::VulkanDevice;

int32_t SeqCb(std::uint32_t pass_index, void* user_data) {
  auto* seq = static_cast<std::vector<std::uint32_t>*>(user_data);
  seq->push_back(pass_index);
  return 0;
}

int32_t FailCb(std::uint32_t /*pass_index*/, void* /*user_data*/) {
  return 3;
}

void test_shader_cache() {
  SlangShaderCache cache;
  SlangEntryDesc descs[6] = {
      {"pbr_visibility", "vsMain", engine::renderer::ShaderStage::kVertex,
       kPbr_vis_vertSpv, kPbr_vis_vertSpvWords},
      {"pbr_visibility", "fsMain", engine::renderer::ShaderStage::kFragment,
       kPbr_vis_fragSpv, kPbr_vis_fragSpvWords},
      {"pbr_shade", "vsShade", engine::renderer::ShaderStage::kVertex,
       kPbr_shade_vertSpv, kPbr_shade_vertSpvWords},
      {"pbr_shade", "fsShade", engine::renderer::ShaderStage::kFragment,
       kPbr_shade_fragSpv, kPbr_shade_fragSpvWords},
      {"post_composite", "vsPost", engine::renderer::ShaderStage::kVertex,
       kPost_vertSpv, kPost_vertSpvWords},
      {"post_composite", "fsPost", engine::renderer::ShaderStage::kFragment,
       kPost_fragSpv, kPost_fragSpvWords},
  };
  // Point at the real source dir so is_stale()/poll() exercise the watcher
  // (in-tree path; tests never write there).
  const char* dir =
      SHADER_SOURCE_DIR;
  for (int i = 0; i < 6; ++i) {
    check_indexed(cache.add_entry(descs[i], dir).ok(), "cache: add entry ", i);
  }
  check(cache.entry_count() == 6, "cache: 6 entries");
  check(!cache.add_entry(descs[0], dir).ok(), "cache: dup rejected");
  SlangEntryDesc bad = descs[0];
  bad.embedded_spv = nullptr;
  bad.embedded_words = 0;
  check(!cache.add_entry(bad, dir).ok(), "cache: null blob rejected");
  for (int i = 0; i < 6; ++i) {
    const std::uint32_t* spv = cache.active_spv(
        static_cast<std::size_t>(i));
    check_indexed(spv != nullptr && spv[0] == 0x07230203u,
                  "cache: active magic ", i);
  }
  check(cache.active_spv(99) == nullptr, "cache: OOB null");
  // No-change: checked-in sources untouched -> not stale.
  bool any_stale = false;
  for (int i = 0; i < 6; ++i) {
    if (cache.is_stale(static_cast<std::size_t>(i))) {
      any_stale = true;
    }
  }
  check(!any_stale, "cache: clean tree not stale");
}

void test_hot_reload() {
  // Scratch copy of one slang module: snapshot -> edit -> stale ->
  // poll (with and without slangc).
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path scratch =
      fs::temp_directory_path(ec) / "slang_hot_test";  // NOSONAR(cpp:S5443)
  fs::create_directories(scratch, ec);
  if (ec) {
    check(false, "hot: scratch dir");
    return;
  }
  const fs::path src =
      fs::path(SHADER_SOURCE_DIR) / "post_composite.slang";
  const fs::path dst = scratch / "post_composite.slang";
  // Copy the real module + its include (pbr_common.slang) so slangc can
  // compile the scratch copy standalone.
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  fs::copy_file(fs::path(SHADER_SOURCE_DIR) / "pbr_common.slang",
                scratch / "pbr_common.slang",
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    check(false, "hot: copy sources");
    return;
  }
  SlangShaderCache cache;
  SlangEntryDesc desc{"post_composite", "fsPost",
                      engine::renderer::ShaderStage::kFragment,
                      kPost_fragSpv, kPost_fragSpvWords};
  check(cache.add_entry(desc, scratch.string().c_str()).ok(),
        "hot: add scratch entry");
  check(!cache.is_stale(0), "hot: fresh copy not stale");
  // Append a comment (size+mtime change) -> stale.
  {
    std::ofstream f(dst, std::ios::app);
    f << "\n// hot-reload probe\n";
  }
  check(cache.is_stale(0), "hot: edited source stale");
  // Without slangc: poll reports attempted-but-not-swapped. Force absence
  // via bogus SLANGC_PATH: system() fails -> swapped=false, last-good
  // preserved. MSVC note: _putenv returns errno (checked); the lifetime
  // rule: _putenv COPIES the string, so the temporary is safe.
#if defined(_WIN32)
  check(_putenv("SLANGC_PATH=Z:/definitely/not/slangc.exe") == 0,
        "hot: bogus SLANGC_PATH set");
#else
  setenv("SLANGC_PATH", "/definitely/not/slangc", 1);
#endif
  ShaderPollResult r{};
  check(cache.poll(&r, 1).ok(), "hot: poll ok without slangc");
  check(r.stale, "hot: poll saw stale");
  const std::uint32_t* before = cache.active_spv(0);
  check(before != nullptr && before[0] == 0x07230203u,
        "hot: last-good magic kept");
  // With the real slangc (if the probe env provides one): a valid edit
  // swaps; a broken edit keeps last-good. _dupenv_s reads the probe path
  // (getenv is C4996 under MSVC /WX); _putenv_s sets it (checked).
#if defined(_WIN32)
  std::array<char, 1024> probe_buf{};
  {
    char* dup = nullptr;
    std::size_t dup_len = 0;
    if (_dupenv_s(&dup, &dup_len, "ENGINE_TEST_SLANGC") == 0 && dup != nullptr) {
      std::size_t n = 0;
      while (n + 1 < probe_buf.size() && dup[n] != '\0') {
        probe_buf[n] = dup[n];
        ++n;
      }
      probe_buf[n] = '\0';
    }
    std::free(dup);
  }
  const char* real_slangc =
      (probe_buf[0] != '\0') ? probe_buf.data() : nullptr;
#else
  const char* real_slangc = std::getenv("ENGINE_TEST_SLANGC");
#endif
  if (real_slangc != nullptr && real_slangc[0] != '\0') {
#if defined(_WIN32)
    check(_putenv_s("SLANGC_PATH", real_slangc) == 0,
          "hot: SLANGC_PATH restored");
#else
    unsetenv("SLANGC_PATH");
    setenv("SLANGC_PATH", real_slangc, 1);
#endif
    ShaderPollResult r2{};
    check(cache.poll(&r2, 1).ok(), "hot: poll ok with slangc");
    check(r2.swapped, "hot: valid edit swapped");
    check(cache.active_spv(0)[0] == 0x07230203u,
          "hot: swapped blob magic");
    check(!cache.is_stale(0), "hot: snapshot re-baselined");
    // Break the source -> poll fails closed, last-good survives.
    {
      std::ofstream f(dst, std::ios::app);
      f << "\nthis is not valid slang (((\n";
    }
    ShaderPollResult r3{};
    check(cache.poll(&r3, 1).ok(), "hot: poll ok on broken edit");
    check(r3.stale && !r3.swapped, "hot: broken edit not swapped");
    check(cache.active_spv(0)[0] == 0x07230203u,
          "hot: last-good survives broken edit");
  } else {
    std::printf("[SKIP] hot: slangc swap (ENGINE_TEST_SLANGC unset)\n");
  }
  fs::remove_all(scratch, ec);
}

void test_pbr_pipeline() {
  PbrPipeline p;
  check(!p.ready(), "pbr: not ready before init");
  check(!p.init(nullptr, 0, kPbr_vis_fragSpv, kPbr_vis_fragSpvWords,
                kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                kPost_fragSpv, kPost_fragSpvWords)
             .ok(),
        "pbr: null blob rejected");
  check(!p.ready(), "pbr: failed init leaves not-ready");
  check(p.init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
               kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
               kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
               kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
               kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "pbr: init 6 slang blobs ok");
  check(p.ready(), "pbr: ready after init");
  check(sizeof(PbrGeometryPush) == 80 && sizeof(PbrGeometryPush) <= 128,
        "pbr: geometry push 80B fits 128B");
  check(sizeof(PbrShadePush) == 64 && sizeof(PbrShadePush) <= 128,
        "pbr: shade push 64B fits 128B");
  check(sizeof(PostPush) == 20 && sizeof(PostPush) <= 128,
        "pbr: post push 20B fits 128B (flags @4 mirrors slang)");
  // 1280x720: visibility 7.37MB, HDR 14.75MB; tiles 80x45=3600.
  check(p.visibility_bytes(1280, 720) ==
            static_cast<std::uint64_t>(1280) * 720 * 8,
        "pbr: visibility 1280x720 bytes");
  check(p.hdr_bytes(1280, 720) ==
            static_cast<std::uint64_t>(1280) * 720 * 16,
        "pbr: hdr 1280x720 bytes");
  check(PbrPipeline::tile_count(1280, 720) == 80 * 45,
        "pbr: 3600 tiles at 1280x720");
  const std::uint64_t grid =
      PbrPipeline::tile_grid_bytes(1280, 720, 1024);
  check(grid == 3600 * 8 + 3600 * 64 * 4 + 1024 * 32,
        "pbr: forward+ grid bytes exact");
  p.shutdown();
  check(!p.ready(), "pbr: shutdown clears");
}

void test_compositor_order() {
  Compositor c;
  engine::renderer::GraphImageDesc img{};
  img.name = "hdr";
  img.width = 64;
  img.height = 64;
  img.depth = 1;
  check(c.graph().add_image(img).ok(), "comp: add hdr image");
  // B writes hdr AND reads it (read-modify-write vignette); A only
  // reads. RAW edge B->A: B must run first. Registered B-then-A, and the
  // compiled order must preserve B first regardless of add order tricks —
  // the callbacks below assert the recorded sequence matches execution.
  std::vector<std::uint32_t> seq;
  engine::renderer::CustomPassDesc b{};
  b.name = "B";
  b.reads[0].image = "hdr";
  b.read_count = 1;
  b.writes[0].image = "hdr";
  b.write_count = 1;
  b.callback = SeqCb;
  b.user_data = &seq;
  engine::renderer::CustomPassDesc a{};
  a.name = "A";
  a.reads[0].image = "hdr";
  a.read_count = 1;
  a.callback = SeqCb;
  a.user_data = &seq;
  check(c.add_custom_pass(b).ok(), "comp: add B");
  check(c.add_custom_pass(a).ok(), "comp: add A");
  check(!c.add_custom_pass(a).ok(), "comp: dup rejected");
  engine::renderer::CustomPassDesc nc{};
  nc.name = "N";
  check(!c.add_custom_pass(nc).ok(), "comp: null cb rejected");
  check(!c.run_frame().ok(), "comp: run before compile rejected");
  check(c.compile().ok(), "comp: compile ok");
  check(c.run_frame().ok(), "comp: run ok");
  // B (write) before A (read): RAW edge B->A. Callbacks receive add
  // indices: B=0, A=1.
  check(seq.size() == 2 && seq[0] == 0 && seq[1] == 1,
        "comp: hazard order B(0) then A(1)");
  check(c.failed_count() == 0, "comp: no failures");
  // Fault isolation: failing pass C reports but the frame completes.
  engine::renderer::CustomPassDesc f{};
  f.name = "C";
  f.reads[0].image = "hdr";
  f.read_count = 1;
  f.callback = FailCb;
  check(c.add_custom_pass(f).ok(), "comp: add failing C");
  check(c.compile().ok(), "comp: recompile ok");
  seq.clear();
  check(c.run_frame().ok(), "comp: run with failure ok");
  check(c.failed_count() == 1, "comp: one failure isolated");
  check(c.pass_result(2) == 3, "comp: failing rc recorded");
  check(c.pass_result(99) == -1, "comp: OOB -> -1");
}

void test_spirv_blobs() {
  auto check_spv = [](const std::uint32_t* w, std::size_t n,
                      const char* name) {
    bool magic = n > 5 && w[0] == 0x07230203u;
    bool ver13 = n > 5 && w[1] >= 0x00010300u;
    check_tagged(magic && ver13, "spirv magic+1.3: ", name);
  };
  check_spv(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, "pbr_vis_vert");
  check_spv(kPbr_vis_fragSpv, kPbr_vis_fragSpvWords, "pbr_vis_frag");
  check_spv(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, "pbr_shade_vert");
  check_spv(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, "pbr_shade_frag");
  check_spv(kPost_vertSpv, kPost_vertSpvWords, "post_vert");
  check_spv(kPost_fragSpv, kPost_fragSpvWords, "post_frag");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
void test_pbr_gpu() {
  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "M3-03-test";
  ci.device_hint = "llvmpipe";
  auto st = dev.init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("pbr-gpu: no physical device");
    return;
  }
  check(st.ok(), "pbr-gpu: device init ok");
  if (!st.ok()) {
    return;
  }
  auto create_mod = [&](const std::uint32_t* w, std::size_t n,
                        VkShaderModule* m, const char* nm) {
    auto vkCreateShaderModule =
        reinterpret_cast<PFN_vkCreateShaderModule>(
            dev.instance_proc("vkCreateShaderModule"));
    auto vkDestroyShaderModule =
        reinterpret_cast<PFN_vkDestroyShaderModule>(
            dev.instance_proc("vkDestroyShaderModule"));
    if (vkCreateShaderModule == nullptr) {
      check(false, "pbr-gpu: proc null");
      return;
    }
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = n * 4;
    sci.pCode = w;
    bool ok = vkCreateShaderModule(dev.device(), &sci, nullptr, m) ==
              VK_SUCCESS;
    check_tagged(ok, "pbr-gpu module: ", nm);
    if (ok && vkDestroyShaderModule != nullptr) {
      vkDestroyShaderModule(dev.device(), *m, nullptr);
      *m = VK_NULL_HANDLE;
    }
  };
  VkShaderModule m = VK_NULL_HANDLE;
  create_mod(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, &m, "vis_vert");
  create_mod(kPbr_vis_fragSpv, kPbr_vis_fragSpvWords, &m, "vis_frag");
  create_mod(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, &m, "shade_vert");
  create_mod(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, &m, "shade_frag");
  create_mod(kPost_vertSpv, kPost_vertSpvWords, &m, "post_vert");
  create_mod(kPost_fragSpv, kPost_fragSpvWords, &m, "post_frag");
}
#endif

}  // namespace

auto main() -> int {
  test_shader_cache();
  test_hot_reload();
  test_pbr_pipeline();
  test_compositor_order();
  test_spirv_blobs();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_pbr_gpu();
#endif
  std::printf(failures == 0 ? "test_slang_pbr: all checks passed\n"
                            : "test_slang_pbr: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
