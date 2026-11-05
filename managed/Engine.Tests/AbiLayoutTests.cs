// M1-04 layout gate: asserts the GENERATED bindings (NativeApi.g.cs) match
// the C99 ABI contract (engine_abi.h) without loading the native library.
// Runs everywhere (Windows/Linux/macOS CI) with zero native dependency:
// struct sizes, field offsets, version constants, and that every ApiTable
// entry uses selective GC-transition suppression: only register_system
// (a guaranteed non-blocking store) is SuppressGCTransition; run,
// propagate, and lifecycle calls are plain Cdecl because they may block.
using System.Reflection;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Tests;

public static class AbiLayoutTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(NativeApi.AbiVersionMajor == 4, "abi major == 4");
        failures += Check(NativeApi.AbiVersionMinor == 25, "abi minor == 25 (4.25: P3-05 inspector entity_remove tail after 4.24 field-metadata)");
        failures += Check(NativeApi.AbiVersionPatch == 0, "abi patch == 0");
        failures += Check(NativeApi.ColumnStride == 64, "column stride == 64");

        unsafe
        {
            failures += Check(sizeof(NativeApi.CollisionEvent2D) == 32, "CollisionEvent2D 32B (3D parity)");
            failures += Check(sizeof(NativeApi.CollisionEvent) == 32, "CollisionEvent 32B (P2-05 3D path)");
            failures += Check(sizeof(NativeApi.Rigidbody2D) == 24, "Rigidbody2D 24B");
            failures += Check(sizeof(NativeApi.BoxCollider2D) == 24, "BoxCollider2D 24B");
            failures += Check(sizeof(NativeApi.CircleCollider2D) == 28, "CircleCollider2D 28B");
            failures += Check(sizeof(NativeApi.PolygonCollider2D) == 64, "PolygonCollider2D 64B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EnginePhysics2DCreate").ToInt64() == 608, "physics2d_create @ 608 (ABI 4.9 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EnginePhysics2DDrain").ToInt64() == 664, "physics2d_drain @ 664 (ABI 4.9 tail end)");
            failures += Check(sizeof(NativeApi.EnginePhysics2DWorld) == 8, "EnginePhysics2DWorld opaque 8B");
            failures += Check(sizeof(NativeApi.LocalTransform) == 64, "LocalTransform 64B");
            failures += Check(sizeof(NativeApi.WorldTransform) == 64, "WorldTransform 64B");
            failures += Check(sizeof(NativeApi.EcsChunkView) == 48, "EcsChunkView 48B");
            failures += Check(sizeof(NativeApi.FrameDispatch) == 4, "FrameDispatch 4B");
            failures += Check(sizeof(NativeApi.ApiTable) == 1288, "ApiTable 1288B (ABI 4.25: +1 entity_remove after 4.24 field-metadata tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineEditorDestroyEntity").ToInt64() == 672, "editor_destroy_entity @ 672 (ABI 4.10 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineEditorSnapshotRestoreSubtree").ToInt64() == 704, "snapshot_restore_subtree @ 704 (ABI 4.10 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAudioCreate").ToInt64() == 712, "audio_create @ 712 (ABI 4.11 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAudioRenderSineDb").ToInt64() == 776, "audio_render_sine_db @ 776 (ABI 4.11 tail end)");
            failures += Check(sizeof(NativeApi.CharacterConfig) == 24, "CharacterConfig 24B (P6-04 capsule + limits)");
            failures += Check(sizeof(NativeApi.CharacterState) == 24, "CharacterState 24B (P6-04 feet + grounded + vy)");
            failures += Check(Marshal.OffsetOf<NativeApi.CharacterState>("Grounded").ToInt64() == 12, "CharacterState.Grounded @ 12");
            failures += Check(Marshal.OffsetOf<NativeApi.CharacterState>("GroundY").ToInt64() == 16, "CharacterState.GroundY @ 16");
            failures += Check(Marshal.OffsetOf<NativeApi.CharacterState>("Vy").ToInt64() == 20, "CharacterState.Vy @ 20");
            failures += Check(sizeof(NativeApi.EngineCharacter) == 8, "EngineCharacter opaque 8B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineCharacterCreate").ToInt64() == 784, "character_create @ 784 (ABI 4.12 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineCharacterGetPos").ToInt64() == 832, "character_get_pos @ 832 (ABI 4.12 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineUiCreate").ToInt64() == 880, "ui_create @ 880 (ABI 4.14 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineUiConsumeClicks").ToInt64() == 960, "ui_consume_clicks @ 960 (ABI 4.14 tail end)");
            failures += Check(sizeof(NativeApi.EngineUi) == 8, "EngineUi opaque 8B");
            failures += Check(sizeof(NativeApi.EngineAudioWorld) == 8, "EngineAudioWorld opaque 8B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineSystemSetPhase").ToInt64() == 840, "system_set_phase @ 840 (ABI 4.13 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimeGetU64").ToInt64() == 872, "time_get_u64 @ 872 (ABI 4.13 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EnginePhysicsWorldFor").ToInt64() == 968, "physics_world_for @ 968 (ABI 4.15 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineSceneSave").ToInt64() == 976, "scene_save @ 976 (ABI 4.16 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineSceneLoad").ToInt64() == 984, "scene_load @ 984 (ABI 4.16 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EnginePhysicsDrainEvents").ToInt64() == 992, "physics_drain_events @ 992 (ABI 4.17 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererAttachUi").ToInt64() == 1000, "renderer_attach_ui @ 1000 (ABI 4.19 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelineCreate").ToInt64() == 1008, "timeline_create @ 1008 (ABI 4.20 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelineClipRemove").ToInt64() == 1048, "timeline_clip_remove @ 1048 (ABI 4.20 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelineDuration").ToInt64() == 1056, "timeline_duration @ 1056 (ABI 4.21 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelineEvaluateActive").ToInt64() == 1088, "timeline_evaluate_active @ 1088 (ABI 4.21 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelinePlayerCreate").ToInt64() == 1096, "timeline_player_create @ 1096 (ABI 4.22 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineTimelinePlayerLooping").ToInt64() == 1192, "timeline_player_looping @ 1192 (ABI 4.22 tail end)");
            // P5-03 animator tail (ABI 4.23, appended after 4.22 playback tail).
            failures += Check(sizeof(NativeApi.EngineAnimator) == 8, "EngineAnimator opaque 8B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorCreate").ToInt64() == 1200, "animator_create @ 1200 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorDestroy").ToInt64() == 1208, "animator_destroy @ 1208 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorSetFloat").ToInt64() == 1216, "animator_set_float @ 1216 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorSetBool").ToInt64() == 1224, "animator_set_bool @ 1224 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorSetTrigger").ToInt64() == 1232, "animator_set_trigger @ 1232 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorUpdate").ToInt64() == 1240, "animator_update @ 1240 (ABI 4.23 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineAnimatorStateName").ToInt64() == 1248, "animator_state_name @ 1248 (ABI 4.23 tail end)");
            // P3-04 field-metadata tail (ABI 4.24, appended after 4.23 animator tail).
            failures += Check(sizeof(NativeApi.FieldDesc) == 16, "FieldDesc 16B (P3-04 field row)");
            failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Name").ToInt64() == 0, "FieldDesc.Name @ 0");
            failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Offset").ToInt64() == 8, "FieldDesc.Offset @ 8");
            failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Type").ToInt64() == 12, "FieldDesc.Type @ 12");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineComponentDescribe").ToInt64() == 1256, "component_describe @ 1256 (ABI 4.24 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineComponentFieldCount").ToInt64() == 1264, "component_field_count @ 1264 (ABI 4.24 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineComponentFieldAt").ToInt64() == 1272, "component_field_at @ 1272 (ABI 4.24 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineEntityRemove").ToInt64() == 1280, "entity_remove @ 1280 (ABI 4.25 tail end)");
            failures += Check(sizeof(NativeApi.EngineTimelinePlayer) == 8, "EngineTimelinePlayer opaque 8B");
            failures += Check(sizeof(NativeApi.TimelineActiveClip) == 16, "TimelineActiveClip 16B (P5-07 active row)");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineActiveClip>("Clip").ToInt64() == 4, "TimelineActiveClip.Clip @ 4");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineActiveClip>("Local").ToInt64() == 8, "TimelineActiveClip.Local @ 8");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineActiveClip>("Weight").ToInt64() == 12, "TimelineActiveClip.Weight @ 12");
            failures += Check(sizeof(NativeApi.EngineTimeline) == 8, "EngineTimeline opaque 8B");
            failures += Check(sizeof(NativeApi.TimelineClip) == 32, "TimelineClip 32B (P5-07 clip row)");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineClip>("SourceStart").ToInt64() == 8, "TimelineClip.SourceStart @ 8");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineClip>("TargetEntity").ToInt64() == 16, "TimelineClip.TargetEntity @ 16");
            failures += Check(Marshal.OffsetOf<NativeApi.TimelineClip>("AssetId").ToInt64() == 24, "TimelineClip.AssetId @ 24");
            failures += Check(sizeof(NativeApi.InputState) == 1668, "InputState 1668B (P2-02 snapshot)");
            failures += Check(sizeof(NativeApi.EngineRenderer) == 8, "EngineRenderer opaque 8B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineComponentRegister").ToInt64() == 312, "component_register @ 312 (ABI 4.2 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineSystemsRun").ToInt64() == 352, "systems_run @ 352 (ABI 4.2 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineInputGetState").ToInt64() == 360, "input_get_state @ 360 (ABI 4.3 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererCreate").ToInt64() == 368, "renderer_create @ 368 (ABI 4.4 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererScreenshot").ToInt64() == 400, "renderer_screenshot @ 400 (ABI 4.4 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineNavCreate").ToInt64() == 408, "nav_create @ 408 (ABI 4.5 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineNavAgentReached").ToInt64() == 464, "nav_agent_reached @ 464 (ABI 4.5 tail end)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererCreateOffscreen").ToInt64() == 472, "renderer_create_offscreen @ 472 (ABI 4.6 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererRenderToBuffer").ToInt64() == 480, "renderer_render_to_buffer @ 480 (ABI 4.6 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererPick").ToInt64() == 488, "renderer_pick @ 488 (ABI 4.6 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineProjectDropAsset").ToInt64() == 496, "project_drop_asset @ 496 (ABI 4.7 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineEditorDeleteEntity").ToInt64() == 504, "editor_delete_entity @ 504 (ABI 4.7 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineLogWrite").ToInt64() == 512, "log_write @ 512 (ABI 4.8 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRendererGpuTime").ToInt64() == 600, "renderer_gpu_time @ 600 (ABI 4.8 tail)");
            failures += Check(Marshal.OffsetOf<NativeApi.InputState>("MouseX").ToInt64() == 1536, "InputState.MouseX @ 1536");
            failures += Check(Marshal.OffsetOf<NativeApi.InputState>("MouseButtons").ToInt64() == 1552, "InputState.MouseButtons @ 1552");
            failures += Check(Marshal.OffsetOf<NativeApi.InputState>("PadAxes").ToInt64() == 1556, "InputState.PadAxes @ 1556");
            failures += Check(Marshal.OffsetOf<NativeApi.InputState>("PadButtons").ToInt64() == 1652, "InputState.PadButtons @ 1652");
            failures += Check(sizeof(NativeApi.EngineChunkColumns) == 120, "EngineChunkColumns 120B");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EnginePhysicsRaycast").ToInt64() == 136, "physics_raycast @ 136 (M4 tail frozen)");
            failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineEditorCreate").ToInt64() == 168, "editor_create @ 168 (M5 tail)");
            failures += Check(sizeof(NativeApi.EngineEditorSession) == 8, "EngineEditorSession opaque 8B");
            failures += Check(sizeof(NativeApi.EngineEditorSnapshot) == 8, "EngineEditorSnapshot opaque 8B");
            failures += Check(sizeof(NativeApi.EngineWorld) == 8, "EngineWorld opaque 8B");
            failures += Check(sizeof(NativeApi.EngineRenderPassRegistry) == 8, "EngineRenderPassRegistry opaque 8B");
            failures += Check(sizeof(NativeApi.EnginePhysicsWorld) == 8, "EnginePhysicsWorld opaque 8B");
            failures += Check(sizeof(NativeApi.Ray) == 24, "Ray 24B");
            failures += Check(sizeof(NativeApi.RaycastHit) == 48, "RaycastHit 48B");
            failures += Check(sizeof(Engine.Runtime.Ray) == 24, "Runtime.Ray 24B");
            failures += Check(sizeof(Engine.Runtime.RaycastHit) == 48, "Runtime.RaycastHit 48B");
            failures += Check(sizeof(NativeApi.EngineNavWorld) == 8, "EngineNavWorld opaque 8B");
            failures += Check(sizeof(NativeApi.NavBox) == 24, "NavBox 24B");
            failures += Check(sizeof(NativeApi.NavBakeConfig) == 48, "NavBakeConfig 48B");
            failures += Check(sizeof(NativeApi.NavStats) == 8, "NavStats 8B");
            failures += Check(sizeof(NativeApi.NavAgentData) == 64, "NavAgentData 64B");
            failures += Check(sizeof(Engine.Runtime.AudioSource) == 64, "Runtime.AudioSource 64B (P5-06 mixer bus)");
            failures += Check(sizeof(Engine.Runtime.AudioListener) == 64, "Runtime.AudioListener 64B");
            failures += Check(sizeof(Engine.Runtime.NavAgent) == 64, "Runtime.NavAgent 64B");
        }

        failures += Check(Marshal.OffsetOf<NativeApi.EngineChunkColumns>("Entities").ToInt64() == 96, "columns.Entities @ 96");
        failures += Check(Marshal.OffsetOf<NativeApi.EngineChunkColumns>("Count").ToInt64() == 104, "columns.Count @ 104");
        failures += Check(Marshal.OffsetOf<NativeApi.EngineChunkColumns>("TermCount").ToInt64() == 108, "columns.TermCount @ 108");
        failures += Check(Marshal.OffsetOf<NativeApi.EngineChunkColumns>("UserData").ToInt64() == 112, "columns.UserData @ 112");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Local") == IntPtr.Zero, "chunk.Local @ 0");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("World").ToInt64() == 8, "chunk.World @ 8");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Entities").ToInt64() == 16, "chunk.Entities @ 16");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Count").ToInt64() == 24, "chunk.Count @ 24");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("UserData").ToInt64() == 40, "chunk.UserData @ 40");
        failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRunSystems").ToInt64() == 64, "run_systems @ 64 (M1/M2 head frozen)");
        failures += Check(Marshal.OffsetOf<NativeApi.ApiTable>("EngineRenderPassCreate").ToInt64() == 72, "renderpass_create @ 72 (M3 tail frozen)");

        // Every ApiTable function pointer must carry SuppressGCTransition:
        // grep the generated source for the exact callconv spelling.
        var gcs = Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "Engine.Bindings", "NativeApi.g.cs");
        gcs = Path.GetFullPath(gcs);
        if (!File.Exists(gcs))
        {
            // Published/test-host layouts: fall back to reflection on types.
            gcs = FindGeneratedSource() ?? string.Empty;
        }

        if (gcs.Length != 0 && File.Exists(gcs))
        {
            var src = File.ReadAllText(gcs);
            var suppressed = System.Text.RegularExpressions.Regex.Count(src, @"delegate\* unmanaged\[Cdecl, SuppressGCTransition\]");
            // 3 occurrences: register_system entry + loader call + the doc
            // comment above naming the convention (comments count in grep).
            failures += Check(suppressed == 3, $"3 suppressed spellings: register_system entry + loader + doc mention (got {suppressed})");
            failures += Check(src.Contains("unmanaged[Cdecl, SuppressGCTransition]<EngineWorld*, byte*,", StringComparison.Ordinal), "register_system suppressed");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, FrameDispatch*, int> EngineSystemsRun", StringComparison.Ordinal), "systems_run takes dispatch (may be null)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineRenderer*, InputState*> EngineInputGetState", StringComparison.Ordinal), "input_get_state present, plain Cdecl");
            failures += Check(!src.Contains("SuppressGCTransition]<EngineRenderer*", StringComparison.Ordinal), "input entry NOT suppressed (borrowed pointer)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, int> EngineWorldPropagate", StringComparison.Ordinal), "propagate NOT suppressed (may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EnginePhysicsWorld*, Ray*, RaycastHit*, int> EnginePhysicsRaycast", StringComparison.Ordinal), "physics_raycast present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EnginePhysicsWorld*, Ray*, RaycastHit*, uint, uint> EnginePhysicsRaycastBatch", StringComparison.Ordinal), "physics_raycast_batch present, plain Cdecl");
            failures += Check(!src.Contains("SuppressGCTransition]<EnginePhysicsWorld*", StringComparison.Ordinal), "physics entries NOT suppressed (may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, EnginePhysicsWorld*> EnginePhysicsWorldFor", StringComparison.Ordinal), "physics_world_for present, plain Cdecl (Jolt init may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, int> EngineSceneSave", StringComparison.Ordinal), "scene_save present, plain Cdecl (file IO may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, int> EngineSceneLoad", StringComparison.Ordinal), "scene_load present, plain Cdecl (file IO may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EnginePhysicsWorld*, CollisionEvent*, uint, uint> EnginePhysicsDrainEvents", StringComparison.Ordinal), "physics_drain_events present, plain Cdecl (drain may contend)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, ulong, byte*, EngineAnimator*> EngineAnimatorCreate", StringComparison.Ordinal), "animator_create present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineAnimator*, byte*, float, int> EngineAnimatorSetFloat", StringComparison.Ordinal), "animator_set_float present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineAnimator*, byte*, int, int> EngineAnimatorSetBool", StringComparison.Ordinal), "animator_set_bool present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineAnimator*, byte*, int> EngineAnimatorSetTrigger", StringComparison.Ordinal), "animator_set_trigger present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineAnimator*, float, int> EngineAnimatorUpdate", StringComparison.Ordinal), "animator_update present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineAnimator*, byte*, int, int> EngineAnimatorStateName", StringComparison.Ordinal), "animator_state_name present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, FieldDesc*, uint, int> EngineComponentDescribe", StringComparison.Ordinal), "component_describe present, plain Cdecl (copies strings, may allocate)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, int> EngineComponentFieldCount", StringComparison.Ordinal), "component_field_count present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, uint, FieldDesc*, int> EngineComponentFieldAt", StringComparison.Ordinal), "component_field_at present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineRenderer*, EngineUi*, int> EngineRendererAttachUi", StringComparison.Ordinal), "renderer_attach_ui present, plain Cdecl (frame submit may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineTimeline*> EngineTimelineCreate", StringComparison.Ordinal), "timeline_create present, plain Cdecl");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineTimelinePlayer*, int> EngineTimelinePlayerPlay", StringComparison.Ordinal), "timeline_player_play present, plain Cdecl");
            failures += Check(!src.Contains("SuppressGCTransition]<EngineTimeline", StringComparison.Ordinal), "timeline entries NOT suppressed (may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, EngineEditorSession*> EngineEditorCreate", StringComparison.Ordinal), "editor_create present, plain Cdecl");
            failures += Check(
                src.Contains(
                    "unmanaged[Cdecl]<EngineNavWorld*> EngineNavCreate",
                    StringComparison.Ordinal),
                "nav_create present, plain Cdecl");
            failures += Check(
                src.Contains(
                    "unmanaged[Cdecl]<EngineNavWorld*, float, float, float, int> EngineNavAgentGoto",
                    StringComparison.Ordinal),
                "nav_agent_goto present, plain Cdecl");
            failures += Check(
                !src.Contains(
                    "SuppressGCTransition]<EngineNavWorld*",
                    StringComparison.Ordinal),
                "nav entries NOT suppressed (may block)");
            failures += Check(src.Contains("unmanaged[Cdecl]<EngineWorld*, byte*, ulong*, uint*, uint, delegate* unmanaged[Cdecl]<EngineChunkColumns*, float, void>, void*, int> EngineSystemRegister", StringComparison.Ordinal), "system_register present, plain Cdecl");
            failures += Check(!src.Contains("DllImport", StringComparison.Ordinal), "no DllImport P/Invoke stubs");
        }
        else
        {
            // Reflection fallback: all ApiTable fields must be function pointers.
            foreach (var f in typeof(NativeApi.ApiTable).GetFields())
            {
                if (f.Name is "Size" or "AbiVersionMajor" or "AbiVersionMinor" or "AbiVersionPatch" or "ColumnStride")
                {
                    continue;
                }

                failures += Check(f.FieldType.IsFunctionPointer || f.FieldType.IsPointer, $"ApiTable.{f.Name} is fnptr");
            }
        }

        Console.WriteLine(failures == 0 ? "AbiLayoutTests: all checks passed" : $"AbiLayoutTests: {failures} FAILED");
        return failures;
    }

    private static string? FindGeneratedSource()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 8 && dir != null; i++, dir = dir.Parent)
        {
            var cand = Path.Combine(dir.FullName, "Engine.Bindings", "NativeApi.g.cs");
            if (File.Exists(cand))
            {
                return cand;
            }

            var up = Path.Combine(dir.FullName, "managed", "Engine.Bindings", "NativeApi.g.cs");
            if (File.Exists(up))
            {
                return up;
            }
        }

        return null;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}
