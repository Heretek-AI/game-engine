# Milestone 3: Zero-Config GPU-Driven Vulkan 1.3 Renderer

* **Timeline:** Months 5–7
* **Verification Gate:** Render $250,000$ dynamic PBR mesh instances with cascaded/virtual shadows and real-time diffuse global illumination at $120+\text{ FPS}$ with $< 0.5\text{ ms}$ CPU render thread overhead.

---

## GitHub Issues Breakdown

### Issue M3-01: SDL3 Windowing, Vulkan 1.3 Bootstrap (`volk` + `VMA`), and Render Graph
* **Labels:** `area:renderer`, `priority:critical`
* **Description:** Initialize Vulkan 1.3 with dynamic rendering (`VK_KHR_dynamic_rendering`), synchronization2 (`VK_KHR_synchronization2`), `buffer_device_address`, and a declarative DAG Render Graph.
* **Acceptance Criteria:**
  - [ ] Integrate SDL3 window creation and Vulkan surface management in `native/renderer/`.
  - [ ] Initialize `volk` and `VulkanMemoryAllocator` (VMA) with dedicated ReBAR (Resizable BAR) ring buffers for fast CPU-to-GPU transform uploads.
  - [ ] Build a declarative `RenderGraph` that automatically computes image layout transitions, memory aliasing, and pipeline barriers per frame.

---

### Issue M3-02: Bindless Descriptor Table & GPU-Driven Indirect Drawing
* **Labels:** `area:renderer`, `priority:critical`
* **Description:** Eliminate CPU draw-call loops by storing all scene geometry, materials, and transforms in GPU SSBOs and rendering via `vkCmdDrawIndexedIndirectCount`.
* **Acceptance Criteria:**
  - [ ] Create a single global bindless descriptor set (`VK_EXT_descriptor_indexing`) holding all textures, samplers, and material buffers.
  - [ ] Stream dirty `WorldTransform` matrices from Flecs tables directly to a persistent GPU Scene SSBO at the end of every simulation frame.
  - [ ] Implement a two-phase GPU Compute Culling pass (Frustum culling + Hierarchical-Z Pyramid occlusion culling) that emits draw commands to an indirect draw buffer.

---

### Issue M3-03: Slang Shader Compiler Pipeline & C# `GraphicsCompositor` API
* **Labels:** `area:renderer`, `area:abi-bridge`
* **Description:** Integrate the **Slang** shader compiler for modular shaders and expose the `RenderGraph` to C# so users can write custom render passes easily.
* **Acceptance Criteria:**
  - [ ] Embed `slang` in `native/renderer/shaders/` with automatic hot-reloading when `.slang` files are modified on disk.
  - [ ] Implement a PBR Metallic-Roughness visibility buffer / forward+ shading pipeline.
  - [ ] Expose `RenderPass` registration across the ABI bridge so C# developers can inject custom compute or post-processing passes into the frame graph.

---

### Issue M3-04: Zero-Bake Real-Time GI (Signed Distance Fields + Radiance Cascades)
* **Labels:** `area:renderer`
* **Description:** Deliver "it just works" real-time 3D lighting out of the box without static lightmap baking progress bars.
* **Acceptance Criteria:**
  - [ ] Generate sparse 3D Signed Distance Field (SDF) bricks for imported meshes on background worker threads.
  - [ ] Implement Screen-Space + World-Space SDF Radiance Cascades for real-time multibounce diffuse global illumination and sky occlusion.
  - [ ] Implement Virtual Shadow Maps (or SDSM) with soft contact shadows and ACES tonemapping.