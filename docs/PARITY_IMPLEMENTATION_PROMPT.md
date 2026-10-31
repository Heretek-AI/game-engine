# Implementation prompt: Unity parity roadmap (P0 to P7)

## 1. Mission

You will turn this engine from a tested headless core into a tool a game developer can use like Unity: it must draw to a window, take input, save scenes, have an editor window, render at modern visual quality, and ship the content systems (animation, UI, particles, terrain, navigation, 2D, networking) that Unity users expect. The work is cut into small slices, one GitHub issue each, titled `[Px-yy]`; this file is the specification every one of those issues points to.

**Read this whole file before you write any code, even if your issue is one slice.** Your training data is out of date for this project: SDL 3.4, Dear ImGui 1.93, Vulkan 1.4 headers, .NET 10 and this repo's own conventions differ from what you remember. Every fact in §4 was checked by running it on 2026-10-31. When this file and your memory disagree, this file wins.

## 2. How to work

1. Work one slice (one issue) per branch and per pull request. Do slices in ID order inside a milestone unless the issue's **Depends on** line says otherwise. Never start a slice whose dependencies are not merged.
2. A slice is done only when every **Acceptance** command in its section passes, exactly as written, on your machine and in CI. "Looks good" is never acceptance.
3. Branch name: `parity/<slice id lowercase>-<short-slug>`, for example `parity/p1-04-swapchain`. PR title: `[P1-04] <issue title>`. PR body must contain `Closes #<issue number>` and the acceptance commands with their real output pasted.
4. Stage files by explicit path (`git add path/one path/two`), never `git add -A` or `git add .`. One commit per logical step is fine; the PR is squash-merged.
5. Never skip, delete, comment out or weaken a test or an assertion to get green. Never turn a `[FAIL]` into a `[SKIP]`. If an existing test fails, find out why.
6. Never invent an API. Look it up in this order: (a) this file, (b) the header or source that is actually built (`deps/`, or the FetchContent source dir `build/<preset>/_deps/<name>-src/`), (c) a 10-line experiment under `build/spikes/` (git-ignored). Never rely on memory, and never on a docs website, which often describes an older major version.
7. Log every discovery, every discrepancy with this file, and every surprising error in `docs/NOTES.md` as a dated bullet with the command you ran and the output you saw. Commit it in the same PR.
8. If the same error survives 3 attempts at fixing it, stop. Write what you tried and the exact error into `docs/NOTES.md`, commit, push, and say so in the PR body instead of trying a 4th idea.
9. Every change to `native/bridge/include/engine/bridge/engine_abi.h` must (a) append new function pointers at the **end** of `EngineApiTable`, never in the middle, (b) bump `ENGINE_ABI_VERSION_MINOR` by exactly 1 per PR, (c) regenerate bindings with `dotnet run --project tools/abi_codegen -c Release -- .`, and (d) keep `engine_abi_layout` and the managed `AbiLayoutTests` green. Never hand-edit `managed/Engine.Bindings/NativeApi.g.cs`.
10. Follow the invariants in `AGENTS.md` (native owns frame state, no `malloc`/`new` on the frame path, blittable `IComponent` only). They apply to every slice.
11. Hard rules found by the spike (each one cost real debugging time):
    - Never let a GPU test pass without a GPU. Every new GPU test must print `[FAIL]` (not `[SKIP]`) when `ENGINE_REQUIRE_GPU=1` is set and no Vulkan device is found (§4.3).
    - Never rely on a window test passing without `SDL_VIDEO_DRIVER=offscreen` set. Without a display, the existing window test passes by checking that window creation *fails* (§4.2).
    - Never link a second copy of volk into an executable that links `engine_core`. volk lives inside `libengine_core`; resolve Vulkan functions through `VulkanDevice::proc()` / `instance_proc()` (§4.4).
    - Never read keyboard state with `SDL_GetKeyboardState` in engine code. Injected test events do not update it (§4.6).
    - Never edit files under `deps/` by hand. Upgrades are whole-tree replacements recorded in `deps/README.md`.
12. New C++ code must pass SonarCloud on the PR: no `strlen`, `snprintf`, `memset`, `memcpy` on non-trivial types, or C arrays in new code, tests included. Use `std::string_view`, `std::array` and `std::span` instead. Format numbers into text with `std::to_chars`.

## 3. Paths and setup

All paths are relative to the repository root (the folder that contains `Engine.slnx`). Run every command from there.

### 3.1 Toolchain (Ubuntu 24.04, the CI image `ubuntu-24.04`)

```bash
sudo apt-get update
sudo apt-get install -y ninja-build clang cmake \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools glslang-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
# .NET 10 SDK: either actions/setup-dotnet in CI, or on a dev box:
sudo apt-get install -y dotnet-sdk-10.0
```

Expected versions (checked 2026-10-31):

| Command | Expected output (first line) |
| --- | --- |
| `clang --version` | `Ubuntu clang version 18.x` or newer |
| `glslangValidator --version` | `Glslang Version: 11:15.1.0` |
| `vulkaninfo --summary \| grep deviceName` | `deviceName = llvmpipe (LLVM 20.1.2, 256 bits)` (lavapipe; version may differ) |
| `dotnet --version` | `10.0.1xx` |

### 3.2 Build and test

```bash
cmake --preset ci-linux
cmake --build --preset ci-linux
SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest --preset ci-linux --output-on-failure
dotnet build Engine.slnx -c Release --nologo
dotnet run --project managed/Engine.Tests -c Release --no-build
```

Use the preset (build dir `build/ci-linux`). An out-of-tree build dir such as `/tmp/build` hits bug P0-01 until it is fixed.

### 3.3 Offline or restricted networks

If GitHub tarball downloads are blocked but `git clone` works, clone each dependency and point FetchContent at it. This is how every fact in this file was verified:

```bash
mkdir -p ~/deps && cd ~/deps
git clone --depth 1 -b v5.6.0 https://github.com/jrouwe/JoltPhysics.git jolt
git clone --depth 1 -b v0.24 https://github.com/zeux/meshoptimizer.git meshopt
git clone https://github.com/richgel999/bc7enc.git bc7enc && git -C bc7enc checkout f66c2e489b07138f2673a2fb3d27c1aa1d565c48
git clone --depth 1 -b v1.15 https://github.com/jkuhlmann/cgltf.git cgltf
git clone https://github.com/nothings/stb.git stb && git -C stb checkout 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
git clone --depth 1 -b release-3.4.18 https://github.com/libsdl-org/SDL.git sdl3
git clone --depth 1 -b v1.93.0-docking https://github.com/ocornut/imgui.git imgui
cd -
cmake --preset ci-linux \
  -DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=$HOME/deps/jolt \
  -DFETCHCONTENT_SOURCE_DIR__ENGINE_MESHOPT=$HOME/deps/meshopt \
  -DFETCHCONTENT_SOURCE_DIR__ENGINE_BC7ENC=$HOME/deps/bc7enc \
  -DFETCHCONTENT_SOURCE_DIR__ENGINE_CGLTF=$HOME/deps/cgltf \
  -DFETCHCONTENT_SOURCE_DIR__ENGINE_STB=$HOME/deps/stb \
  -DFETCHCONTENT_SOURCE_DIR_SDL3=$HOME/deps/sdl3 \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=$HOME/deps/imgui
```

The variable name is `FETCHCONTENT_SOURCE_DIR_<NAME>` where `<NAME>` is the first argument of `FetchContent_Declare`, upper-cased. `_engine_meshopt` therefore becomes `FETCHCONTENT_SOURCE_DIR__ENGINE_MESHOPT` (two underscores).

### 3.4 Pinned new dependencies

Every new third-party dependency is fetched with `FetchContent_Declare(... URL <tarball> URL_HASH SHA256=<hash>)`, the same pattern as Jolt in `native/physics/CMakeLists.txt`. The hash could not be computed in the verification sandbox (tarball downloads were blocked), so the first slice that adds a dependency computes it on a machine with network access:

```bash
curl -sSL -o /tmp/dep.tar.gz <URL>
cmake -E sha256sum /tmp/dep.tar.gz
```

and records URL + hash in the CMake file and a row in `deps/README.md`.

| Dependency | Pin | Tarball URL | License | Used by |
| --- | --- | --- | --- | --- |
| SDL3 | `release-3.4.18` | `https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz` | Zlib | P1-02 onward |
| Dear ImGui (docking) | `v1.93.0-docking` | `https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0-docking.tar.gz` | MIT | P3 |
| Recast/Detour | `v1.6.0` | `https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz` | Zlib | P6-03 |
| Box2D | `v3.2.0` | `https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz` | MIT | P6-07 |
| RmlUi | `6.3` | `https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.tar.gz` | MIT | P5-04 |
| FreeType | `VER-2-14-3` | `https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz` | FTL (BSD-style, credit required) | P5-04 |
| ENet | `v1.3.18` | `https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz` | MIT | P7-04 |

Tags were confirmed with `git ls-remote --tags` on 2026-10-31. Every license above requires keeping the upstream copyright notice; add each to `THIRD_PARTY_NOTICES.md` (create it in the first PR that adds a dependency) with the license text.

## 4. Verified facts

### 4.1 State of `main` at 0fd0a3d (the starting point)

| Fact | Evidence |
| --- | --- |
| Engine renderer source records zero Vulkan commands | `grep -n "vkCmd" native/renderer/src/*.cpp \| wc -l` prints `0` |
| No swapchain anywhere in engine source | `grep -rn vkCreateSwapchainKHR native/renderer/src` prints nothing |
| `VulkanDevice::init` enables no device extensions at all (no `VK_KHR_swapchain`) | `native/renderer/src/vulkan_device.cpp` lines 248 to 330: `dc.enabledExtensionCount` is never set |
| `VulkanDevice` requires dynamicRendering, synchronization2, bufferDeviceAddress, descriptor indexing and drawIndirectCount, and lavapipe has them all | `engine_renderer_bootstrap` prints `gpu: llvmpipe ... api=1.4.318` and passes |
| CI installs no Vulkan headers, so the renderer builds as a stub and every GPU section prints `[SKIP]` | `.github/workflows/ci.yml` installs only `ninja-build clang` |
| SDL3 is found only through `find_package(SDL3)` or pkg-config, and Ubuntu 24.04 has no SDL3 package | `apt-cache search libsdl3` prints nothing |
| `EngineApiTable` has 44 entries; none for renderer devices, windows, input, physics bodies or collision events | `dotnet run --project tools/abi_codegen -c Release -- .` prints `abi_codegen: 44 entries` |
| Native collision events exist (`engine::physics::CollisionEvent`, `kEnter`/`kExit`, ring buffer fed by a Jolt `ContactListener`) but never cross the ABI | `native/physics/include/engine/physics/collision_events.hpp`, `native/physics/src/transform_sync.cpp:60` |
| No scene file format exists | no save/load code in `native/` or `managed/` |
| `engine-cli snapshot capture` returns `HeadlessStub: true` | `managed/Engine.Editor/CliDispatcher.cs:301` |
| The C# editor (`managed/Engine.Editor`) is a headless library: hierarchy model, inspector undo/redo, gizmo math, play mode, JSON-RPC over a Unix socket | `managed/Engine.Editor/*.cs` |
| Native tests are plain executables that print `[PASS] name` / `[FAIL] name` / `[SKIP] name` and return non-zero on any failure; managed tests are `Check(...)` calls summed in `managed/Engine.Tests/Program.cs` | `native/tests/test_gi_zero_bake.cpp`, `managed/Engine.Tests/Program.cs` |
| The root `CMakeLists.txt` sets `CMAKE_POSITION_INDEPENDENT_CODE ON` globally, so static third-party libs can be linked into `libengine_core.so` | `CMakeLists.txt:39` |

### 4.2 SDL3 3.4.18

- Fetching SDL3 statically into `engine_core_renderer` works with exactly this block (it replaces the whole `find_package(SDL3 QUIET)` ... pkg-config block in `native/renderer/CMakeLists.txt`). With it, the full CTest suite passed (32/32 of the tests that do not need the export or audio fixtures) and `engine_renderer_bootstrap` printed `[PASS] window: valid + extent`:

```cmake
option(ENGINE_WITH_SDL3 "Fetch and statically link SDL3 for real windows" ON)
if(ENGINE_WITH_SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz")
    FetchContent_MakeAvailable(SDL3)
    target_link_libraries(engine_core_renderer PUBLIC SDL3::SDL3-static)
    target_compile_definitions(engine_core_renderer PUBLIC ENGINE_RENDERER_HAS_SDL3=1)
    message(STATUS "renderer: SDL3 release-3.4.18 (static) — real window path")
else()
    message(STATUS "renderer: ENGINE_WITH_SDL3=OFF — stub RendererWindow (headless Vulkan OK)")
endif()
```

- **Surprise:** SDL3's configure step hard-fails on Linux when X11 is present but an X11 extension package is missing: `Couldn't find dependency package for XCURSOR. Please install the needed packages or configure with -DSDL_X11_XCURSOR=OFF`. Install the full apt list in §3.1. With it, SDL reports `Video drivers: dummy offscreen x11(dynamic)` (Wayland did not enable; that is fine for now).
- **Surprise:** with no display and no `SDL_VIDEO_DRIVER` set, `engine_renderer_bootstrap` still passes, printing `[PASS] window: create fails only for missing video device`. That check silently hides a missing window. CI must set `SDL_VIDEO_DRIVER=offscreen`.
- `SDL_VIDEO_DRIVER=offscreen` selects the offscreen driver (`SDL_GetCurrentVideoDriver()` returns `offscreen`). Its Vulkan instance extensions are exactly `VK_KHR_surface` and `VK_EXT_headless_surface`, and `SDL_Vulkan_CreateSurface` succeeds on lavapipe.
- `SDL_Init` returns `bool` in SDL3 (true on success), not `int`. `SDL_Vulkan_GetInstanceExtensions(&n)` returns `const char* const*` owned by SDL.

### 4.3 Swapchain and readback on lavapipe (headless surface)

Captured output of the reference program in Appendix A (`SDL_VIDEO_DRIVER=offscreen ./spike`):

```
video driver: offscreen
inst ext: VK_KHR_surface
inst ext: VK_EXT_headless_surface
gpu: llvmpipe (LLVM 20.1.2, 256 bits)
present support q0: 1
caps: cur 4294967295x4294967295 min 4 max 0 usage 0x8009f
fmt 37 cs 0
fmt 44 cs 0
present mode 1
present mode 2
swapchain 320x240 images 4
frame 0 image 0 present 0
frame 1 image 0 present 0
frame 2 image 0 present 0
pixel0 bytes: 255 128 0 255 (format 37)
OK
```

What this means for the code:

1. `currentExtent` is `0xFFFFFFFF x 0xFFFFFFFF` on a headless surface. When `currentExtent.width == 0xFFFFFFFFu`, use the window size from `RendererWindow::width()/height()`.
2. `minImageCount` is 4 and `maxImageCount` is 0 (no limit). Request `max(minImageCount, 2)`; never hard-code 2 or 3.
3. Formats offered: `37` = `VK_FORMAT_R8G8B8A8_UNORM` and `44` = `VK_FORMAT_B8G8R8A8_UNORM`, both `SRGB_NONLINEAR`. No `_SRGB` format is offered, so the engine must do sRGB encoding in the final shader (the existing `post_composite.slang` already does).
4. Present modes offered: `1` = MAILBOX and `2` = FIFO. Use FIFO; it is always available.
5. `supportedUsageFlags` includes `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (0x1), so swapchain images can be copied to a buffer for screenshots and golden tests. Create the swapchain with `COLOR_ATTACHMENT | TRANSFER_SRC`.
6. The headless swapchain hands back image index 0 every frame. Never assume indices rotate.
7. A clear to `(1.0, 0.5, 0.0, 1.0)` reads back as bytes `255 128 0 255` in `R8G8B8A8_UNORM`. Use exactly that for the first test.
8. Rendering the same ImGui frame twice produced byte-identical PNGs (`sha256 2490c4c6...` both runs). Lavapipe is deterministic on one machine. Across Mesa versions it is not guaranteed, so golden comparisons use the tolerance in P1-05.

### 4.4 volk inside `engine_core`

`deps/volk` is compiled into `libengine_core` with hidden visibility. A test or tool executable that links `engine_core` must not compile `volk.c` again. It includes `<volk.h>` only for types and resolves every function through `VulkanDevice::proc("vkCmdDraw")` (device level) or `VulkanDevice::instance_proc("vkGetPhysicalDeviceSurfaceFormatsKHR")` (instance level), as `native/tests/test_gpu_indirect.cpp` does. Engine code inside `native/renderer/src/` can call `vk*` directly because volk is in the same library.

### 4.5 Dear ImGui 1.93.0-docking with SDL3 and Vulkan

- Built from source files `imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp backends/imgui_impl_sdl3.cpp backends/imgui_impl_vulkan.cpp`.
- Compile definitions that work with the engine's volk rule: `VK_NO_PROTOTYPES` and `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, then load functions before `ImGui_ImplVulkan_Init`:

```cpp
struct ImGuiVkLoader { VkInstance instance; VkDevice device; };
static ImGuiVkLoader g_loader{instance, device};
const bool loaded = ImGui_ImplVulkan_LoadFunctions(
    VK_API_VERSION_1_3,
    [](const char* name, void* user) -> PFN_vkVoidFunction {
      auto* l = static_cast<ImGuiVkLoader*>(user);
      PFN_vkVoidFunction f = vkGetDeviceProcAddr(l->device, name);
      return f != nullptr ? f : vkGetInstanceProcAddr(l->instance, name);
    },
    &g_loader);  // returned true in the spike
```

  Inside `engine_core` (where volk is linked) `vkGetDeviceProcAddr`/`vkGetInstanceProcAddr` are the volk globals; outside it, wrap `VulkanDevice::proc` / `instance_proc` instead.
- **Surprise (API moved in 2025-09):** `ImGui_ImplVulkan_InitInfo` no longer has `RenderPass`, `Subpass`, `MSAASamples` or `PipelineRenderingCreateInfo` at the top level. They live in `InitInfo.PipelineInfoMain`. Working dynamic-rendering setup:

```cpp
ImGui_ImplVulkan_InitInfo ii{};
ii.ApiVersion = VK_API_VERSION_1_3;
ii.Instance = instance; ii.PhysicalDevice = physical; ii.Device = device;
ii.QueueFamily = graphics_family; ii.Queue = graphics_queue;
ii.DescriptorPoolSize = 16;           // backend creates its own pool
ii.MinImageCount = 2; ii.ImageCount = swapchain_image_count;
ii.UseDynamicRendering = true;
VkFormat color_format = swapchain_format;  // must outlive Init
ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
ImGui_ImplVulkan_Init(&ii);
```

- Per frame: `ImGui_ImplVulkan_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame(); ... ImGui::Render();` then inside `vkCmdBeginRendering`/`vkCmdEndRendering`: `ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);`. Init with `ImGui_ImplSDL3_InitForVulkan(window)`.
- Set `io.IniFilename = nullptr` in tests so no `imgui.ini` is written, and `io.ConfigFlags |= ImGuiConfigFlags_DockingEnable`.
- A 640x480 frame with one ImGui window over an orange clear produced 60000 non-clear pixels and the expected picture (window titled "Hierarchy", text "Entity 1", button "Play").

### 4.6 Input through SDL3 events

Captured output of the input spike (offscreen driver):

```
push: 1
got key scancode=26 down=1
got motion 10,20 rel 3
events=3 keystate[W]=0 numkeys=512
gamepads=0
```

- `SDL_PushEvent` with `SDL_EVENT_KEY_DOWN` (fields `key.scancode`, `key.key`, `key.down`, `key.windowID`) and `SDL_EVENT_MOUSE_MOTION` (`motion.x/y/xrel/yrel`) are delivered by `SDL_PollEvent` under the offscreen driver. That is how input tests inject input.
- **Surprise:** `SDL_GetKeyboardState()` still reports W as up after a pushed key-down. The engine must keep its own key-state table updated from events. Never read `SDL_GetKeyboardState`.
- `SDL_PollEvent` returned one more event than was pushed (3 for 2): SDL adds its own window events. Tests must filter by event type, never count all events.
- `SDL_SCANCODE_W` is `26`. Key codes are `SDLK_W` etc. Gamepads come from `SDL_GetGamepads(&count)` after `SDL_Init(SDL_INIT_GAMEPAD)`; none exist in CI, so gamepad tests use `SDL_EVENT_GAMEPAD_AXIS_MOTION` / `SDL_EVENT_GAMEPAD_BUTTON_DOWN` injected events.

### 4.7 Shaders

- New shaders in P1 to P3 are GLSL, compiled offline exactly the way `native/renderer/shaders/README.md` documents:

```bash
glslangValidator -V --target-env vulkan1.3 -o /tmp/tri_vert.spv native/renderer/shaders/tri.vert
python3 native/renderer/shaders/embed_spv.py /tmp/tri_vert.spv native/renderer/shaders/tri_vert_spv.inc
```

  Verified: a 5-line `#version 460` vertex shader using `gl_VertexIndex` compiles to 360 words and embeds. Check in both the `.vert`/`.frag` source and the `_spv.inc`. Never compile shaders at runtime.
- Using `gl_VertexIndex` needs `shaderDrawParameters`, which `VulkanDevice` already enables.
- The existing Slang PBR/GI/post blobs (`*_spv.inc`) load and their pipelines build on lavapipe (`engine_slang_pbr`, `engine_gi_zero_bake` pass). Editing `.slang` files needs `slangc`, which is not installable from apt; P4 slices reuse the checked-in blobs and only change them in the P4-00 spike if `slangc` is obtained.

### 4.8 .NET function pointers and reflection

```
F IsPointer=False IsFunctionPointer=True
V IsPointer=True IsFunctionPointer=False
```

For a field declared `delegate* unmanaged[Cdecl]<int, void> F`, `FieldInfo.FieldType.IsPointer` is **false** on .NET 10; use `IsFunctionPointer`. This is bug P0-01.

### 4.9 Compilers

- Supported and CI-tested: clang on Linux and macOS, MSVC on Windows.
- GCC 13.3 with `-Werror` fails on main. The first errors (from `ninja -k 0`), which P0-02 must fix:

```
native/bridge/src/dotnet_host.cpp:368:43: error: declaration of 'int& out' shadows a parameter [-Werror=shadow]
native/core/src/job_scheduler.cpp:202:20: error: conversion to 'size_t' from 'int' may change the sign of the result [-Werror=sign-conversion]
native/core/src/job_scheduler.cpp:282:22: (same)
native/physics/src/transform_sync.cpp:415:14: error: 'void* memset(...)' clearing an object of non-trivial type 'struct engine::physics::{anonymous}::PullRow' [-Werror=class-memaccess]
native/renderer/src/sdf_brick_builder.cpp:240..242: error: conversion from 'int64_t' to 'float' may change value [-Werror=conversion]
native/renderer/src/sdf_brick_builder.cpp:268..270: error: conversion from 'uint32_t' to 'float' may change value [-Werror=conversion]
native/renderer/src/vma_impl.cpp:24,25,30: error: unknown option after '#pragma GCC diagnostic' kind [-Werror=pragmas]
cc1: error: command-line option '-fno-rtti' is valid for C++/D/ObjC++ but not for C [-Werror]
build/_deps/meshopt-src/src/meshoptimizer.h:1154,1171: error: conversion to 'long unsigned int' from 'int' may change the sign [-Werror=sign-conversion]
```

### 4.10 Libraries already in the tree that later slices rely on

| Need | Already available | Evidence |
| --- | --- | --- |
| Terrain collider | Jolt `HeightFieldShapeSettings` | `Jolt/Physics/Collision/Shape/HeightFieldShape.h` |
| Character controller | Jolt `CharacterVirtual` | `Jolt/Physics/Character/CharacterVirtual.h` |
| Audio buses and effects | miniaudio `ma_sound_group_init`, `ma_node_graph_init`, `ma_lpf_node_init`, `ma_delay_node_init` | `deps/miniaudio/miniaudio.h` |
| glTF skins and animations | cgltf v1.15 | `native/assets/CMakeLists.txt` |
| Mesh LOD | meshoptimizer v0.24 `meshopt_simplify` | `native/assets/CMakeLists.txt` |
| PNG write for screenshots | `stb_image_write.h` in the stb fetch | spike wrote `frame.png` with `stbi_write_png` |

### 4.11 Not verified, be careful

Check these early in the slice that needs them and record the answer in `docs/NOTES.md`:

1. SHA256 of every new tarball (§3.4). Compute it; never leave `URL_HASH` out.
2. SDL3 static build on `macos-15` and `windows-2025` CI images. It should need no extra packages, but nobody has run it. P1-02 must check both CI jobs are green.
3. A real GPU (NVIDIA/AMD/Intel) with a real X11 or Wayland window. Everything here ran on lavapipe with the offscreen driver. P1-04 must also be tried once on a desktop with a GPU before the PR merges; paste the output.
4. Golden images across Mesa versions. The CI image may ship a different lavapipe; P1-05's tolerance exists for that.
5. macOS presentation needs MoltenVK and the portability extension; not attempted (P7-02).
6. RmlUi, FreeType, Recast, Box2D and ENet were only confirmed to exist at those tags. Their APIs were not exercised. Each milestone from P4 on starts with a `-00` spike slice that verifies the facts its slices need and adds them to this file in a PR before any slice code is written.
7. `engine_audio_spatial` hung for 300 s once under `ctest -j4` and passed alone in 0.6 s. Cause unknown (P0-03).
8. `engine_export_publish_aot` failed locally with `NETSDK1047: Assets file ... doesn't have a target for 'net10.0/linux-x64'` when run after a plain build; CI passes. Possibly Ubuntu's source-built SDK; P0-04 decides.

### 4.12 Visual fidelity facts (P4-00 spike, verified 2026-10-30 on lavapipe)

P4-01 through P4-12 stay thin by design: the entry points, expected
inputs and file list below are what those slices use. Do not invent
criteria beyond them.

#### How the existing pieces are driven from a real frame

- `PbrPipeline` (`native/renderer/include/engine/renderer/pbr_pipeline.hpp`):
  `init()` takes the six checked-in SPIR-V blobs (vis/shade/post vert+frag,
  `*_spv.inc`, SPIR-V >= 1.3, magic `0x07230203`); null or bad-magic blobs
  are rejected with no partial state. `create_gpu()` is deliberately
  DEFERRED: it returns `kInvalidArgument` ("pbr create_gpu deferred to
  compositor frame setup (M3-04)") until the compositor frame setup lands,
  so callers fail fast at the cause instead of dereferencing null pipelines.
  P4 slices wire vis -> shade -> post through the compositor, not by calling
  `create_gpu()` directly. Push-constant mirrors are byte-exact with the
  slang structs: `PbrGeometryPush` 80B, `PbrShadePush` 64B, `PostPush` 20B
  (exposure @0, flags @4). `PostPush` is 20B, not 24B: the README line that
  says "post 24B" is a typo (the header `static_assert` pins 20B).

```
$ SDL_VIDEO_DRIVER=offscreen ./build/ci-linux/native/tests/engine_slang_pbr
[PASS] pbr: init 6 slang blobs ok
[PASS] pbr: ready after init
[PASS] pbr: geometry push 80B fits 128B
[PASS] pbr: shade push 64B fits 128B
[PASS] pbr: post push 20B fits 128B (flags @4 mirrors slang)
```

- `GiFrameGraph` (`native/renderer/include/engine/renderer/gi_frame_graph.hpp`):
  `build(dev, bindless_set_layout, gi_vert, gi_vert_words, gi_frag,
  gi_frag_words, shadow_vert, shadow_vert_words, shadow_frag,
  shadow_frag_words, pbr)` — needs the bindless SET layout from
  `BindlessTable::create_gpu()` plus the four GI/shadow blobs; the three
  M3-03 PBR blobs are borrowed from the already-`init()`ed `PbrPipeline`.
  Builds its own pipeline layout with the fragment-stage push range the GI
  resolve + shadow passes need (the bindless GPU layout ships zero push
  ranges). Double-build is rejected (`kAlreadyInitialized`); `destroy()` runs
  after `VulkanDevice::shutdown()` (handles stored, not re-queried).

```
$ SDL_VIDEO_DRIVER=offscreen ./build/ci-linux/native/tests/engine_gi_zero_bake
[PASS] gi-gpu: graph build ok (Lavapipe)
[PASS] gi-gpu: gi + shadow pipelines live
[PASS] gi-gpu: double-build rejected
[PASS] gi-gpu: destroy clears
```

- `GpuCull` (`native/renderer/include/engine/renderer/gpu_cull.hpp`):
  `init(max_instances, cull_spv, cull_spv_words, hiz_spv, hiz_spv_words)`
  binds the two precompiled GLSL compute blobs (`cull_spv.inc`,
  `hiz_spv.inc`); `create_gpu(device, bindless_layout)` builds the two
  compute pipelines and allocates the indirect (capacity x
  `VkDrawIndexedIndirectCommand`), count (one u32) and visible (N x u32)
  buffers via VMA. Dispatch shape is 256 threads/group
  (`CullGroupCount`). Phase B reads its candidate count from its own push
  constant — no GPU→CPU readback between phases. The count-draw
  (`vkCmdDrawIndexedIndirectCount`) reads the DRAW counter buffer, which
  phase B owns and which is zeroed before dispatch (separate from phase A's
  candidate counter). CPU references (`CullFrustumCpu`, `CullHizCpu`) are the
  bit-exact contract the shaders must match.

```
$ SDL_VIDEO_DRIVER=offscreen ./build/ci-linux/native/tests/engine_gpu_indirect
[PASS] spirv: cull push 48B fits 128B
[PASS] spirv: hiz push 88B fits 128B
[PASS] spirv: frame block 160B
[PASS] indirect-gpu: count-draw submits on queue
GPU_INDIRECT_OK
```

- `SceneSsbo` (`native/renderer/include/engine/renderer/scene_ssbo.hpp`):
  `init(max_instances)` reserves the CPU shadow (no GPU); one
  `stream_begin(count)` / `stream_write(first, matrices, count)` /
  `stream_end()` triple per frame copies the dirty `WorldTransform` column
  ranges (packed 64B row-major matrices) into the ring, and
  `create_gpu(device, addr_hint)` allocates the persistent device SSBO plus
  upload staging via VMA. One `SceneInstance` is 96B (64B matrix + 16B
  sphere + ids/flags); 250k instances = 24MB in one allocation. Binding 0
  of the bindless set; `SceneMaterial` (64B) is binding 1.

```
$ SDL_VIDEO_DRIVER=offscreen ./build/ci-linux/native/tests/engine_gpu_indirect
[PASS] indirect-gpu: scene init
[PASS] indirect-gpu: scene gpu mark
```

  Frame order for P4-06: stream SceneSsbo → phase A cull → barrier →
  phase B Hi-Z → count-draw against the bindless set → vis/shade/post
  passes through the compositor. `PbrPipeline::create_gpu()` stops being a
  stub when the compositor frame setup (M3-04 owner) constructs the three
  graphics pipelines; until then the CPU plan + Lavapipe
  `vkCreateShaderModule` proofs above are the integration contract.

#### slangc in CI: yes, GitHub release asset, version 2026.20

- `slangc` is NOT installable from apt; CI fetches the prebuilt binary from
  the shader-slang GitHub release. Latest release at spike time is
  `v2026.20` (published 2026-10-17). Linux x86-64 asset for CI
  (`ubuntu-24.04`, glibc 2.28+):

```
$ curl -sSL -o /tmp/slang-linux.tar.gz \
    https://github.com/shader-slang/slang/releases/download/v2026.20/slang-2026.20-linux-x86_64-glibc-2.28.tar.gz
$ cmake -E sha256sum /tmp/slang-linux.tar.gz
84ebd777968caceea26b21bfd014341078be6047be6c5a963a69f984f896cebd  /tmp/slang-linux.tar.gz
$ tar xzf /tmp/slang-linux.tar.gz -C /tmp/slangtest && /tmp/slangtest/bin/slangc -v
2026.20
```

- `slangc -v` prints the bare version (`2026.20`); `--version` is rejected
  (`error[E00017]: unknown command-line option '--version'`). The ten
  `(stem, entry, stage)` triples in `slang_build.py:26-37` all compile with
  the flags that script uses (`-target spirv -emit-spirv-directly
  -fvk-use-entrypoint-name`); every output is magic `07230203`, SPIR-V
  1.5 (`00010500`, satisfies the >= 1.3 check):

```
$ for e in ...; do slangc -target spirv -emit-spirv-directly \
    -fvk-use-entrypoint-name -Inative/renderer/shaders \
    -entry $2 -stage $3 -o /tmp/p400spike/$4.spv \
    native/renderer/shaders/$1.slang; done
pbr_vis_vert: 710 words magic=07230203 ver=00010500
pbr_vis_frag: 174 words magic=07230203 ver=00010500
pbr_shade_vert: 153 words magic=07230203 ver=00010500
pbr_shade_frag: 1697 words magic=07230203 ver=00010500
post_vert: 153 words magic=07230203 ver=00010500
post_frag: 536 words magic=07230203 ver=00010500
gi_resolve_vert: 229 words magic=07230203 ver=00010500
gi_resolve_frag: 1405 words magic=07230203 ver=00010500
shadow_trace_vert: 232 words magic=07230203 ver=00010500
shadow_trace_frag: 1136 words magic=07230203 ver=00010500
```

- P4 slices that recompile shaders keep using the checked-in `*_spv.inc`
  blobs by default and only touch `.slang` sources when `slangc` is present;
  without `slangc` the hot-reload swap section keeps skipping
  (`[SKIP] hot: slangc swap (ENGINE_TEST_SLANGC unset)`) and the blob checks
  still run. `slang_build.py --slangc <path>` requires the shader dir to
  live inside the repo (its embed step refuses out-of-tree paths), so CI
  regenerates in-tree, never into `/tmp`.

#### Timestamp queries on lavapipe: supported

- lavapipe (`llvmpipe LLVM 19.1.7`, Vulkan 1.4.305 on Mesa 25.0.7) exposes
  `timestampPeriod = 1.0 ns` and 64 valid timestamp bits on its graphics
  queue; a two-timestamp pool (`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`) around a
  no-op submit returns monotonic readable results. The P3-09 profiler's
  timestamp-query plan around the scene pass works on lavapipe. Spike
  program: raw Vulkan (no engine), compiled `g++ -O1 -o p4_00_spike
  p4_00_spike.cpp -lvulkan`:

```
$ SDL_VIDEO_DRIVER=offscreen ./p4_00_spike
[PASS] instance create
[PASS] physical device count=1
  dev[0] llvmpipe (LLVM 19.1.7, 256 bits) api=1.4.305
[INFO] timestampPeriod=1.000000 ns
[PASS] timestampPeriod nonzero (timestamps supported)
[PASS] graphics queue family present
[PASS] queue timestampValidBits=64
[PASS] logical device create
[PASS] timestamp query pool create
[PASS] queue submit with timestamps
[PASS] fence wait
[PASS] query results readable
[INFO] t0=101869994453906 t1=101869994475016 delta=21110 ticks (21.110 us)
[PASS] t1 >= t0 (monotonic)
P4_00_TS_OK
```

#### File list P4-01…P4-12 start from

- Already in tree (wire up, do not recreate): `pbr_pipeline.hpp`,
  `gi_frame_graph.hpp`, `gpu_cull.hpp`, `scene_ssbo.hpp`,
  `bindless_table.hpp`, `compositor_bridge.hpp`, `gi_cascade.hpp`,
  `gi_shadow.hpp`, `NativeApi.g.cs` (ABI: append entries + minor bump +
  `tools/abi_codegen` + `engine_abi_layout`/`AbiLayoutTests` green).
- New per slice (golden + test + source named in each slice card).
- Golden harness is DEFINED by P1-05 (§P1-05: `golden_check(name, rgba,
  w, h)`) but NOT YET IN TREE at P4-00 spike time
  (`native/tests/golden_compare.hpp` absent; `native/tests/golden/`
  absent — P1-05 creates both per §5 target layout). Contract P4
  slices code against: pass when every channel differs by at most 2
  and at most 0.1% of pixels differ; `ENGINE_GOLDEN_DIR` compile
  definition locates goldens; `ENGINE_UPDATE_GOLDENS=1` overwrites the
  golden and prints `[PASS] golden: updated <name>`. P4 slices depend
  on P1-05 landing first (or vendor their own compare).

## 5. Target layout

New and changed paths across the roadmap (one line each; a slice creates only the files it names):

```
.github/workflows/ci.yml                  # P1-01 GPU lane on Linux: apt deps, SDL_VIDEO_DRIVER=offscreen, ENGINE_REQUIRE_GPU=1
THIRD_PARTY_NOTICES.md                    # first dependency PR: upstream license texts
native/renderer/CMakeLists.txt            # P1-02 SDL3 FetchContent block (§4.2)
native/renderer/include/engine/renderer/
  swapchain.hpp                           # P1-04 Swapchain: create/recreate/acquire/present
  frame_context.hpp                       # P1-04 per-frame command buffer, fences, semaphores (2 frames in flight)
  screenshot.hpp                          # P1-04 copy swapchain or offscreen image to RGBA8 bytes + PNG
  forward_renderer.hpp                    # P1-06..P1-08 minimal forward pass (triangle, meshes, ECS draw)
  gpu_mesh.hpp                            # P1-07 vertex/index buffers uploaded from asset data
native/renderer/src/*.cpp                 # implementations of the headers above
native/renderer/shaders/tri.vert|frag     # P1-06 GLSL + *_spv.inc
native/renderer/shaders/mesh.vert|frag    # P1-07 GLSL + *_spv.inc
native/input/                             # P2-01 input module (OBJECT lib linked into engine_core like the others)
  include/engine/input/input.hpp          #   key/mouse/gamepad state, edges, actions
  src/input.cpp
native/scene/                             # P2-06 scene serializer (JSON v1)
native/editor_app/                        # P3 native editor executable (ImGui), engine-editor
native/player_app/                        # P2-07 engine-player executable
native/tests/golden/*.png                 # P1-05 reference images
native/tests/golden_compare.hpp           # P1-05 PNG compare helper used by GPU tests
native/tests/test_swapchain.cpp           # P1-04
native/tests/test_forward_triangle.cpp    # P1-06
native/tests/test_forward_mesh.cpp        # P1-07
native/tests/test_ecs_render.cpp          # P1-08
native/tests/test_input.cpp               # P2-01
native/tests/test_scene_io.cpp            # P2-06
native/tests/test_editor_shell.cpp        # P3-01
managed/Engine.Runtime/Input.cs           # P2-02
managed/Engine.Runtime/Time.cs            # P2-03
managed/Engine.Runtime/Rendering.cs       # P1-09 Camera/MeshRenderer/Light components
managed/Engine.Runtime/Collision.cs       # P2-05
managed/Engine.Runtime/Scene.cs           # P2-06
samples/HelloCube/                        # P1-09 first C# sample that draws
samples/MarbleRun/                        # P2-08 original playable sample game
docs/NOTES.md                             # dated discoveries (append, never rewrite history)
```

## 6. Milestones and slices

Tracking: roadmap issue #56. Each milestone is an epic issue with its slices as sub-issues:

| Milestone | Epic | Slices |
| --- | --- | --- |
| P0 | #57 | P0-01 #65, P0-02 #66, P0-03 #67, P0-04 #68 |
| P1 | #58 | P1-01 #69, P1-02 #70, P1-03 #71, P1-04 #72, P1-05 #73, P1-06 #74, P1-07 #75, P1-08 #76, P1-09 #77, P1-10 #78 |
| P2 | #59 | P2-01 #79, P2-02 #80, P2-03 #81, P2-04 #82, P2-05 #83, P2-06 #84, P2-07 #85, P2-08 #86 |
| P3 | #60 | P3-01 #87, P3-02 #88, P3-03 #89, P3-04 #90, P3-05 #91, P3-06 #92, P3-07 #93, P3-08 #94, P3-09 #95, P3-10 #96, P3-11 #97 |
| P4 | #61 | P4-00 #98, P4-01 #99, P4-02 #100, P4-03 #101, P4-04 #102, P4-05 #103, P4-06 #104, P4-07 #105, P4-08 #106, P4-09 #107, P4-10 #108, P4-11 #109, P4-12 #110 |
| P5 | #62 | P5-00 #111, P5-01 #112, P5-02 #113, P5-03 #114, P5-04 #115, P5-05 #116, P5-06 #117, P5-07 #118 |
| P6 | #63 | P6-00 #119, P6-01 #120, P6-02 #121, P6-03 #122, P6-04 #123, P6-05 #124, P6-06 #125, P6-07 #126, P6-08 #127, P6-09 #128 |
| P7 | #64 | P7-00 #129, P7-01 #130, P7-02 #131, P7-03 #132, P7-04 #133, P7-05 #134, P7-06 #135, P7-07 #136 |

An issue body may add acceptance detail to a slice; when the issue and this file differ, meet the stricter of the two and note the difference in `docs/NOTES.md`.

Every slice lists **Depends on**, **Do** and **Acceptance**. "GPU test" means a native test that requires a Vulkan device: it uses `device_hint = "llvmpipe"` where the existing tests do, and obeys rule 11 (FAIL, not SKIP, under `ENGINE_REQUIRE_GPU=1`).

### P0 Hygiene: latent bugs found by the 2026-10-31 assessment

#### P0-01 Fix function-pointer reflection in AbiLayoutTests
- **Depends on:** nothing.
- **Do:** In `managed/Engine.Tests/AbiLayoutTests.cs`, the reflection fallback loop checks `f.FieldType.IsPointer`. Change the check to `f.FieldType.IsFunctionPointer || f.FieldType.IsPointer` (§4.8). Keep the message text.
- **Acceptance:**
  - `cmake -S . -B /tmp/oot -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/oot && ctest --test-dir /tmp/oot -R "engine_compositor_live$" --output-on-failure` passes. Before the fix it fails with `AbiLayoutTests: 42 FAILED`.
  - The in-tree preset run (§3.2) still passes.

#### P0-02 Make the native build pass with GCC 13
- **Depends on:** nothing.
- **Do:** Fix every error in §4.9 without disabling warnings globally. Rules: rename the shadowing variable; use `static_cast<std::size_t>(...)` after asserting non-negative; replace the `memset` with value-initialization (`row = PullRow{};`); in `sdf_brick_builder.cpp` use `static_cast<float>(bx)` etc.; wrap the clang-only pragmas in `vma_impl.cpp` in `#if defined(__clang__)`; add `-fno-rtti`/`-fno-exceptions` only to C++ sources with `$<$<COMPILE_LANGUAGE:CXX>:...>` in the helper that adds them (find it with `grep -rn "fno-rtti" cmake native/CMakeLists.txt CMakeLists.txt`); for the meshoptimizer header warning mark its include dir `SYSTEM`. Then add a `gcc` matrix entry to the `native` CI job: `os: ubuntu-24.04`, a new preset `ci-linux-gcc` inheriting `ci-linux` with `CMAKE_C_COMPILER=gcc`, `CMAKE_CXX_COMPILER=g++`.
- **Acceptance:** `cmake --preset ci-linux-gcc && cmake --build --preset ci-linux-gcc` exits 0, and `ctest --preset ci-linux-gcc` has the same pass count as `ci-linux`. CI shows the new `native (ubuntu-24.04, gcc)` job green.

#### P0-03 Find and fix the `engine_audio_spatial` hang
- **Depends on:** nothing.
- **Do:** Reproduce with `ctest --preset ci-linux -j4 --repeat until-fail:30 -R "engine_audio_spatial|engine_physics|engine_gi_zero_bake"`. Attach `gdb -p <pid> -batch -ex "thread apply all bt"` output to `docs/NOTES.md` when it hangs. Fix the cause. Do not just lower the timeout or add `RUN_SERIAL` unless the backtrace proves contention on a process-wide resource (then explain it in the PR).
- **Acceptance:** `ctest --preset ci-linux -j4 --repeat until-fail:50 -R engine_audio_spatial` passes all 50 runs, and the test's own timeout is 60 s or less.

#### P0-04 Decide the `engine_export_publish_aot` local failure
- **Depends on:** nothing.
- **Do:** On a fresh clone with the Microsoft .NET 10 SDK (`actions/setup-dotnet` or `dotnet-install.sh`), run §3.2. If `engine_export_publish_aot` passes, write the finding in `docs/NOTES.md` (Ubuntu's source-built SDK lacks the ILCompiler restore path) and close as not-planned. If it fails, add `-r ${EXPORT_SAMPLE_RID}` restore before the `--no-build` publish in `native/tests/CMakeLists.txt` (the `add_test(NAME engine_export_publish_aot ...)` block) via a setup test that runs `dotnet restore -r <rid>`.
- **Acceptance:** `ctest --preset ci-linux -R engine_export --output-on-failure` passes on a fresh clone, or the NOTES entry explains why it is environment-only.

### P1 First pixels: the renderer draws and presents

Goal: an SDL3 window shows a lit mesh scene driven by ECS data, a C# sample does it, and CI proves it with golden images on lavapipe.

#### P1-01 CI GPU lane on Linux
- **Depends on:** nothing.
- **Do:**
  1. In `.github/workflows/ci.yml`, job `native`, step "Install Ninja + Clang (Linux)": install the full apt list from §3.1 (without dotnet).
  2. Add to the Linux test step `env: { SDL_VIDEO_DRIVER: offscreen, ENGINE_REQUIRE_GPU: "1" }`.
  3. In each existing GPU test (`test_renderer_bootstrap.cpp`, `test_gpu_indirect.cpp`, `test_slang_pbr.cpp`, `test_gi_zero_bake.cpp`), find every `[SKIP]` printed because no device or no Vulkan was found. Wrap it: when `std::getenv("ENGINE_REQUIRE_GPU")` is non-null and equals `"1"`, print `[FAIL]` with the same text and count it as a failure. Put the helper in `native/tests/gpu_required.hpp`:

```cpp
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string_view>
// Returns 1 (a failure to add to the test's count) when ENGINE_REQUIRE_GPU=1,
// else prints [SKIP] and returns 0.
inline auto gpu_missing(std::string_view what) -> int {
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
}
```

  Skips for optional tooling that CI does not have (`ENGINE_TEST_SLANGC unset`, `ENGINE_TEST_EXPORT_AOT`) stay `[SKIP]`.
- **Acceptance:**
  - CI log of `native (ubuntu-24.04)` contains `gpu: llvmpipe` and `renderer: Vulkan SDK headers found — real volk/VMA path`.
  - `grep -c "\[SKIP\].*no physical device"` over that log is `0`.
  - Locally, `ENGINE_REQUIRE_GPU=1 VK_ICD_FILENAMES=/nonexistent ./build/ci-linux/native/tests/engine_gi_zero_bake; echo $?` prints a `[FAIL]` line and a non-zero exit code.

#### P1-02 Fetch and link SDL3 3.4.18
- **Depends on:** P1-01.
- **Do:** Replace the SDL3 block in `native/renderer/CMakeLists.txt` with §4.2's block, adding `URL_HASH SHA256=<hash>` (§3.4). Add the `deps/README.md` row and `THIRD_PARTY_NOTICES.md` entry. Add `set_tests_properties(engine_renderer_bootstrap PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")` so the window path is exercised even when the developer forgets the variable.
- **Acceptance:**
  - Configure prints `renderer: SDL3 release-3.4.18 (static) — real window path` on all three CI OSes.
  - `ctest --preset ci-linux -R engine_renderer_bootstrap -V` prints `[PASS] window: valid + extent` and does **not** print `window: create fails only for missing video device`.
  - All three `native` CI jobs green.

#### P1-03 Enable the swapchain extension in VulkanDevice
- **Depends on:** P1-02.
- **Do:** Add `bool enable_swapchain{false};` to `DeviceCreateInfo`. When true: require `VK_KHR_swapchain` in `vkEnumerateDeviceExtensionProperties` for the chosen physical device (fail with `kDeviceCreateFailed` and message `"VK_KHR_swapchain missing"` if absent), and pass it in `dc.enabledExtensionCount`/`ppEnabledExtensionNames`. Add `auto supports_present(VkSurfaceKHR) const noexcept -> bool` that calls `vkGetPhysicalDeviceSurfaceSupportKHR(physical_, graphics_family_, surface, &ok)`.
- **Acceptance:** new section in `test_renderer_bootstrap.cpp`: create window (offscreen), pass `RendererWindow::instance_extensions` as `extra_instance_extensions`, `init` with `enable_swapchain = true`, `create_surface`, then `[PASS] device: swapchain ext enabled` and `[PASS] device: queue supports present`. GPU test rules apply.

#### P1-04 Swapchain, frame loop and screenshot
- **Depends on:** P1-03.
- **Do:** Port Appendix A into engine classes:

```cpp
namespace engine::renderer {
struct SwapchainDesc { std::uint32_t width; std::uint32_t height; bool vsync{true}; };
class ENGINE_CORE_API Swapchain {
 public:
  auto create(VulkanDevice& dev, VkSurfaceKHR surface, const SwapchainDesc& d) noexcept -> Status;
  auto recreate(std::uint32_t width, std::uint32_t height) noexcept -> Status;  // waits idle, rebuilds
  auto destroy() noexcept -> void;
  [[nodiscard]] auto format() const noexcept -> VkFormat;
  [[nodiscard]] auto extent() const noexcept -> VkExtent2D;
  [[nodiscard]] auto image_count() const noexcept -> std::uint32_t;
  [[nodiscard]] auto image(std::uint32_t i) const noexcept -> VkImage;
  [[nodiscard]] auto view(std::uint32_t i) const noexcept -> VkImageView;   // created once per image, not per frame
  [[nodiscard]] auto handle() const noexcept -> VkSwapchainKHR;
};
struct FrameBegin { bool ok; std::uint32_t image_index; VkCommandBuffer cmd; };
class ENGINE_CORE_API FrameContext {   // kFramesInFlight = 2
 public:
  static constexpr std::uint32_t kFramesInFlight = 2;
  auto create(VulkanDevice& dev, Swapchain& sc) noexcept -> Status;
  auto destroy() noexcept -> void;
  auto begin_frame() noexcept -> FrameBegin;      // wait fence, acquire, reset+begin cmd
  auto end_frame(VkImageLayout current_layout) noexcept -> Status;  // barrier to PRESENT_SRC, submit, present
};
auto capture_rgba8(VulkanDevice& dev, VkImage image, VkFormat fmt, VkExtent2D ext,
                   VkImageLayout current_layout, std::vector<std::uint8_t>& out) noexcept -> Status; // one-shot copy + wait
auto write_png(const char* path, std::span<const std::uint8_t> rgba, std::uint32_t w, std::uint32_t h) noexcept -> Status;
}
```

  Rules from §4.3: extent fallback for `0xFFFFFFFF`, image count `max(minImageCount, 2)`, prefer `B8G8R8A8_UNORM` then `R8G8B8A8_UNORM`, FIFO, usage `COLOR_ATTACHMENT | TRANSFER_SRC`. On `VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR` from acquire or present, call `recreate` with the window's current size. `capture_rgba8` swizzles BGRA to RGBA so PNGs are always RGBA. Put `#define STB_IMAGE_WRITE_IMPLEMENTATION` in exactly one engine `.cpp`.
- **Acceptance:** new GPU test `engine_swapchain` (`native/tests/test_swapchain.cpp`, environment `SDL_VIDEO_DRIVER=offscreen`): opens a 320x240 window, runs 3 frames that clear to `(1.0, 0.5, 0.0, 1.0)`, then prints `[PASS] swapchain: 3 frames presented`, `[PASS] swapchain: pixel(0,0) == 255,128,0,255` and `[PASS] swapchain: recreate 640x360`. Also run it once on a desktop with a real GPU and no `SDL_VIDEO_DRIVER` set, and paste the output in the PR.

#### P1-05 Golden-image harness
- **Depends on:** P1-04.
- **Do:** `native/tests/golden_compare.hpp` with `auto golden_check(std::string_view name, std::span<const std::uint8_t> rgba, std::uint32_t w, std::uint32_t h) -> int` (returns failures). It loads `native/tests/golden/<name>.png` with `stb_image` (path from compile definition `ENGINE_GOLDEN_DIR`), and passes when every channel differs by at most 2 and at most 0.1% of pixels differ at all. On failure it writes `<build>/golden_out/<name>.actual.png` and `<name>.diff.png` (red where different) and prints both paths. When `ENGINE_UPDATE_GOLDENS=1`, it overwrites the golden and prints `[PASS] golden: updated <name>`. Add the orange clear from P1-04 as golden `clear_orange_320x240.png`.
- **Acceptance:** `engine_swapchain` prints `[PASS] golden: clear_orange_320x240`. Changing the clear color to `(0,0,1,1)` locally makes it print `[FAIL] golden: clear_orange_320x240` plus the two output paths (revert after checking; paste the output in the PR).

#### P1-06 First triangle
- **Depends on:** P1-05.
- **Do:** `ForwardRenderer` with `create(VulkanDevice&, VkFormat color, VkFormat depth)` and `record_triangle(VkCommandBuffer, VkImageView color, VkExtent2D)`. GLSL `tri.vert` (positions and colors from constant arrays indexed by `gl_VertexIndex`, as in §4.7) and `tri.frag`, both embedded as `_spv.inc`. Pipeline: dynamic rendering (`VkPipelineRenderingCreateInfo`), no vertex input, dynamic viewport and scissor, no culling.
- **Acceptance:** GPU test `engine_forward_triangle` renders 320x240 and prints `[PASS] golden: triangle_320x240`. The golden shows a red/green/blue triangle on black; attach it to the PR.

#### P1-07 Mesh, camera, depth and one light
- **Depends on:** P1-06.
- **Do:** `GpuMesh` uploads interleaved vertices (`float pos[3]; float normal[3]; float uv[2];`, 32 bytes) and `uint32_t` indices through VMA. Add a depth buffer (`VK_FORMAT_D32_SFLOAT`). `mesh.vert/frag`: push constant `{ mat4 mvp; mat4 model; vec4 light_dir; vec4 base_color; }` (160 bytes, within the 256-byte minimum), Lambert lighting `max(dot(n, -light_dir), 0) * 0.9 + 0.1`, sRGB encode in the fragment shader. Build a unit cube procedurally in the test (24 vertices, 36 indices). Camera at `(2, 2, 3)` looking at origin, 60° vertical FOV, near 0.1, far 100, right-handed, Vulkan clip space (Y flipped, depth 0..1).
- **Acceptance:** `engine_forward_mesh` prints `[PASS] golden: cube_lit_320x240` and `[PASS] mesh: depth test hides back faces` (render the cube twice with the second offset behind the first; the golden shows only the front one).

#### P1-08 Draw ECS entities
- **Depends on:** P1-07.
- **Do:** Native Flecs components registered in `native/ecs`: `Camera { float fov_y_rad; float near_z; float far_z; std::uint32_t is_main; }`, `MeshRenderer { std::uint32_t mesh_id; float base_color[4]; }`, `DirectionalLight { float direction[3]; float intensity; }`. Register these three through `GenericSystems::register_component(world, "Engine.Runtime.Camera", sizeof(Camera), alignof(Camera))` (and `Engine.Runtime.MeshRenderer`, `Engine.Runtime.DirectionalLight`), never with `ECS_COMPONENT` or `ecs_component_init` directly. Reason: C# `Ecs.ComponentId<T>()` registers by `typeof(T).FullName` with separator `""`; `register_component` is idempotent for the same name, size and alignment, so P1-09's C# structs then bind to the native components. Any other name or path, or a different size, makes Flecs abort or create a second component (gotcha 20). A `MeshRegistry` maps `mesh_id` to `GpuMesh`. `ForwardRenderer::render_world(ecs_world_t*, VkCommandBuffer, ...)` queries `(WorldTransform, MeshRenderer)` and draws each, using the first `Camera` with `is_main = 1` and its `WorldTransform`. Upload model matrices into the existing upload ring rather than one push constant per draw when count > 1 (an SSBO indexed by `gl_InstanceIndex` is fine).
- **Acceptance:** `engine_ecs_render` spawns a 10x10x10 grid of cubes (1000 entities) through Flecs, renders, prints `[PASS] golden: grid_1000_640x360` and `[PASS] ecs-render: 1000 draws`. Moving one entity's `LocalTransform` and calling `world_propagate` changes the next frame's image (`[PASS] ecs-render: transform change visible`).

#### P1-09 Rendering from C#
- **Depends on:** P1-08.
- **Do:** ABI entries (append, minor bump): `renderer_create(EngineWorld*, uint32_t width, uint32_t height, uint32_t flags) -> EngineRenderer*` (flag bit 0 = hidden window), `renderer_destroy`, `renderer_frame(EngineRenderer*) -> int32_t` (pumps SDL events, renders the world, presents; returns 1 when the window was closed), `renderer_mesh_create(EngineRenderer*, const float* vertices, uint32_t vertex_count, const uint32_t* indices, uint32_t index_count) -> uint32_t mesh_id`, `renderer_screenshot(EngineRenderer*, const char* png_path) -> int32_t`. C# wrappers in `managed/Engine.Runtime/Rendering.cs`: `Renderer` (IDisposable), and `IComponent` structs `Camera`, `MeshRenderer`, `DirectionalLight` in namespace `Engine.Runtime` (so their full names match P1-08's registration) and matching the native layouts exactly (add size asserts to `AbiLayoutTests`, and a test that `Ecs.ComponentId<Camera>()` returns the id the native side registered). New sample `samples/HelloCube` (copy `samples/IllusionSample` structure): one camera, one light, one rotating cube via a generated system.
- **Acceptance:** CTest `engine_hello_cube` runs the sample with `SDL_VIDEO_DRIVER=offscreen` and arguments `--frames 10 --screenshot <build>/hello_cube.png`, then a compare step passes against golden `hello_cube_10f_640x360.png`. `engine_abi_layout` and `AbiLayoutTests` pass. `abi_codegen` prints `abi_codegen: 49 entries`.

#### P1-10 Real `snapshot capture`
- **Depends on:** P1-09.
- **Do:** `CliDispatcher` snapshot: when the editor session has a live `Renderer`, call `renderer_screenshot` and return width, height and `HeadlessStub: false`. Keep the stub only when no renderer exists, unchanged.
- **Acceptance:** a `CliApiTests` case that starts the editor server with a renderer (offscreen) and runs `engine-cli snapshot capture --out <tmp>/s.png --pass final` gets `HeadlessStub=false`, a PNG that exists, and width/height equal to the window size.

### P2 Playable loop: input, time, physics from C#, scenes, a player and a sample game

#### P2-01 Native input module
- **Depends on:** P1-09.
- **Do:** `native/input` OBJECT library. `class InputState` with `process_event(const SDL_Event&)`, `begin_frame()` (clears per-frame edges), `key_down(Scancode)`, `key_pressed(Scancode)` (went down this frame), `key_released(Scancode)`, `mouse_position()`, `mouse_delta()`, `mouse_button(int)`, `gamepad_axis(int pad, Axis)`, `gamepad_button(int pad, Button)`. State is a `std::array<std::uint8_t, SDL_SCANCODE_COUNT>` updated only from events (§4.6). Gamepads opened on `SDL_EVENT_GAMEPAD_ADDED` with `SDL_OpenGamepad`. `renderer_frame` feeds every polled event to the input state before rendering.
- **Acceptance:** `engine_input` test injects key down W, polls, asserts `key_pressed(W)` and `key_down(W)`, next frame asserts `!key_pressed(W) && key_down(W)`, injects key up and asserts `key_released(W)`; injects mouse motion `(10,20)` rel `(3,0)` and asserts position and delta; injects `SDL_EVENT_GAMEPAD_AXIS_MOTION` for a fake joystick id and asserts the axis value normalised to `[-1, 1]`. All `[PASS]`.

#### P2-02 C# Input API and action maps
- **Depends on:** P2-01.
- **Do:** ABI: `input_get_state(EngineRenderer*) -> const EngineInputState*` where `EngineInputState` is a blittable struct with `uint8_t keys[512]; uint8_t pressed[512]; uint8_t released[512]; float mouse_x, mouse_y, mouse_dx, mouse_dy; uint32_t mouse_buttons; float pad_axes[4][6]; uint32_t pad_buttons[4];` (read zero-copy from C#). C# `static class Input` with `GetKey(Key)`, `GetKeyDown(Key)`, `GetKeyUp(Key)`, `MousePosition`, `MouseDelta`, `GetAxis(string name)`; `Key` enum values equal SDL scancodes. Action maps from `input_actions.json` (`{"axes":{"Horizontal":{"negative":["A","Left"],"positive":["D","Right"],"gamepadAxis":"LeftX"}}}`), loaded once at startup.
- **Acceptance:** managed test `InputApiTests` feeds a fake `EngineInputState` and checks `GetAxis("Horizontal")` is `1` with D held, `-1` with A held, `0` with both, and equals the gamepad axis when no key is held. `AbiLayoutTests` asserts `sizeof(EngineInputState)` matches native.

#### P2-03 Time and fixed timestep
- **Depends on:** P1-09.
- **Do:** A frame loop helper in native: variable `dt` clamped to 0.1 s, fixed step 1/50 s with an accumulator, at most 5 fixed steps per frame, `time_scale`. C# `static class Time` exposes `DeltaTime`, `FixedDeltaTime`, `TimeScale`, `FrameCount`, `RealtimeSinceStartup`. Systems marked with the existing `Phase` attribute run in the fixed step when the phase is `Phase.FixedUpdate` (add the enum member if absent).
- **Acceptance:** native test drives the loop with fake clock steps `[0.016, 0.016, 0.2, 0.001]` and asserts fixed steps `[0, 1, 5, 0]` (accumulator math written out in the test comment) and that clamping happened once.

#### P2-04 Physics bodies from C#
- **Depends on:** P2-03.
- **Do:** Components `RigidBody { float mass; uint32_t motion_type; /*0 static,1 kinematic,2 dynamic*/ float linear_damping; float angular_damping; }` and `BoxCollider { float half_extents[3]; }`, `SphereCollider { float radius; }`, `CapsuleCollider { float radius; float half_height; }`. A system creates Jolt bodies for new entities with `RigidBody` + one collider, destroys them on removal, and the existing transform sync pushes results back. ABI entry `physics_world_for(EngineWorld*) -> EnginePhysicsWorld*` so the existing raycast entries become usable from C#.
- **Acceptance:** native test drops a dynamic sphere (radius 0.5) from y=5 onto a static box floor (top at y=0); after 2 simulated seconds at 50 Hz its y is within `0.5 ± 0.05`. Managed test does the same through C# components and `Physics.Raycast` down from y=10 hits the floor at y=0.

#### P2-05 Collision events reach C#
- **Depends on:** P2-04.
- **Do:** ABI `physics_drain_events(EnginePhysicsWorld*, EngineCollisionEvent* out, uint32_t capacity) -> uint32_t count`, `EngineCollisionEvent` mirrors `engine::physics::CollisionEvent` exactly (static_assert size and offsets). C# `Physics.CollisionEvents` returns `ReadOnlySpan<CollisionEvent>` over a native-owned buffer filled once per fixed step (no managed allocation). Add `ICollisionHandler` with `OnCollisionEnter(Entity self, Entity other, Vector3 point)` and `OnCollisionExit` dispatched by the runtime.
- **Acceptance:** managed test: sphere falls on floor; exactly one `kEnter` for the pair within 2 s; moving the sphere up 3 units produces exactly one `kExit`. A sensor box reports `sensor = 1`. `GC.GetAllocatedBytesForCurrentThread()` delta across 100 drained frames is `0`.

#### P2-06 Scene files (JSON v1)
- **Depends on:** P2-04.
- **Do:** `native/scene` with `save_scene(ecs_world_t*, const char* path)` and `load_scene(ecs_world_t*, const char* path)`. Format, exactly:

```json
{
  "format": "engine-scene",
  "version": 1,
  "entities": [
    {
      "id": 1,
      "name": "Floor",
      "parent": 0,
      "components": {
        "LocalTransform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [10, 0.2, 10]},
        "MeshRenderer": {"mesh": "assets/cube.glb#0", "base_color": [0.8, 0.8, 0.8, 1]},
        "Mover": {"$bytes": "0000803f00000000", "$size": 8, "$schema": "a1b2c3d4"}
      }
    }
  ]
}
```

  Built-in components get named fields. User components registered through `component_register` are written as hex bytes plus size plus a schema hash (FNV-1a 32 of `name:size:align`); on load, a mismatched size or hash is skipped with a warning printed, never a crash. `id` is a file-local id; `parent` refers to it (0 = root). Meshes are referenced by asset path plus index, never by runtime `mesh_id`. Use a small JSON writer/reader written in the module (no new dependency); numbers printed with `std::to_chars` shortest round-trip.
- **Acceptance:** `engine_scene_io` builds a 3-level hierarchy of 10 entities with built-in and user components, saves, loads into a fresh world, saves again, and asserts the two files are byte-identical (`[PASS] scene: round-trip stable`); a file with a wrong `$size` loads the rest and prints one warning (`[PASS] scene: bad user component skipped`). ABI `scene_save`/`scene_load` and C# `Scene.Save/Load` with a managed round-trip test.

#### P2-07 `engine-player` executable
- **Depends on:** P2-02, P2-03, P2-06.
- **Do:** `native/player_app/main.cpp` builds `engine-player`. Usage: `engine-player <project_dir> [--scene <relative path>] [--frames N] [--screenshot out.png] [--hidden]`. A project dir contains `project.json` (`{"name":"...","startScene":"scenes/main.scene.json","gameAssembly":"bin/Game.dll"}`). It creates the world, loads the game assembly through the existing `dotnet_host`, loads the start scene, and runs the loop until the window closes or N frames pass.
- **Acceptance:** CTest `engine_player_hello` runs `engine-player samples/HelloCube --frames 10 --screenshot <build>/player.png --hidden` with `SDL_VIDEO_DRIVER=offscreen`; exit code 0; golden compare passes.

#### P2-08 Sample game "Marble Run"
- **Depends on:** P2-05, P2-07.
- **Do:** An original small game in `samples/MarbleRun`: the player rolls a sphere (WASD or left stick applies torque) along a ramp course built from boxes to a goal trigger; falling below y = -10 respawns at start; reaching the goal shows "Finished in N s" (console log until UI exists in P5). Add a `--autoplay inputs.json` option to the player that replays recorded key states per frame, for testing.
- **Acceptance:** CTest `engine_marble_autoplay` replays `samples/MarbleRun/test/autoplay.json` and the log contains `Finished in` within 1500 frames; exit code 0.

### P3 Editor shell (Dear ImGui docking, native executable)

The editor is a native executable, `engine-editor`, drawing with Dear ImGui into the engine's own SDL3 window. It talks to the world directly through the native editor bridge (`native/ecs/src/editor_bridge.cpp` and the `editor_*` ABI entries), and keeps the C# `Engine.Editor` library as the automation and CLI surface.

#### P3-01 Editor window with dockspace
- **Depends on:** P1-04.
- **Do:** Fetch ImGui `v1.93.0-docking` (§3.4) into a static library `engine_imgui` with the defines and loader from §4.5. `native/editor_app/main.cpp` builds `engine-editor [--project dir] [--frames N] [--screenshot out.png]`. Main menu bar (File, Edit, GameObject, Window, Help, items stubbed), a full-window dockspace, and empty dock windows "Hierarchy", "Inspector", "Scene", "Game", "Project", "Console" in a default layout built with `ImGui::DockBuilder*` on first run. `io.IniFilename` = `<project>/.editor/imgui.ini`, `nullptr` when `--frames` is given.
- **Acceptance:** GPU test `engine_editor_shell` runs `engine-editor --frames 3 --screenshot <build>/editor.png` (offscreen) and the golden `editor_default_layout_1280x720.png` passes.

#### P3-02 Scene viewport and editor camera
- **Depends on:** P3-01, P1-08.
- **Do:** Render the world into an offscreen color+depth target sized to the "Scene" dock window, register it with `ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)`, show it with `ImGui::Image`. Recreate on resize (free with `ImGui_ImplVulkan_RemoveTexture`). Editor camera: right mouse + WASD/QE fly, scroll to dolly, F to frame the selection; only while the Scene window is hovered.
- **Acceptance:** golden `editor_scene_grid_1280x720.png` with the P1-08 grid loaded; injected right-mouse + W events for 10 frames move the camera forward (`[PASS] editor: fly moves camera`).

#### P3-03 Hierarchy panel
- **Depends on:** P3-02.
- **Do:** Tree of entities from `editor_count_roots`/`editor_get_root`/`editor_count_children`/`editor_get_child`, names from `editor_set_name` data, selection, double-click rename, drag-drop reparent (`editor_set_parent`), context menu Create Empty / Delete / Duplicate. Every mutation goes through an undo stack built on `editor_snapshot_take/restore` (one snapshot per action, capped at 100).
- **Acceptance:** native test drives the panel with ImGui's test-free approach: call the panel's action functions directly (`hierarchy_reparent(a, b)`, `hierarchy_delete(e)`) then `undo()`, and assert world state via the editor bridge after each; plus a golden of the panel with 3 nested entities.

#### P3-04 Component field metadata
- **Depends on:** P2-06.
- **Do:** ABI `component_describe(EngineWorld*, const char* name, const EngineFieldDesc* fields, uint32_t count)` where `EngineFieldDesc { const char* name; uint32_t offset; uint32_t type; /* 0 f32, 1 i32, 2 u32, 3 bool8, 4 vec2, 5 vec3, 6 vec4, 7 quat, 8 color4, 9 entity, 10 asset-ref */ }`. The Roslyn generator emits a `component_describe` call next to its existing `component_register` call for every `IComponent` field it understands, and nothing for others. Built-in components describe themselves natively. Scene files (P2-06) switch user components from `$bytes` to named fields when a description exists (version stays 1; `$bytes` still loads).
- **Acceptance:** `GeneratorDriverTests` case: a component `struct Health : IComponent { public float Current; public int Max; }` produces descriptions `Current f32 @0`, `Max i32 @4`. Scene round-trip test writes `"Health": {"Current": 75, "Max": 100}`.

#### P3-05 Inspector panel
- **Depends on:** P3-03, P3-04.
- **Do:** For the selected entity, list components with field editors by type (`DragFloat`, `DragInt`, `Checkbox`, `DragFloat3`, quaternion shown as Euler degrees, `ColorEdit4`, entity picker, asset field). Add Component button lists registered components. Remove Component via context menu. Each edit commits one undo step when the widget is deactivated (`ImGui::IsItemDeactivatedAfterEdit`).
- **Acceptance:** native test sets a float through the panel's edit function, asserts the component value, undoes, asserts the old value; golden of the inspector with `LocalTransform` and `MeshRenderer`.

#### P3-06 Gizmos and picking
- **Depends on:** P3-05.
- **Do:** Object-ID picking: render entity ids into an `R32_UINT` target in the scene pass, read back one pixel on click. Translate/rotate/scale gizmo with W/E/R hotkeys and local/world toggle; port the math from `managed/Engine.Editor/ViewportGizmo.cs` (its tests define the expected behaviour) to C++ and draw handles with `ImGui::GetWindowDrawList()`. Gizmo drags are one undo step.
- **Acceptance:** native tests: click at the projected center of a known cube selects it (`[PASS] pick: center selects entity`), click on background clears selection; translate drag along X by a scripted mouse path moves `LocalTransform.position.x` by the expected amount ± 0.01.

#### P3-07 Play mode
- **Depends on:** P3-02, P2-03.
- **Do:** Toolbar Play / Pause / Step / Stop. Play takes an `editor_snapshot_take`, runs game systems and physics in the "Game" view through the main camera; Stop restores the snapshot. Edits during play are discarded on Stop, and the toolbar is tinted while playing.
- **Acceptance:** native test: play 60 frames with a falling sphere, stop, assert the sphere's position equals the pre-play value exactly.

#### P3-08 Project browser and drag-in assets
- **Depends on:** P3-02.
- **Do:** "Project" panel lists the project's `assets/` tree (folders, `.glb`, `.png`, `.scene.json`, `.cs`), using the existing asset GUID database (`native/assets`). Dragging a `.glb` into the Scene view imports it (existing pipeline) and spawns an entity with `MeshRenderer` at the ground point under the cursor. Double-clicking a scene opens it (prompting to save the current one).
- **Acceptance:** native test calls `project_drop_asset("assets/cube.glb", screen_pos)` and asserts one new entity with a valid `mesh_id` within one frame (the M5 gate: visible in 1 frame).

#### P3-09 Console and profiler panels
- **Depends on:** P3-01.
- **Do:** A log sink capturing native `std::printf`-style engine logs and C# `Debug.Log/Warning/Error` (new ABI `log_write(int level, const char* msg)`), with level filters, search, clear, and collapse duplicates. Profiler: CPU frame time graph (last 300 frames), per-system timings from Flecs system stats, GPU time from timestamp queries around the scene pass.
- **Acceptance:** C# `Debug.Log("hello")` appears in the console model (`console_count() == 1`, text matches); profiler shows a non-zero GPU time on lavapipe after 10 frames.

#### P3-10 Projects, saving and building from the editor
- **Depends on:** P3-08, P2-07.
- **Do:** File menu: New Project (creates `project.json`, `assets/`, `scenes/main.scene.json`, a `Game.csproj` referencing `Engine.Runtime`), Open Project, Save Scene (Ctrl+S), Save As. Build menu runs `dotnet build` on the game project and shows errors in the console; Export runs the existing export pipeline (`engine-cli`/`ExportPipeline`) as a subprocess with arguments passed as an argv array, never a shell string.
- **Acceptance:** native test creates a project in a temp dir via the same functions the menu calls, saves a scene, reopens it and compares entity counts; a build with a deliberate C# syntax error yields at least one console entry with level Error and the file name.

#### P3-11 Script workflow and hot reload in the editor
- **Depends on:** P3-10.
- **Do:** Watch `assets/**/*.cs`; on save, rebuild the game assembly and reload it with the existing collectible-ALC hot reload; compile errors go to the console and the old assembly keeps running. "Open C# Project" launches the OS default handler for the `.csproj` (`SDL_OpenURL`).
- **Acceptance:** test edits a system's constant in a temp project, waits for reload, and observes the new behaviour within 2 s of the file write; a syntax error leaves the old behaviour running and logs an Error.

### P4 Visual fidelity

Each slice adds a golden on lavapipe and a manual check on a real GPU pasted in the PR.

- **P4-00 Spike for P4.** Verify, by running, the facts P4 needs: how to drive the existing `PbrPipeline`, `GiFrameGraph`, `GpuCull`, `SceneSsbo` from a real frame; whether `slangc` can be obtained in CI (GitHub release asset of shader-slang) and its exact version; timestamp query support on lavapipe. Add the findings to §4 of this file in a PR before any P4 slice starts.
- **P4-01 PBR materials.** glTF metallic-roughness materials (base color, normal, metallic-roughness, occlusion, emissive textures, BC7 from the existing pipeline) through the bindless table; `Material` asset and component. Acceptance: golden of a sphere grid varying metallic (x) and roughness (y).
- **P4-02 HDR and tonemapping.** Render to `R16G16B16A16_SFLOAT`, then the existing `post_composite` (ACES + sRGB) to the swapchain; exposure on the camera. Acceptance: golden; an emissive value of 10 does not clip to white at exposure −3.
- **P4-03 Cascaded shadow maps.** 4 cascades for the main directional light, PCF 3x3, stable cascades (texel snapping), per-light shadow toggle. Acceptance: golden of a pillar casting a shadow; a moving camera does not shimmer (two goldens 1 cm apart differ in < 0.5% of pixels in the shadow region).
- **P4-04 Point and spot lights.** Clustered forward (16x9x24 clusters), up to 1024 lights, range attenuation, spot cone. Acceptance: golden with 64 colored point lights; CPU light-binning time printed.
- **P4-05 Skybox and image-based lighting.** Equirect HDR import, cubemap conversion, irradiance and prefiltered specular maps, BRDF LUT, all generated at import with compute. Acceptance: golden of chrome and rough spheres under an HDR sky.
- **P4-06 GPU-driven path.** Wire `SceneSsbo` + `GpuCull` + `vkCmdDrawIndexedIndirectCount` into the frame for static meshes. Acceptance: golden equals the P4-01 golden; 250k instances measured on a real GPU, numbers pasted (the M3 gate).
- **P4-07 Real-time GI.** Wire the existing SDF bricks and `GiFrameGraph` into the frame. Acceptance: golden of a Cornell-box style room where the red wall tints the floor.
- **P4-08 Bloom.** Dual-filter downsample/upsample chain, threshold and intensity on a `PostProcess` component. Acceptance: golden.
- **P4-09 SSAO.** GTAO half resolution with bilateral blur. Acceptance: golden; toggle off restores the P4-05 golden.
- **P4-10 Anti-aliasing.** TAA with jittered projection, history rejection, and FXAA fallback. Acceptance: golden after 16 frames on a static scene; edge pixel variance below threshold.
- **P4-11 Fog and post stack.** Height fog, color grading LUT, vignette; one `PostProcess` component with ordered effects. Acceptance: goldens.
- **P4-12 LOD.** `meshopt_simplify` LOD chain at import (4 levels), screen-size selection, crossfade optional. Acceptance: test asserts LOD index by distance; golden at 3 distances.

### P5 Content systems

- **P5-00 Spike for P5.** Verify RmlUi 6.3 + FreeType 2.14.3 build and a Vulkan render interface (RmlUi ships backends; find which), cgltf skin/animation APIs on a sample `.glb`, and miniaudio group/effect node wiring. Write facts into §4 in a PR first.
- **P5-01 Skeletal mesh import.** Skins, joints, inverse bind matrices, and animation clips from glTF into the `.pak`. Acceptance: importer test on a generated two-bone `.glb` checks joint count, hierarchy and keyframe values.
- **P5-02 Animation playback and skinning.** Clip sampling (linear, step, cubic spline), blending of two clips, GPU skinning in `mesh.vert` (4 weights). Acceptance: golden of a bent arm at t = 0.5 s; sampling unit tests.
- **P5-03 Animator.** State machine asset (JSON): states, transitions with conditions on float/bool/trigger parameters, cross-fade; C# `Animator.SetFloat/SetBool/SetTrigger`. Acceptance: managed test drives parameters and asserts the active state sequence.
- **P5-04 Game UI.** RmlUi with FreeType text, rendering through the engine; C# `UIDocument.Load("ui/hud.rml")`, element lookup, text set, click events. Acceptance: golden of a HUD with a label and button; injected click fires the C# handler once.
- **P5-05 Particles.** GPU compute emitter/update with billboarded rendering, sorted alpha and additive modes; `ParticleSystem` component (rate, burst, lifetime, start speed/size/color, gravity, color and size over life). Acceptance: deterministic seed golden after 60 frames; particle count after 1 s at rate 100 is 100 ± 1.
- **P5-06 Audio mixer.** `AudioSource`/`AudioListener` components, buses (Master, Music, SFX) via `ma_sound_group`, low-pass and delay effects, volume in dB. Acceptance: offline (noDevice) render of a sine through a bus at −6 dB has peak amplitude 0.5 ± 0.01.
- **P5-07 Sequencer (timeline).** Tracks for transform, animation clip, audio and activation, scrubbed in the editor. Acceptance: evaluating at t gives expected component values.

### P6 World building, 2D and prefabs

- **P6-00 Spike for P6.** Verify Recast/Detour v1.6.0 and Box2D v3.2.0 build into `engine_core`, and the Jolt `HeightFieldShape` and `CharacterVirtual` call sequences, by running small programs. Facts into §4 first.
- **P6-01 Terrain.** Heightmap asset (R16 PNG), chunked mesh with LOD, 4-layer splat material, Jolt height-field collider. Acceptance: raycast hits the terrain at the sampled height ± 0.01; golden.
- **P6-02 Terrain tools.** Raise/lower/smooth/flatten brushes and splat painting in the editor, with undo. Acceptance: brush function test changes heights in radius only.
- **P6-03 Navigation.** Bake a navmesh from static colliders with Recast; `NavAgent` component pathing with Detour crowd. Acceptance: agent reaches a goal around an obstacle in a test scene within N frames.
- **P6-04 Character controller.** Jolt `CharacterVirtual`; C# `CharacterController.Move(Vector3)`, grounded flag, slope limit, step height. Acceptance: controller climbs a 0.3 m step, stops at a 60° slope.
- **P6-05 Joints.** Fixed, hinge, slider, distance and spring joints as components over Jolt constraints. Acceptance: hinged door swings and stops at its limit.
- **P6-06 Sprites and 2D camera.** Batched sprite renderer, atlases, sorting layers and order, orthographic camera, pixel-perfect option. Acceptance: golden of 10k sprites; one draw call per atlas.
- **P6-07 2D physics.** Box2D v3 world, `Rigidbody2D`, box/circle/polygon colliders, 2D collision events into C#. Acceptance: box stack settles; events counted.
- **P6-08 Tilemaps.** Grid tilemap component, tile palette in the editor, tilemap collider. Acceptance: golden; collider count equals merged regions.
- **P6-09 Prefabs.** Prefab asset (scene subset), instantiate, per-field overrides, apply/revert, nested prefabs, built on the existing `editor_is_prefab_instance`. Acceptance: changing the prefab asset updates instances except overridden fields.

### P7 Platforms, networking and ecosystem

- **P7-00 Spike for P7.** Verify MoltenVK presentation on `macos-15` and a real window on `windows-2025` (with the LunarG Vulkan SDK in CI), ENet v1.3.18 build, and Android NDK + Vulkan feasibility. Facts into §4 first.
- **P7-01 Windows GPU lane.** Vulkan SDK in the Windows CI job, SwiftShader or WARP-equivalent for headless tests, all GPU tests running.
- **P7-02 macOS presentation.** MoltenVK with `VK_KHR_portability_enumeration`, swapchain on a Metal layer, CI build and headless tests.
- **P7-03 Player builds from the editor.** NativeAOT player plus `.pak` for Windows, Linux and macOS from the editor's Build window; cold boot < 1 s measured.
- **P7-04 Networking.** ENet transport, server/client sessions, entity snapshot replication with delta compression for components marked `[Replicated]`, reliable RPCs. Acceptance: two in-process peers converge on 100 replicated transforms within 10 ticks at 10% simulated loss.
- **P7-05 Packages.** `project.json` `packages` with git URL + tag, resolved into `Packages/`, version lock file. Acceptance: resolving twice is a no-op; lock file pins commit SHAs.
- **P7-06 Documentation and tutorials.** A docs site in `docs/manual/` (getting started, editor tour, scripting, rendering, physics) plus three step-by-step tutorials built on the samples. Acceptance: every code snippet in the manual is extracted and compiled in CI.
- **P7-07 Android.** Vulkan Android player build, touch input mapped into `Input`. Spike decides go/no-go.

Out of scope for this roadmap: consoles (platform SDKs are under NDA), WebGL/WebGPU export, a visual shader graph and visual scripting. Each could become its own roadmap later.

## 7. Gotchas

Re-read before every slice.

1. Set `SDL_VIDEO_DRIVER=offscreen` for every test that opens a window, or the window test silently "passes" by failing to open.
2. Set `ENGINE_REQUIRE_GPU=1` in CI; without it GPU tests skip and pass.
3. Headless swapchain `currentExtent` is `0xFFFFFFFF`; use the window size.
4. Headless swapchain always returns image index 0.
5. lavapipe offers no `_SRGB` swapchain format; encode sRGB in the last shader.
6. `minImageCount` on lavapipe is 4; never hard-code the count.
7. `SDL_GetKeyboardState` ignores pushed events; keep engine key state from events.
8. `SDL_PollEvent` returns SDL's own window events too; filter by type in tests.
9. `SDL_Init` returns `bool` in SDL3.
10. SDL3 configure fails on Linux if any X11 extension dev package is missing; install the full §3.1 list.
11. volk lives in `libengine_core`; executables resolve `vk*` through `VulkanDevice::proc`/`instance_proc` and never compile `volk.c`.
12. ImGui 1.93: `RenderPass`/`PipelineRenderingCreateInfo` are under `InitInfo.PipelineInfoMain`.
13. ImGui with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` needs `ImGui_ImplVulkan_LoadFunctions` before `ImGui_ImplVulkan_Init`, or it crashes on the first Vulkan call.
14. The `VkFormat` array given to `PipelineRenderingCreateInfo.pColorAttachmentFormats` must outlive `ImGui_ImplVulkan_Init`.
15. Set `io.IniFilename = nullptr` in tests, or a stray `imgui.ini` changes the layout and breaks goldens.
16. `VulkanDevice::init` enables no device extensions; swapchain needs `enable_swapchain = true` (P1-03).
17. Append ABI entries at the end of `EngineApiTable`; bump minor; regenerate; never hand-edit `NativeApi.g.cs`.
18. `.NET` function-pointer fields report `IsPointer == false`; use `IsFunctionPointer`.
19. Use the `ci-linux` preset build dir; out-of-tree dirs break `AbiLayoutTests` until P0-01 lands.
20. Flecs aborts the process when a component name is re-registered with a different size (from the Illusion Layer work). Native components that C# also sees are registered through `GenericSystems::register_component` under the C# full name (P1-08).
21. No `strlen`, `snprintf`, `memset` or C arrays in new C++ (SonarCloud gate fails the PR).
22. Pass subprocess arguments as an argv array; never build a shell command string from file names or user input.
23. Golden PNGs are RGBA; swizzle BGRA swapchain data before writing or comparing.

## 8. Definition of done

For each slice:

- [ ] Every Acceptance command in the slice passes locally and is pasted with output in the PR body.
- [ ] All CI jobs are green, including SonarCloud and CodeRabbit gates from `AGENTS.md`.
- [ ] New GPU tests obey `ENGINE_REQUIRE_GPU`, and new window tests set `SDL_VIDEO_DRIVER=offscreen`.
- [ ] ABI changes appended, minor bumped, bindings regenerated, layout tests updated.
- [ ] New dependencies pinned with `URL_HASH`, listed in `deps/README.md` and `THIRD_PARTY_NOTICES.md`.
- [ ] `docs/NOTES.md` has dated entries for every discovery or deviation.
- [ ] The issue is closed by the PR (`Closes #N`).

For the roadmap: every P0 to P7 issue closed, and a fresh assessment against Unity scores no area "Missing".

## Appendix A: reference swapchain program (ran on 2026-10-31)

This is the exact program that produced the output in §4.3. It is scratch code that shows the call sequence; P1-04 ports it into `Swapchain`/`FrameContext`. It links its own volk because it does not link `engine_core`; engine code must not (gotcha 11). Build it out of tree:

<!-- file: build/spikes/swap/CMakeLists.txt -->
```cmake
cmake_minimum_required(VERSION 3.25)
project(spike_swap CXX C)
set(CMAKE_CXX_STANDARD 20)
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz")
FetchContent_MakeAvailable(SDL3)
find_package(Vulkan REQUIRED)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
add_executable(spike main.cpp ${REPO}/deps/volk/volk.c)
target_include_directories(spike PRIVATE ${REPO}/deps/volk ${STB_DIR})
target_compile_definitions(spike PRIVATE VK_NO_PROTOTYPES)
target_link_libraries(spike PRIVATE SDL3::SDL3-static ${CMAKE_DL_LIBS})
```

Run: `cmake -S build/spikes/swap -B build/spikes/swap/b -G Ninja -DSTB_DIR=<path to stb checkout> && ninja -C build/spikes/swap/b && SDL_VIDEO_DRIVER=offscreen build/spikes/swap/b/spike`. `<path to stb checkout>` is `build/ci-linux/_deps/_engine_stb-src` after a preset configure, or `$HOME/deps/stb` (§3.3).

<!-- file: build/spikes/swap/main.cpp -->
```cpp
#include <volk.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <cstdio>
#include <vector>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("FAIL %s = %d\n", #x, (int)r_); return 1; } } while (0)
int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
  std::printf("video driver: %s\n", SDL_GetCurrentVideoDriver());
  SDL_Window* win = SDL_CreateWindow("spike", 320, 240, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  if (!win) { std::printf("CreateWindow: %s\n", SDL_GetError()); return 1; }
  CK(volkInitialize());
  Uint32 n = 0; const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
  for (Uint32 i = 0; i < n; ++i) std::printf("inst ext: %s\n", exts[i]);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = n; ici.ppEnabledExtensionNames = exts;
  VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst)); volkLoadInstance(inst);
  VkSurfaceKHR surf; if (!SDL_Vulkan_CreateSurface(win, inst, nullptr, &surf)) { std::printf("surface: %s\n", SDL_GetError()); return 1; }
  uint32_t pc = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &pc, &pd);
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); std::printf("gpu: %s\n", pp.deviceName);
  VkBool32 sup = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup); std::printf("present support q0: %u\n", sup);
  float prio = 1.f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f13; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
  VkDevice dev; CK(vkCreateDevice(pd, &dci, nullptr, &dev)); volkLoadDevice(dev);
  VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  VkSurfaceCapabilitiesKHR caps; CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
  std::printf("caps: cur %ux%u min %u max %u usage 0x%x\n", caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags);
  uint32_t fc = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fc, nullptr); std::vector<VkSurfaceFormatKHR> fmts(fc); vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fc, fmts.data());
  for (auto& f : fmts) std::printf("fmt %d cs %d\n", f.format, f.colorSpace);
  uint32_t pmc = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &pmc, nullptr); std::vector<VkPresentModeKHR> pms(pmc); vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &pmc, pms.data());
  for (auto m : pms) std::printf("present mode %d\n", m);
  VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? VkExtent2D{320, 240} : caps.currentExtent;
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sci.surface = surf; sci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
  sci.imageFormat = fmts[0].format; sci.imageColorSpace = fmts[0].colorSpace; sci.imageExtent = ext; sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
  VkSwapchainKHR sc; CK(vkCreateSwapchainKHR(dev, &sci, nullptr, &sc));
  uint32_t ic = 0; vkGetSwapchainImagesKHR(dev, sc, &ic, nullptr); std::vector<VkImage> imgs(ic); vkGetSwapchainImagesKHR(dev, sc, &ic, imgs.data());
  std::printf("swapchain %ux%u images %u\n", ext.width, ext.height, ic);
  // readback buffer
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = ext.width * ext.height * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf; CK(vkCreateBuffer(dev, &bci, nullptr, &buf)); VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp); uint32_t mt = 0;
  for (; mt < mp.memoryTypeCount; ++mt) if ((mr.memoryTypeBits & (1u << mt)) && (mp.memoryTypes[mt].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = mt; VkDeviceMemory mem; CK(vkAllocateMemory(dev, &mai, nullptr, &mem)); vkBindBufferMemory(dev, buf, mem, 0);
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acq, done; vkCreateSemaphore(dev, &semi, nullptr, &acq); vkCreateSemaphore(dev, &semi, nullptr, &done);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkCreateFence(dev, &fci, nullptr, &fence);
  for (int frame = 0; frame < 3; ++frame) {
    uint32_t idx; CK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkResetCommandBuffer(cb, 0); vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.image = imgs[idx]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b; vkCmdPipelineBarrier2(cb, &di);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vci.image = imgs[idx]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = sci.imageFormat; vci.subresourceRange = b.subresourceRange;
    VkImageView view; CK(vkCreateImageView(dev, &vci, nullptr, &view));
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{1.0f, 0.5f, 0.0f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = {{0, 0}, ext}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri); vkCmdEndRendering(cb);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; vkCmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy reg{}; reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; reg.imageExtent = {ext.width, ext.height, 1};
    vkCmdCopyImageToBuffer(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT; b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT; b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; vkCmdPipelineBarrier2(cb, &di);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acq; si.pWaitDstStageMask = &ws; si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &done;
    CK(vkQueueSubmit(q, 1, &si, fence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &done; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(q, &pi); std::printf("frame %d image %u present %d\n", frame, idx, (int)pr);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fence); vkDestroyImageView(dev, view, nullptr);
  }
  void* p; vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p); auto* px = static_cast<unsigned char*>(p);
  std::printf("pixel0 bytes: %u %u %u %u (format %d)\n", px[0], px[1], px[2], px[3], sci.imageFormat);
  stbi_write_png("frame.png", (int)ext.width, (int)ext.height, 4, px, (int)ext.width * 4);
  vkDeviceWaitIdle(dev); std::printf("OK\n"); return 0;
}
```
