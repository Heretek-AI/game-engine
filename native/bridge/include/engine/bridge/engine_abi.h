// engine_abi.h — M1-04: the single-entry-point C99 ABI contract.
//
// The ONLY stable boundary between native frame state (Flecs tables in
// engine_core) and managed code (Engine.Bindings). Everything else in
// native/ may change layout freely; this header MUST NOT break layout
// without bumping ENGINE_ABI_VERSION_MAJOR.
//
// Design rules:
//   * Pure C99 (no C++): extern "C" guarded, fixed-width stdint types only,
//     no bool (use int32_t), no pointers-to-C++-classes, no VLAs.
//   * All pointers are raw table memory. Native OWNS the memory; managed
//     code only ever views it (ReadOnlySpan / Span over EcsChunkView).
//   * version/abi_version first in the table so a managed host can reject
//     a mismatched native binary before touching any function pointer.
//   * Function pointers are Cdecl. The C# generator emits
//     `delegate* unmanaged[Cdecl, SuppressGCTransition]` for each entry,
//     which JIT-compiles to a single raw `call` instruction — zero
//     P/Invoke stub overhead on the frame path.
//
// Generated bindings: tools/abi_codegen parses THIS file and emits
// managed/Engine.Bindings/NativeApi.g.cs. Edit both sides together and
// keep the layout assertions (native/tests/test_abi_layout.cpp +
// Engine.Tests layout checks) green.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define ENGINE_ABI_ALIGNAS(n) _Alignas(n)
#elif defined(__cplusplus) && (__cplusplus >= 201103L)
#define ENGINE_ABI_ALIGNAS(n) alignas(n)
#elif defined(_MSC_VER)
#define ENGINE_ABI_ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_ABI_ALIGNAS(n) __attribute__((aligned(n)))
#else
#define ENGINE_ABI_ALIGNAS(n)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// ABI version. Bump MAJOR on any layout change to the structs below or any
// signature change in EngineApiTable; MINOR/PATCH for comment-only edits.
// tools/abi_codegen embeds these into NativeApi.g.cs; managed asserts equal.
//
// M3-03 (ABI 3.0.0): appends the RenderPass registration tail
// (renderpass_create/destroy/add_image/add_pass/compile/run_frame) for the
// C# GraphicsCompositor. All M1/M2 entries keep order + signature (the
// M1-04 72B layout gate pins the head); new fields extend the tail only.
//
// M4-03 (ABI 4.0.0): appends the Physics spatial-query tail
// (physics_raycast/sphere_cast/overlap_sphere/raycast_batch) for the C#
// Physics API. All M1/M2/M3 entries keep order + signature; new fields
// extend the tail only.
//
// M5-02 (ABI 4.1.0): appends the Editor inspection tail
// (editor_create/destroy/spawn/set_name/set_parent/add_local/set_local/
// get_local/count_children/get_child/count_roots/get_root/is_prefab/
// begin_edit/end_edit + snapshot_take/restore/discard). Lets the C#
// Engine.Editor query the Flecs hierarchy (EcsChildOf/EcsIsA), edit native
// component memory in place, and checkpoint/restore worlds for PIE.
// All ABI 1-3 entries keep order + signature; new fields extend the tail.
//
// ABI 4.2.0: appends the generic component/system tail
// (component_register/entity_create/entity_set/entity_get/system_register/
// systems_run) used by Engine.Analyzers-generated IComponent systems.
// Every user component gets its own Flecs column; systems receive one
// EngineChunkColumns view per matched table. All ABI 4.1 entries keep
// order + signature; new fields extend the tail.
//
// ABI 4.3.0: appends the P2-02 input tail (input_get_state) exposing the
// event-fed input snapshot for the C# Input API. EngineRenderer arrives
// as a new opaque handle (P1-09 owns renderer lifecycle: create/destroy/
// frame; this entry only reads the renderer's input member). All ABI 4.2
// entries keep order + signature; new fields extend the tail.
// ABI 4.4.0: appends the P1-09 renderer tail (renderer_create/destroy/
// frame/mesh_create/screenshot) AFTER input_get_state (merged after P2-02
// landed on main; append-at-end keeps both tails' offsets stable).
//
// ABI 4.5.0: appends the P6-07 2D physics tail (physics2d_create/
// destroy/body_box/body_circle/body_polygon/body_destroy/step/drain)
// AFTER the renderer tail (merged after P6-07 landed on its own branch
// against ABI 4.3; append-at-end keeps the 4.3/4.4 offsets stable).
// Box2D v3 worlds are caller-owned (same model as the M4-01 3D world);
// the 2D collision ring is owned by the World2D and drained into the
// caller's buffer (zero managed alloc). All earlier entries keep order
// + signature; new fields extend the tail only.
// ---------------------------------------------------------------------------
#define ENGINE_ABI_VERSION_MAJOR 4
#define ENGINE_ABI_VERSION_MINOR 5
#define ENGINE_ABI_VERSION_PATCH 0

// 64-byte cache-line pitch for the component columns handed to managed
// code. Matches alignas(64) LocalTransform / WorldTransform (M1-03):
// codegen emits Debug.Assert stride == 64 on the managed mirrors.
#define ENGINE_ABI_COLUMN_STRIDE 64

// ---------------------------------------------------------------------------
// Inline component mirrors. The C# generator emits blittable structs with
// the same field order; static asserts below pin the native side and the
// managed layout tests pin the C# side.
// ---------------------------------------------------------------------------

// LocalTransform mirror: 16 x float32 = 64 bytes (M1-03 layout).
// px,py,pz | qx,qy,qz,qw | sx,sy,sz | pad[5].
// 64-byte aligned: component columns ride whole cache lines, and managed
// mirrors use explicit Size=64. alignas(64) on the C side to match.
typedef struct ENGINE_ABI_ALIGNAS(64) EngineLocalTransform {
  float px;
  float py;
  float pz;
  float qx;
  float qy;
  float qz;
  float qw;
  float sx;
  float sy;
  float sz;
  float pad[6];
} EngineLocalTransform;

// WorldTransform mirror: row-major 4x4 = 64 bytes.
typedef struct ENGINE_ABI_ALIGNAS(64) EngineWorldTransform {
  float m[16];
} EngineWorldTransform;

// Velocity mirror (M1-05): (vx, vy, vz, pad) = 16 bytes, 16-byte aligned.
// The benchmark archetype is (Local, World, Velocity); the Velocity column
// rides `user_data` when the registration context is null — see
// engine_abi.cpp. Deliberately NOT referenced by EngineApiTable: the M1-04
// table shape (and MAJOR version) stay frozen; codegen ignores this struct
// the same way it ignores opaque handles (field walk is table-driven).
typedef struct ENGINE_ABI_ALIGNAS(16) EngineVelocity {
  float vx;
  float vy;
  float vz;
  float pad;
} EngineVelocity;

// ---------------------------------------------------------------------------
// EcsChunkView — one Flecs table slice handed to managed per callback.
//
// A managed system receives a POINTER to this struct (never by value:
// the struct is 40 bytes and must not be copied on the frame path).
// `local` / `world` point at the first element of the table columns;
// element i is at ((T*)ptr)[i] because columns are tightly packed arrays
// of 64-byte components. `entities` (may be NULL) points at the first
// ecs_entity_t (uint64) of the chunk; NULL means "entity ids not needed".
// `user_data` round-trips the context pointer registered with the system.
// ---------------------------------------------------------------------------
typedef struct EcsChunkView {
  const EngineLocalTransform* local;  // LocalTransform column (never NULL)
  EngineWorldTransform* world;        // WorldTransform column (never NULL)
  const uint64_t* entities;           // entity ids, or NULL
  int32_t count;                      // entities in this chunk (> 0)
  int32_t stride_local;               // == ENGINE_ABI_COLUMN_STRIDE
  int32_t stride_world;               // == ENGINE_ABI_COLUMN_STRIDE
  void* user_data;                    // opaque callback context
} EcsChunkView;

// ---------------------------------------------------------------------------
// ManagedSystemCallback — signature of a managed per-chunk system.
//
// Implemented in C# as [UnmanagedCallersOnly(CallConvs = [CallConvCdecl])]
// static void. Called on native worker threads (enkiTS pool): the body
// must be thread-safe, must not allocate (frame path), and must not throw
// (an uncaught throw across the ABI is fatal — M1-05 host installs a
// fail-fast guard). view is valid ONLY for the duration of the call.
// dt is the frame delta in seconds, passed BY VALUE per dispatch (t_adeaf670:
// per-dispatch ownership — no shared static, safe under parallel dispatch).
// ---------------------------------------------------------------------------
typedef void (*ManagedSystemCallback)(const EcsChunkView* view, float dt);

// ---------------------------------------------------------------------------
// FrameDispatch — per-dispatch parameters for run_systems.
//
// dt carries the frame delta in seconds. Callers pass a POINTER to a
// FrameDispatch (NULL = zero-dt default); the value is fanned out BY VALUE
// to every callback as `float dt`. EcsChunkView/user_data semantics are
// unchanged (velocity column still rides the null-context slot).
// ---------------------------------------------------------------------------
typedef struct FrameDispatch {
  float dt;
} FrameDispatch;

// ---------------------------------------------------------------------------
// Lifecycle / registration surface (Cdecl function pointers).
// ---------------------------------------------------------------------------

// Opaque native world handle (owns the Flecs world + cascade query).
typedef struct EngineWorld EngineWorld;

// Opaque render-pass registry (owns a native Compositor: RenderGraph +
// custom C# pass callbacks). Created per compositor, destroyed with it.
typedef struct EngineRenderPassRegistry EngineRenderPassRegistry;

// Custom render-pass callback (C ABI): invoked once per frame when the
// pass's turn arrives in compiled graph order. `pass_index` is the pass's
// REGISTRATION (add-order) index, not the compiled execution slot;
// `user_data` round-trips the registration context.
// Return 0 on success; nonzero marks the pass failed (isolated — the
// frame continues with the remaining passes).
typedef int32_t (*EngineRenderPassCallback)(uint32_t pass_index,
                                            void* user_data);

// Create/destroy the native world (registers Local/WorldTransform,
// cascade query). Returns NULL on failure. destroy is idempotent on NULL.
typedef EngineWorld* (*EngineWorldCreateFn)(void);
typedef void (*EngineWorldDestroyFn)(EngineWorld* world);

// Bulk-spawn `count` identity entities; returns entities actually created.
typedef uint64_t (*EngineWorldBulkCreateFn)(EngineWorld* world,
                                            uint64_t count);

// Run hierarchy propagation (BFS EcsCascade, SIMD). Returns 0 on success.
typedef int32_t (*EngineWorldPropagateFn)(EngineWorld* world);

// Register a managed system; invoked once per table chunk on run_systems.
// `name` is a NUL-terminated UTF-8 label (copied by native, may be NULL).
// Returns 0 on success, nonzero errno-style code on failure.
typedef int32_t (*EngineRegisterSystemFn)(EngineWorld* world,
                                          const char* name,
                                          ManagedSystemCallback callback,
                                          void* user_data);

// Run all registered managed systems over every matching chunk, then the
// native propagation step. Returns 0 on success. All callbacks run on the
// calling thread unless task-threading was enabled (M1-03 scheduler).
// dispatch carries per-dispatch parameters (frame dt); pass NULL for the
// zero-dt default (keeps the M1-04 layout gate's counting-callback shape
// compiling unchanged).
typedef int32_t (*EngineRunSystemsFn)(EngineWorld* world,
                                      const FrameDispatch* dispatch);

// ---------------------------------------------------------------------------
// RenderPass registration tail (M3-03, ABI 3: GraphicsCompositor).
//
// A registry owns one native Compositor. The C# side creates a registry,
// adds graph images, registers custom passes (each: name + read/write
// image-name lists + callback + context), compiles, then runs one frame
// at a time. Image lists are NUL-terminated UTF-8 string arrays owned by
// the caller for the duration of the call (native copies what it keeps).
// All entries return 0 on success, nonzero errno-style codes on failure.
// Callbacks use the Cdecl convention (managed side: UnmanagedCallersOnly;
// plain Cdecl — a custom pass may touch GPU-mapped memory and block).
// ---------------------------------------------------------------------------

// Create/destroy a render-pass registry. destroy is idempotent on NULL.
typedef EngineRenderPassRegistry* (*EngineRenderPassCreateFn)(void);
typedef void (*EngineRenderPassDestroyFn)(EngineRenderPassRegistry* reg);

// Add a named graph image (transient scratch, e.g. visibility/HDR).
// Returns 0 on success.
typedef int32_t (*EngineRenderPassAddImageFn)(EngineRenderPassRegistry* reg,
                                              const char* name,
                                              uint64_t width,
                                              uint64_t height,
                                              int32_t transient_image);

// Register one custom pass: `name` + `reads`/`writes` image-name arrays
// (`read_count`/`write_count` entries) + per-frame callback + context.
// Returns 0 on success.
typedef int32_t (*EngineRenderPassAddPassFn)(EngineRenderPassRegistry* reg,
                                             const char* name,
                                             const char** reads,
                                             int32_t read_count,
                                             const char** writes,
                                             int32_t write_count,
                                             EngineRenderPassCallback callback,
                                             void* user_data);

// Compile the graph (built-ins + customs). Must succeed before run_frame.
typedef int32_t (*EngineRenderPassCompileFn)(EngineRenderPassRegistry* reg);

// Run one frame: walk compiled order, invoke each custom callback once.
// Returns 0 on success; -1 null registry, -2 not compiled. Per-pass
// results are readable via EngineRenderPassPassResult.
typedef int32_t (*EngineRenderPassRunFrameFn)(EngineRenderPassRegistry* reg);

// Last-frame result of custom pass `index` (registration order):
// 0 = ok / callback rc / -1 OOB. Returns -1 on null registry.
typedef int32_t (*EngineRenderPassPassResultFn)(
    const EngineRenderPassRegistry* reg, int32_t index);

// Failed-pass count from the last run_frame (0 when none failed).
typedef int32_t (*EngineRenderPassFailedCountFn)(
    const EngineRenderPassRegistry* reg);

// ---------------------------------------------------------------------------
// Physics spatial-query tail (M4-03, ABI 4: Physics.Raycast/SphereCast/
// OverlapSphere/RaycastBatch).
//
// The native physics world behind these queries is created OUTSIDE the
// EngineWorld lifecycle (M4-01 PhysicsWorld on the caller's scheduler +
// arena); the ABI handle is the owning PhysicsWorld pointer (borrowed —
// destroy the world only after the last query). All query entries are
// plain blocking Cdecl calls (they take per-body locks / join worker
// lanes — never SuppressGCTransition, same rule as run_systems).
//
// The batch entry shards rays over the owning world's shared enkiTS pool
// (lane 0 inline on the caller); singles take per-body shared locks.
//
// Layout (all blittable, matching managed/Engine.Runtime/Physics.cs):
//   EngineRay    — 24 bytes: ox,oy,oz, dx,dy,dz (direction*length).
//   EngineHit    — 48 bytes: px,py,pz, fraction, user_data,
//                  nx,ny,nz, body_id, hit.
// Opaque handle: EnginePhysicsWorld (the owning PhysicsWorld, borrowed).
// ---------------------------------------------------------------------------

// Opaque native physics world (the owning PhysicsWorld, NOT the bare
// JPH::PhysicsSystem: the batch entry needs the world's job system +
// worker count for the enkiTS lane fan-out). Lifetime borrows the caller's
// world — destroy the world only after the last query. See engine_abi.cpp.
typedef struct EnginePhysicsWorld EnginePhysicsWorld;

// Ray input: origin + direction*length. 24 bytes.
typedef struct EngineRay {
  float ox;
  float oy;
  float oz;
  float dx;
  float dy;
  float dz;
} EngineRay;

// Query hit. 48 bytes: point + fraction + body userdata, then normal +
// body id + hit flag.
typedef struct EngineHit {
  float px;
  float py;
  float pz;
  float fraction;
  uint64_t user_data;
  float nx;
  float ny;
  float nz;
  uint32_t body_id;
  uint32_t hit;
} EngineHit;

// Single raycast: closest hit (1) or miss (0, hit->hit = 0).
typedef int32_t (*EnginePhysicsRaycastFn)(const EnginePhysicsWorld* world,
                                          const EngineRay* ray,
                                          EngineHit* out_hit);

// Sphere cast: sphere of `radius` swept along the ray. 1 hit / 0 miss.
typedef int32_t (*EnginePhysicsSphereCastFn)(const EnginePhysicsWorld* world,
                                             const EngineRay* ray,
                                             float radius, EngineHit* out_hit);

// Sphere overlap: bodies intersecting the sphere. Writes up to `capacity`
// body ids (+userdata when non-null), returns bodies written.
typedef uint32_t (*EnginePhysicsOverlapSphereFn)(
    const EnginePhysicsWorld* world, float cx, float cy, float cz,
    float radius, uint32_t* out_body_ids, uint64_t* out_user_data,
    uint32_t capacity);

// Parallel batch: `count` rays -> `count` hits, sharded over native worker
// lanes. Returns rays processed.
typedef uint32_t (*EnginePhysicsRaycastBatchFn)(
    const EnginePhysicsWorld* world, const EngineRay* rays, EngineHit* hits,
    uint32_t count);


// ---------------------------------------------------------------------------
// Editor inspection tail (M5-02, ABI 4.1: Engine.Editor hierarchy + in-place
// edits + PIE snapshots).
//
// A registry owns per-world editor sessions (opaque to C#): each session
// binds ONE EngineWorld and caches the queries the panels need. The C#
// hierarchy panel walks roots -> children via the count/get pairs; the
// inspector reads/writes LocalTransform memory in place between
// begin_edit/end_edit (zero-copy, Command-pattern Undo on the C# side);
// PIE checkpoints the scene via snapshot_take and restores it on exit.
//
// All entries return 0 on success, nonzero errno-style codes on failure
// (null handles, OOB indices, unknown entities). Handles:
//   EngineEditorSession* — one bound editor session (destroy idempotent).
//   EngineEditorSnapshot* — one scene checkpoint (discard idempotent).
// Names are NUL-terminated UTF-8 owned by the caller (native copies).
// ---------------------------------------------------------------------------

// Opaque editor session (owns queries bound to one EngineWorld).
typedef struct EngineEditorSession EngineEditorSession;

// Opaque PIE snapshot (owns one scene checkpoint of one world).
typedef struct EngineEditorSnapshot EngineEditorSnapshot;

// Create/destroy an editor session bound to `world`. NULL world -> NULL.
typedef EngineEditorSession* (*EngineEditorCreateFn)(EngineWorld* world);
typedef void (*EngineEditorDestroyFn)(EngineEditorSession* session);

// Spawn one named identity entity (Local + World). Returns 0 on success
// and writes the new entity id to `out_entity` (0 status, not the id);
// nonzero on failure. Duplicate names always allocate a FRESH entity
// (ecs_new + ecs_set_name: never resolves to an existing row); exact
// duplicate name strings share the Flecs name index, so prefer unique
// names for lookup and accept that name->entity lookup may be ambiguous.
// `out_entity` may be NULL (spawn still happens).
typedef int32_t (*EngineEditorSpawnFn)(EngineEditorSession* session,
                                       const char* name, uint64_t* out_entity);

// Rename an entity (empty/NULL name clears to unnamed). 0 on success.
typedef int32_t (*EngineEditorSetNameFn)(EngineEditorSession* session,
                                         uint64_t entity, const char* name);

// Reparent: attach `entity` under `parent` (EcsChildOf), or detach to a
// root when `parent` is 0. 0 on success.
typedef int32_t (*EngineEditorSetParentFn)(EngineEditorSession* session,
                                           uint64_t entity, uint64_t parent);

// Ensure the entity carries a LocalTransform row (idempotent). 0 on success.
typedef int32_t (*EngineEditorAddLocalFn)(EngineEditorSession* session,
                                          uint64_t entity);

// Overwrite the LocalTransform row (10 floats: px,py,pz,qx,qy,qz,qw,sx,sy,sz). 0 on success.
typedef int32_t (*EngineEditorSetLocalFn)(EngineEditorSession* session,
                                          uint64_t entity, const float* xyz_q_s);

// Read the LocalTransform row into `out_xyz_q_s` (10 floats). 0 on success.
typedef int32_t (*EngineEditorGetLocalFn)(const EngineEditorSession* session,
                                          uint64_t entity, float* out_xyz_q_s);

// Hierarchy walk: child count / indexed child of `parent` (0 = roots).
typedef int32_t (*EngineEditorCountChildrenFn)(
    const EngineEditorSession* session, uint64_t parent,
    uint64_t* out_count);
typedef int32_t (*EngineEditorGetChildFn)(const EngineEditorSession* session,
                                          uint64_t parent, uint64_t index,
                                          uint64_t* out_child);

// Root count / indexed root (roots = entities with no EcsChildOf parent).
typedef int32_t (*EngineEditorCountRootsFn)(
    const EngineEditorSession* session, uint64_t* out_count);
typedef int32_t (*EngineEditorGetRootFn)(const EngineEditorSession* session,
                                         uint64_t index, uint64_t* out_child);

// Prefab probe: 1 when the entity inherits from a prefab (EcsIsA),
// 0 when it does not, negative on error.
typedef int32_t (*EngineEditorIsPrefabInstanceFn)(
    const EngineEditorSession* session, uint64_t entity);

// Edit barrier: begin/end a direct in-place LocalTransform edit burst
// (the inspector's Command apply between, Undo replays the before-image).
// begin returns a writable LocalTransform pointer (NULL on error);
// end closes the burst. 0 on success.
typedef EngineLocalTransform* (*EngineEditorBeginEditFn)(
    EngineEditorSession* session, uint64_t entity);
typedef int32_t (*EngineEditorEndEditFn)(EngineEditorSession* session);

// PIE checkpoint: snapshot_take captures Local rows + hierarchy + names;
// snapshot_restore replays it (entities resurrected with the same ids;
// PIE-spawned extras are deleted); snapshot_discard frees the checkpoint.
// restore/discard are idempotent on NULL. 0 on success.
typedef EngineEditorSnapshot* (*EngineEditorSnapshotTakeFn)(
    EngineEditorSession* session);
typedef int32_t (*EngineEditorSnapshotRestoreFn)(
    EngineEditorSession* session, EngineEditorSnapshot* snapshot);
typedef void (*EngineEditorSnapshotDiscardFn)(
    EngineEditorSnapshot* snapshot);

// ---------------------------------------------------------------------------
// Generic component/system tail (ABI 4.2: Illusion Layer).
//
// Components are registered by NAME (a UTF-8 type name, e.g. the C#
// Type.FullName "Game.Mover"). The name is never split on '.', so
// "Game.Mover" is one flat Flecs entity. Registering the same name again
// with the same size/alignment returns the same id; a different
// size/alignment returns 0 (never aborts).
//
// A system is a list of up to ENGINE_ABI_MAX_TERMS component ids plus an
// access flag per term (ENGINE_ABI_ACCESS_IN = read-only,
// ENGINE_ABI_ACCESS_INOUT = read-write). systems_run walks systems in
// registration order and calls each callback once per matched table with
// an EngineChunkColumns view: columns[i] is the base of term i's column
// (element j at (char*)columns[i] + j * sizes[i]).
//
// Callbacks run on the thread that called systems_run. They MUST NOT call
// entity_create/entity_set/system_register (those return
// ENGINE_ABI_ERR_IN_RUN while systems_run is active).
// ---------------------------------------------------------------------------
#define ENGINE_ABI_MAX_TERMS 8
#define ENGINE_ABI_ACCESS_IN 0
#define ENGINE_ABI_ACCESS_INOUT 1

// Error codes shared by the generic tail (negative int32_t returns).
#define ENGINE_ABI_ERR_NULL_ARG -1
#define ENGINE_ABI_ERR_BAD_ARG -2
#define ENGINE_ABI_ERR_SIZE_MISMATCH -3
#define ENGINE_ABI_ERR_NOT_FOUND -4
#define ENGINE_ABI_ERR_IN_RUN -5
#define ENGINE_ABI_ERR_INTERNAL -6

// One matched table slice handed to a generic system callback. 120 bytes
// on 64-bit targets (the engine is 64-bit only).
typedef struct EngineChunkColumns {
  void* columns[8];         // term i column base, i < term_count
  int32_t sizes[8];         // term i element size in bytes
  const uint64_t* entities; // entity ids for the rows (never NULL)
  int32_t count;            // rows in this slice (> 0)
  int32_t term_count;       // terms in this system (1..8)
  void* user_data;          // context passed to system_register
} EngineChunkColumns;

// ---------------------------------------------------------------------------
// P2-02 input snapshot tail (ABI 4.3: C# Input API + action maps).
//
// The snapshot is a plain-data projection of the event-fed input tables
// (engine::input::InputState, P2-01): held/pressed/released key tables
// (SDL scancode index, 512 slots), mouse position/delta + SDL button
// bitmask (bit b-1 = SDL_BUTTON_* b), and per-pad axis/button tables
// (up to 4 pads x 6 axes, normalised to [-1, 1]). Native owns the memory
// (a member of the renderer, P1-09); managed views it zero-copy via the
// returned pointer, valid until the next frame pump. Buttons beyond
// SDL_BUTTON 32 are unrepresentable in the bitmask and read as released;
// unknown pads read 0 / released. Plain Cdecl — the entry never blocks.
// ---------------------------------------------------------------------------

// Opaque native renderer handle (P1-09 owns renderer_create/destroy/
// frame; this entry only borrows it to read the input snapshot).
typedef struct EngineRenderer EngineRenderer;

// Blittable input snapshot: 1536 + 4*4 + 4 + 96 + 16 = 1668 bytes.
typedef struct EngineInputState {
  uint8_t keys[512];      // held table (scancode index)
  uint8_t pressed[512];   // went down this frame
  uint8_t released[512];  // went up this frame
  float mouse_x;          // last motion position
  float mouse_y;
  float mouse_dx;  // accumulated this frame
  float mouse_dy;
  uint32_t mouse_buttons;  // SDL button bitmask
  float pad_axes[4][6];    // slot x axis, [-1, 1]
  uint32_t pad_buttons[4];  // per-slot SDL button bitmask
} EngineInputState;

// Borrowed snapshot of the renderer's input state (NULL renderer -> NULL).
// Zero-copy: the pointer aliases native memory owned by the renderer.
typedef const EngineInputState* (*EngineInputGetStateFn)(
    EngineRenderer* renderer);

// ---------------------------------------------------------------------------
// Renderer tail (P1-09, ABI 4.4): window + frame loop + mesh upload for one
// world, driven from C#.
//
// EngineRenderer is an opaque native handle owning the SDL3 window, the
// Vulkan device + swapchain + frame context, the ForwardRenderer mesh
// pipeline, one depth buffer, and the uploaded GpuMeshes (mesh_id is the
// vector index + 1; 0 is reserved/invalid). The world is BORROWED (owned
// by the caller — destroy the renderer before the world, or frames stop).
//
// All entries are plain blocking Cdecl calls (they pump SDL, wait fences,
// and submit GPU work — never SuppressGCTransition, same rule as
// run_systems).
//
// Opaque handle: EngineRenderer (owns window + swapchain + meshes).
// ---------------------------------------------------------------------------

// Create a renderer for `world` (borrowed, must outlive the renderer):
// opens a hidden (flags & 1) or shown SDL3 window of width x height,
// builds the mesh pipeline (D32_SFLOAT depth), and registers the P1-08
// render components on the world (idempotent). Returns NULL on failure
// (null world, zero extent, no Vulkan device, pipeline build failure).
// Flag bit 0 (ENGINE_RENDERER_HIDDEN = 1): hidden window (offscreen CI).
#define ENGINE_RENDERER_HIDDEN 1u

// Create/destroy a renderer. destroy frees the renderer only (never the
// world); idempotent on NULL.
typedef EngineRenderer* (*EngineRendererCreateFn)(EngineWorld* world,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t flags);
typedef void (*EngineRendererDestroyFn)(EngineRenderer* renderer);

// Pump SDL events, run one render_world frame over the bound world, and
// present. Returns 0 on success, 1 when the window was closed (SDL_EVENT_QUIT
// seen: the caller should stop its loop), negative errno-style codes on
// failure (null renderer, frame errors). Skips cleanly (0, no draw) when
// the world has no is_main camera yet.
typedef int32_t (*EngineRendererFrameFn)(EngineRenderer* renderer);

// Upload one interleaved MeshVertex buffer (pos[3], normal[3], uv[2] =
// 32 bytes/vertex; `vertices` carries vertex_count * 8 floats) plus u32
// indices. Returns the mesh_id (>= 1; 0 is invalid) or 0 on failure
// (null renderer/vertices/indices, zero counts). Registered in the
// renderer's MeshRegistry so render_world draws resolve it.
typedef uint32_t (*EngineRendererMeshCreateFn)(
    EngineRenderer* renderer, const float* vertices, uint32_t vertex_count,
    const uint32_t* indices, uint32_t index_count);

// Capture the last presented frame to `png_path` (NUL-terminated UTF-8,
// caller-owned). Returns 0 on success, negative on failure (null
// renderer/path, no frame rendered yet, write failure).
typedef int32_t (*EngineRendererScreenshotFn)(EngineRenderer* renderer,
                                              const char* png_path);

// Generic system callback: one call per matched table, dt by value.
typedef void (*EngineSystemCallback)(const EngineChunkColumns* view, float dt);

// Register (or look up) a component by name. Returns the component id,
// or 0 on failure (null/empty name, size == 0, alignment not a power of
// two or > 64, size % alignment != 0, or an existing component with the
// same name but a different size/alignment).
typedef uint64_t (*EngineComponentRegisterFn)(EngineWorld* world,
                                              const char* name,
                                              uint32_t size,
                                              uint32_t alignment);

// Create one empty entity. Returns its id, or 0 on failure (including
// while systems_run is active).
typedef uint64_t (*EngineEntityCreateFn)(EngineWorld* world);

// Copy `size` bytes from `data` into `component` on `entity` (adds the
// component if missing). `size` must equal the registered size.
// Returns 0 on success or an ENGINE_ABI_ERR_* code.
typedef int32_t (*EngineEntitySetFn)(EngineWorld* world, uint64_t entity,
                                     uint64_t component, const void* data,
                                     uint32_t size);

// Copy `component` of `entity` into `out` (`size` must equal the
// registered size). Returns 0 on success, ENGINE_ABI_ERR_NOT_FOUND when
// the entity does not have the component, or another ENGINE_ABI_ERR_*.
typedef int32_t (*EngineEntityGetFn)(EngineWorld* world, uint64_t entity,
                                     uint64_t component, void* out,
                                     uint32_t size);

// Register a system over `term_count` component ids (1..8) with one
// access flag per term. Returns the system index (>= 0) or an
// ENGINE_ABI_ERR_* code. `name` is copied (truncated to 63 bytes).
typedef int32_t (*EngineSystemRegisterFn)(EngineWorld* world,
                                          const char* name,
                                          const uint64_t* components,
                                          const uint32_t* access,
                                          uint32_t term_count,
                                          EngineSystemCallback callback,
                                          void* user_data);

// Run every generic system once, in registration order, serially on the
// calling thread. dispatch may be NULL (dt = 0). Returns 0 on success or
// an ENGINE_ABI_ERR_* code.
typedef int32_t (*EngineSystemsRunFn)(EngineWorld* world,
                                      const FrameDispatch* dispatch);

// Opaque native 2D physics world (caller-owned engine::physics::World2D,
// NOT the raw b2WorldId: the drain entry needs the world's owned event
// ring + body bookkeeping. Create after the scheduler/arena are up;
// destroy only after the last step/drain).
typedef struct EnginePhysics2DWorld EnginePhysics2DWorld;

// 2D collision event: same 32-byte record as the 3D path (P2-05 shape
// parity — entity_a/entity_b carry Flecs entities, kind 0=enter 1=exit,
// sensor 1 when either shape is a sensor; point is the body-A origin and
// z is always 0). Static asserts below pin size + offsets.
typedef struct EngineCollisionEvent2D {
  uint64_t entity_a;
  uint64_t entity_b;
  float point_x;
  float point_y;
  float point_z;
  uint8_t kind;
  uint8_t sensor;
  uint8_t reserved[2];
} EngineCollisionEvent2D;

// 2D Rigidbody + collider descriptors (blittable, field order matches
// managed/Engine.Runtime/Physics2D.cs): type 0=static 1=kinematic
// 2=dynamic; polygon carries up to 6 local-space vertices.
typedef struct EngineRigidbody2D {
  float gravity_scale;
  float linear_damping;
  float angular_damping;
  uint8_t fixed_rotation;
  uint8_t bullet;
  uint8_t allow_sleeping;
  uint8_t sensor;
  uint8_t active;
  uint8_t type;
  uint8_t reserved[4];
} EngineRigidbody2D;

typedef struct EngineBoxCollider2D {
  float hx;
  float hy;
  float density;
  float friction;
  float restitution;
  uint8_t sensor;
  uint8_t reserved[3];
} EngineBoxCollider2D;

typedef struct EngineCircleCollider2D {
  float cx;
  float cy;
  float radius;
  float density;
  float friction;
  float restitution;
  uint8_t sensor;
  uint8_t reserved[3];
} EngineCircleCollider2D;

typedef struct EnginePolygonCollider2D {
  float vx[6];
  float vy[6];
  float density;
  float friction;
  uint8_t count;
  uint8_t sensor;
  uint8_t reserved[6];
} EnginePolygonCollider2D;

// Create/destroy a 2D world (gravity in m/s^2; sub_steps 0 => 4).
// Returns NULL on null args; destroy is idempotent on NULL.
typedef EnginePhysics2DWorld* (*EnginePhysics2DCreateFn)(
    float gravity_x, float gravity_y, uint32_t sub_steps);
typedef void (*EnginePhysics2DDestroyFn)(EnginePhysics2DWorld* world);

// Spawn one body + its collider (entity carried as body user-data;
// angle in radians). Returns the body id (0 on failure).
typedef uint64_t (*EnginePhysics2DBodyBoxFn)(
    EnginePhysics2DWorld* world, uint64_t entity, float px, float py,
    float angle, const EngineRigidbody2D* rb,
    const EngineBoxCollider2D* box);
typedef uint64_t (*EnginePhysics2DBodyCircleFn)(
    EnginePhysics2DWorld* world, uint64_t entity, float px, float py,
    float angle, const EngineRigidbody2D* rb,
    const EngineCircleCollider2D* circle);
typedef uint64_t (*EnginePhysics2DBodyPolygonFn)(
    EnginePhysics2DWorld* world, uint64_t entity, float px, float py,
    float angle, const EngineRigidbody2D* rb,
    const EnginePolygonCollider2D* poly);
typedef void (*EnginePhysics2DBodyDestroyFn)(EnginePhysics2DWorld* world,
                                             uint64_t body);

// Fixed-step update (dt > 0): b2World_Step + drain into the world's
// owned ring. Returns bodies simulated (0 on null world/bad dt).
typedef uint32_t (*EnginePhysics2DStepFn)(EnginePhysics2DWorld* world,
                                          float dt);

// Drain pending 2D contact events (begin + end) into the caller's
// buffer (capacity slots). Returns events written; drops counted on
// the native ring when the caller's buffer is smaller than pending.
typedef uint32_t (*EnginePhysics2DDrainFn)(EnginePhysics2DWorld* world,
                                           EngineCollisionEvent2D* out,
                                           uint32_t capacity);

// ---------------------------------------------------------------------------
// EngineApiTable — the single-entry-point ABI surface.
//
// `size` is sizeof(EngineApiTable) as compiled into the native binary; the
// managed host rejects the table when size < its own sizeof (native older
// than managed). `abi_version_*` must equal ENGINE_ABI_VERSION_*.
// `column_stride` must equal ENGINE_ABI_COLUMN_STRIDE (64).
// ---------------------------------------------------------------------------
typedef struct EngineApiTable {
  uint64_t size;
  int32_t abi_version_major;
  int32_t abi_version_minor;
  int32_t abi_version_patch;
  int32_t column_stride;
  EngineWorldCreateFn world_create;
  EngineWorldDestroyFn world_destroy;
  EngineWorldBulkCreateFn world_bulk_create;
  EngineWorldPropagateFn world_propagate;
  EngineRegisterSystemFn register_system;
  EngineRunSystemsFn run_systems;
  // M3-03 RenderPass registration tail (ABI 3). Head order frozen.
  EngineRenderPassCreateFn renderpass_create;
  EngineRenderPassDestroyFn renderpass_destroy;
  EngineRenderPassAddImageFn renderpass_add_image;
  EngineRenderPassAddPassFn renderpass_add_pass;
  EngineRenderPassCompileFn renderpass_compile;
  EngineRenderPassRunFrameFn renderpass_run_frame;
  EngineRenderPassPassResultFn renderpass_pass_result;
  EngineRenderPassFailedCountFn renderpass_failed_count;
  // M4-03 Physics spatial-query tail (ABI 4). Head + renderpass order frozen.
  EnginePhysicsRaycastFn physics_raycast;
  EnginePhysicsSphereCastFn physics_sphere_cast;
  EnginePhysicsOverlapSphereFn physics_overlap_sphere;
  EnginePhysicsRaycastBatchFn physics_raycast_batch;
  // M5-02 Editor inspection tail (ABI 4.1). Head + M3/M4 tails order frozen.
  EngineEditorCreateFn editor_create;
  EngineEditorDestroyFn editor_destroy;
  EngineEditorSpawnFn editor_spawn;
  EngineEditorSetNameFn editor_set_name;
  EngineEditorSetParentFn editor_set_parent;
  EngineEditorAddLocalFn editor_add_local;
  EngineEditorSetLocalFn editor_set_local;
  EngineEditorGetLocalFn editor_get_local;
  EngineEditorCountChildrenFn editor_count_children;
  EngineEditorGetChildFn editor_get_child;
  EngineEditorCountRootsFn editor_count_roots;
  EngineEditorGetRootFn editor_get_root;
  EngineEditorIsPrefabInstanceFn editor_is_prefab_instance;
  EngineEditorBeginEditFn editor_begin_edit;
  EngineEditorEndEditFn editor_end_edit;
  EngineEditorSnapshotTakeFn editor_snapshot_take;
  EngineEditorSnapshotRestoreFn editor_snapshot_restore;
  EngineEditorSnapshotDiscardFn editor_snapshot_discard;
  // ABI 4.2 generic component/system tail. All earlier order frozen.
  EngineComponentRegisterFn component_register;
  EngineEntityCreateFn entity_create;
  EngineEntitySetFn entity_set;
  EngineEntityGetFn entity_get;
  EngineSystemRegisterFn system_register;
  EngineSystemsRunFn systems_run;
  // P2-02 input snapshot tail (ABI 4.3). All earlier order frozen.
  EngineInputGetStateFn input_get_state;
  // P1-09 renderer tail (ABI 4.4). All earlier order frozen.
  // Renderer owns the SDL3 window + swapchain + frame loop + meshes for
  // one world. create() takes ownership of nothing (the world stays owned
  // by the caller); destroy() frees the renderer only.
  EngineRendererCreateFn renderer_create;
  EngineRendererDestroyFn renderer_destroy;
  EngineRendererFrameFn renderer_frame;
  EngineRendererMeshCreateFn renderer_mesh_create;
  EngineRendererScreenshotFn renderer_screenshot;
  // P6-07 2D physics tail (ABI 4.5). Merged after P1-09/input landed on
  // main, so the tail sits AFTER the renderer entries: append-at-end keeps
  // the 4.3/4.4 offsets stable. All earlier order frozen.
  EnginePhysics2DCreateFn physics2d_create;
  EnginePhysics2DDestroyFn physics2d_destroy;
  EnginePhysics2DBodyBoxFn physics2d_body_box;
  EnginePhysics2DBodyCircleFn physics2d_body_circle;
  EnginePhysics2DBodyPolygonFn physics2d_body_polygon;
  EnginePhysics2DBodyDestroyFn physics2d_body_destroy;
  EnginePhysics2DStepFn physics2d_step;
  EnginePhysics2DDrainFn physics2d_drain;

} EngineApiTable;

// Single entry point: returns a pointer to the process-lifetime API table
// (never NULL, stable address). Exported from engine_core: under C++ the
// prototype carries ENGINE_CORE_API (dllexport/dllimport on Windows); the
// pure-C99 view stays undecorated so the header compiles as strict C99.
#ifdef __cplusplus
#include "engine/core/engine_core_export.hpp"
#endif
#ifdef __cplusplus
ENGINE_CORE_API
#endif
const EngineApiTable* engine_get_api_table(void);

#ifdef __cplusplus
}  // extern "C"
#endif
