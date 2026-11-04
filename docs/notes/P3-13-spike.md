# P3-13 spike — Avalonia 12.1.4 GPU interop survey (OS × API matrix)

Date: 2026-11-04. Branch: `parity/p3-13-zero-copy-viewport`.
Rule-6 method: every type/member below was confirmed against the shipped
assemblies/XML docs in `~/.nuget/packages/avalonia/12.1.4/` (ref XML +
`strings` over `lib/net10.0/*.dll` + a `dotnet run` reflection probe in
`/tmp/avinspect`) — not from memory or the docs site. Avalonia version is
pinned: `Engine.EditorApp.csproj` / `Engine.EditorApp.Tests.csproj` both
reference `Avalonia` + `Avalonia.Desktop` `12.1.4`.

Prior art on this branch (already committed as `60ef522`, NOT this spike's
claim): `docs/notes/P3-13.md` + `GpuViewportBridge` +
`AvaloniaViewportInterop` + CPU bridge tests. This spike doc adds the
OS × API matrix, per-OS share path, and fallback conditions the
implementation consumes.

## Entry point (all OSes)

- `ElementComposition.GetElementVisual(host).Compositor` →
  `await Compositor.TryGetCompositionGpuInterop()` →
  `ICompositionGpuInterop` (`Avalonia.Base.xml`, reflection-confirmed).
- `null` return = no interop on this backend (headless/Skia/software) →
  CPU fallback, no throw.
- Display: `Compositor.CreateDrawingSurface()` +
  `CompositionDrawingSurface.{UpdateAsync, UpdateWithKeyedMutexAsync,
  UpdateWithSemaphoresAsync, UpdateWithTimelineSemaphoresAsync}` +
  `CompositionSurfaceVisual.Surface`, attached with
  `ElementComposition.SetElementChildVisual` (all confirmed in
  `Avalonia.Base.dll` reflection 2026-11-04).

## Query surface (never hardcode — ask the compositor at runtime)

- `ICompositionGpuInterop.SupportedImageHandleTypes` /
  `SupportedSemaphoreTypes` (string lists from
  `KnownPlatformGraphicsExternal{Image,Semaphore}HandleTypes`).
- `GetSynchronizationCapabilities(handleType)` →
  `CompositionGpuImportedImageSynchronizationCapabilities`
  (`Semaphores | KeyedMutex | Automatic | TimelineSemaphores`) — picks
  which `Update*Async` overload to call.
- Import: `ImportImage(IPlatformHandle, PlatformGraphicsExternalImageProperties)`
  (cross-device; `VulkanProperties.Layout` carries the `VkImageLayout` the
  engine left the image in) and
  `ImportImage(ICompositionImportableSharedGpuContextImage)` (same-GPU-context
  fast path, no fd/NT-handle round-trip when engine and Avalonia share one
  `VkDevice`). Same pair for semaphores.
- Lifetime: `ICompositionImportedObject.ImportCompleted` — once complete, the
  caller may free the engine-side non-owning share handle. Keep the `VkImage`
  alive until the `UpdateAsync` task completes.
- Health: `IsLost` (re-check per frame), `DeviceLuid`/`DeviceUuid`
  (confirm same physical device before importing).

## OS × API matrix (Avalonia 12.1.4)

| OS | Compositor backend (shipped in NuGet) | Image handle types | Semaphore handle types | Engine export call |
|---|---|---|---|---|
| Linux (X11/Wayland) | `Avalonia.Vulkan.dll` (`IVulkanExternalImage/Semaphore`, `VkImportMemoryFdInfoKHR`, `VkImportSemaphoreFdInfoKHR`) or `Avalonia.OpenGL.dll` (EGL `glImportMemoryFdEXT` / `glImportSemaphoreFdEXT`, `ICompositionImportableOpenGlSharedTexture`) | `VulkanOpaquePosixFileDescriptor` | `VulkanOpaquePosixFileDescriptor` | `vkGetMemoryFdKHR` (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`), `vkGetSemaphoreFdKHR` |
| Windows | Vulkan NT-handle path (`VkImportMemoryWin32HandleInfoKHR`, `VkImportSemaphoreWin32HandleInfoKHR`) or D3D11 shared-handle path (ANGLE strings present in `Avalonia.OpenGL.dll`) | `VulkanOpaqueNtHandle`, `D3D11TextureNtHandle`, `D3D11TextureGlobalSharedHandle` | `VulkanOpaqueNtHandle`, `Direct3D12FenceNtHandle` | `vkGetMemoryWin32HandleKHR` (`OPAQUE_WIN32_BIT`) or `IDXGIResource1::CreateSharedHandle` (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) |
| macOS | `Avalonia.Metal.dll` (`IMetalExternalObjectsFeature`, `IMetalExternalTexture`, `IMetalSharedEvent`) | `IOSurfaceRef` | `MetalSharedEvent` | MoltenVK export to IOSurface / `MTLSharedEvent` from `VkSemaphore` |

XML-doc notes that constrain the design:

- `D3D11TextureGlobalSharedHandle` (`IDXGIResource::GetSharedHandle`,
  `D3D11_RESOURCE_MISC_SHARED[_KEYEDMUTEX]`): handle does NOT own the video
  memory reference — the engine must keep the texture valid until import
  completes.
- `IExternalObjectsHandleWrapRenderInterfaceContextFeature`: wrap
  synchronously for MoltenVK `MTLSharedEvent`/IOSurface handles, which are
  `__unsafe_unretained` — do not destroy the Vk source object first.

## Recommended share path per OS

- Linux: engine renders viewport to a dedicated `VkImage` created with
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`, exports per frame with
  `vkGetMemoryFdKHR`, `ImportImage(new PlatformHandle(fd,
  VulkanOpaquePosixFileDescriptor), props)` → `UpdateAsync` (or the
  semaphore/keyed-mutex/timeline variant per capabilities) on the Scene
  panel's `CompositionDrawingSurface`. Preferred: same-`VkDevice`
  `ICompositionImportableSharedGpuContextImage` when available (no fd).
- Windows: same shape with `OPAQUE_WIN32_BIT` NT handles; if the engine
  renders via D3D11, `CreateSharedHandle` + `D3D11TextureNtHandle` (global
  shared handle only for legacy `GetSharedHandle` textures).
- macOS: IOSurface-backed `VkImage` via MoltenVK → `IOSurfaceRef` import,
  `MetalSharedEvent` for cross-queue sync; wrap synchronously per the XML
  warning above.

Engine-side prerequisite (not yet in tree): `VulkanDevice::init` currently
enables no device extensions and has no external-memory allocation path —
the shareable image + `vkGetMemoryFdKHR/Win32HandleKHR` export must be
added behind the existing `enable_swapchain`-style flag pattern.

## Fallback conditions (CPU readback stays)

Use the `WriteableBitmap` lock/`InvalidateVisual` path (§4.5, ~2 ms per
1280×720 copy) whenever ANY of these hold:

1. `TryGetCompositionGpuInterop()` returns null (headless tests with
   `UseHeadlessDrawing=false`, Skia/software compositor).
2. `IsLost` is true (check per frame; device loss mid-session).
3. `SupportedImageHandleTypes` lacks the engine's export type
   (backend/handle mismatch — e.g. Vulkan-fd engine vs D3D11 compositor).
4. `ImportImage`/`Update*Async` throws.
5. `DeviceUuid`/`DeviceLuid` mismatch (engine and compositor on different
   physical GPUs — import would fail or copy anyway).
6. `ENGINE_REQUIRE_GPU` unset / `SDL_VIDEO_DRIVER=offscreen` headless runs.

Headless golden tests always take this path by construction (no compositor
exists), so the P3-13 golden half is unaffected by interop availability.

## What this host proves (2026-11-04)

- `/dev/dri`: `card0` + `renderD128` present, but `vulkaninfo` not
  installed and `nvidia-smi` absent → lavapipe/software only, no real GPU.
- All type/member/handle-type claims above come from the 12.1.4 assemblies
  on disk, verified this session. The 1920×1080 with/without-copy timing
  comparison requires a real-GPU host with a compositor and is NOT claimed
  here (see `docs/notes/P3-13.md` for the lavapipe fallback numbers and
  the `engine_compositor_live` harness that prints both `[INFO]
  viewport-timing` lines).
