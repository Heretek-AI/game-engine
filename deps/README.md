# Vendored native dependencies (M1-01)

Checked in directly (no submodules) so clean clones build offline after
checkout. **Do not hand-edit vendored trees** — upgrades are whole-tree
replacements, recorded below.

| Dep    | Version pinned | Upstream              | Notes |
|--------|---------------|-----------------------|-------|
| flecs  | `v4.1.6` (`fb55f3c2`) | https://github.com/SanderMertens/flecs | C99/C++ archetype ECS; built **shared** (`FLECS_SHARED=ON`, `FLECS_TESTS=OFF`) — single per-process universe (see `native/CMakeLists.txt`) |
| enkiTS | `master@404a3bf8` | https://github.com/dougbinks/enkiTS | C++ task scheduler; static lib, C interface ON (M1-03 binds it to Flecs task hooks), examples OFF |
| volk   | `master@2026-10` (`VOLK_HEADER_VERSION 366`, Vulkan 1.4 headers) | https://github.com/zeux/volk | Vulkan meta-loader; compiled as `engine_volk` static lib (M3-01, `native/renderer`) — only `volk.c`/`volk.h` vendored (re-vendor: record the exact commit SHA in this row and check it out in the recipe below) |
| VMA    | `master@2026-10` (v3.1.0-era `vk_mem_alloc.h`) | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | Header-only allocator; single TU `native/renderer/src/vma_impl.cpp` (base flags, M3-01) — only `include/vk_mem_alloc.h` vendored (re-vendor: record the exact commit SHA in this row and check it out in the recipe below) |
| JoltPhysics | `v5.6.0` | https://github.com/jrouwe/JoltPhysics | Physics kernel (M4-01: fetched at configure via FetchContent, pinned tag + SHA256 in `native/physics/CMakeLists.txt`, MIT; build-tree only, never vendored); built as static `Jolt` lib (C++17, exceptions/RTTI ON, own warning set), engine adapter in `native/physics/` links it + shared enkiTS pool |
| SDL3 | `release-3.4.18` (`c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90`) | https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz | SDL3 window + Vulkan surface (P1-02: fetched at configure via FetchContent, pinned tag + SHA256 in `native/renderer/CMakeLists.txt`, Zlib; build-tree only, never vendored); built **static** (`SDL3::SDL3-static`), real window path offscreen-capable headless |
| stb (stb_image_write) | `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` (`a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d`) | https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz | PNG writer for screenshots (P1-04: same tarball as the assets stb_image fetch, separate FetchContent name `_engine_stb_image_write` in `native/renderer/CMakeLists.txt`, MIT; header-only + single `stb_image_write_impl.c` TU) |

```bash
git clone --depth 1 --branch v4.1.6 https://github.com/SanderMertens/flecs.git deps/flecs
git clone --depth 1 https://github.com/dougbinks/enkiTS.git deps/enkiTS
# volk: replace <VOLK_SHA> with the exact upstream commit recorded in the
# pin table above (keeps re-vendors byte-identical to this PR).
git clone https://github.com/zeux/volk.git deps/volk-tmp && (cd deps/volk-tmp && git checkout <VOLK_SHA>) && mv deps/volk-tmp/volk.{c,h} deps/volk/ && mv deps/volk-tmp/LICENSE.md deps/volk/ && rm -rf deps/volk-tmp
# VMA: replace <VMA_SHA> likewise. rm -rf the old include tree first — the
# plain mv fails on every re-vendor when deps/vma/include already exists.
git clone https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git deps/vma-tmp && (cd deps/vma-tmp && git checkout <VMA_SHA>) && rm -rf deps/vma/include && mv deps/vma-tmp/include deps/vma/ && mv deps/vma-tmp/LICENSE.txt deps/vma/ && rm -rf deps/vma-tmp
rm -rf deps/flecs/.git deps/enkiTS/.git
# then update the pin table above with the new tag/SHA
```
| Box2D | `v3.2.0` | https://github.com/erincatto/box2d | MIT | P6-07 (fetched at configure via FetchContent, pinned tag + SHA256 in `native/physics/CMakeLists.txt`; build-tree only, never vendored) |
