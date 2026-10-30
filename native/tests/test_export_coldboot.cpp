// test_export_coldboot.cpp — M5-04 AC3: exported-game cold-boot gate.
//
// Launches the staged NativeAOT ExportSample binary 5x as FRESH processes
// (no handle reuse — true cold boot: loader + mmap + ABI + world + frame)
// and asserts every run exits 0 with EXPORT-SAMPLE-OK on stdout and wall
// <1000ms. Prints per-run timings + worst.
//
// Binary discovery (first hit wins):
//   1. <build>/export_stage/ExportSample  (ctest export fixture layout)
//   2. <repo>/samples/ExportSample/bin/Release/net10.0/<rid>/publish/
//   3. compositor_live/ExportSample        (staged probe sibling)
// The .pak the game cold-opens is staged beside it by the export fixture;
// the sample itself only needs engine_core + flecs siblings (same dir).
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <sys/stat.h>
#include <unistd.h>
#define POPEN popen
#define PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace {

auto BinDir() -> std::string {
#if defined(_WIN32)
  char buf[4096]{};
  DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
  std::string p(buf, n > 0 ? n : 0);
#else
  // /proc/self/exe -> <build>/native/tests/engine_export_coldboot.
  char buf[4096]{};
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  std::string p = (n > 0) ? std::string(buf, static_cast<std::size_t>(n)) : "";
#endif
  auto pos = p.find_last_of("/\\");
  return (pos == std::string::npos) ? "." : p.substr(0, pos);
}

auto FindGame() -> std::string {
  std::string here = BinDir();
#if defined(_WIN32)
  const std::string kExe = "ExportSample.exe";
#else
  const std::string kExe = "ExportSample";
#endif
  std::vector<std::string> cands;
  cands.push_back(here + "/export_stage/" + kExe);
  cands.push_back(here + "/../export_stage/" + kExe);
  // Repo publish layout: <repo>/samples/ExportSample/bin/Release/...
  fs::path dir(here);
  for (int i = 0; i < 8; ++i) {
    fs::path slnx = dir / "Engine.slnx";
    std::error_code ec;
    if (fs::exists(slnx, ec)) {
      for (const char* rid : {"linux-x64", "win-x64", "osx-arm64"}) {
        fs::path p = dir / "samples" / "ExportSample" / "bin" / "Release" /
                     "net10.0" / rid / "publish" / kExe;
        cands.push_back(p.string());
      }
      break;
    }
    dir = dir.parent_path();
    if (dir.empty()) {
      break;
    }
  }
  for (const auto& c : cands) {
    std::error_code ec;
    if (fs::exists(c, ec)) {
      return c;
    }
  }
  return "";
}

auto RunOnce(const std::string& game, std::string& out, double& wall_ms)
    -> int {
  // Quote the binary path (spaces in build dirs are legal on all OSes).
  std::string cmd = "\"" + game + "\" 2>&1";
  auto t0 = std::chrono::steady_clock::now();
  FILE* pipe = POPEN(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) !=
         nullptr) {
    out += buf.data();
  }
  int rc = PCLOSE(pipe);
  wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                               t0)
          .count();
  return rc;
}

}  // namespace

auto main() -> int {
  std::string game = FindGame();
  if (game.empty()) {
    std::printf("[FAIL] coldboot: ExportSample binary not staged\n");
    return 1;
  }
  std::printf("coldboot: game=%s\n", game.c_str());
  double worst = 0.0;
  for (int i = 0; i < 5; ++i) {
    std::string out;
    double wall = 0.0;
    int rc = RunOnce(game, out, wall);
    worst = wall > worst ? wall : worst;
    bool ok = (rc == 0) && (out.find("EXPORT-SAMPLE-OK") != std::string::npos);
    std::printf("[%s] coldboot run %d: wall=%.1fms rc=%d\n", ok ? "PASS" : "FAIL",
                i, wall, rc);
    if (!ok) {
      std::printf("  output tail: %s\n", out.substr(out.size() > 500 ? out.size() - 500 : 0).c_str());
      return 1;
    }
    if (wall >= 1000.0) {
      std::printf("[FAIL] coldboot run %d %.1fms >= 1000ms gate\n", i, wall);
      return 1;
    }
  }
  std::printf("[PASS] coldboot: 5/5 runs OK, worst %.1fms (gate <1000ms)\n", worst);
  std::printf("EXPORT_COLDBOOT_OK worst_ms=%.1f\n", worst);
  return 0;
}
