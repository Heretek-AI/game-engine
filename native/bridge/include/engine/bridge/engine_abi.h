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
// ---------------------------------------------------------------------------
#define ENGINE_ABI_VERSION_MAJOR 4
#define ENGINE_ABI_VERSION_MINOR 1
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
