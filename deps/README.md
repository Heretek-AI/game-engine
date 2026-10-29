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
