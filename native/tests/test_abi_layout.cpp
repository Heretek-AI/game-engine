// test_abi_layout.cpp — M1-04 acceptance: pins the C99 ABI contract.
// (M1-05 extension: the benchmark world + Velocity mirror pins.)
//
// 1. engine_get_api_table() is exported from engine_core and non-NULL.
// 2. Table size/version/stride self-describe correctly.
// 3. Struct sizes/offsets match the codegen contract (EcsChunkView.count@24,
//    64B components, 16B EngineVelocity, 48B chunk view, table layout).
// 4. Lifecycle round-trip: create -> bulk_create(4096) -> register a native
//    counting callback -> run_systems sums every chunk count == 4096 rows ->
//    propagate -> destroy. Exercises the same path the C# host will use.
//    (M1-05: the benchmark world bulk-creates the (Local, World, Velocity)
//    archetype, so the counting callback fires over the benchmark chunks;
//    run_systems no longer propagates — propagation is covered explicitly
//    by world_propagate below.)
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/bridge/engine_abi.h"
#include "engine/ecs/transform.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

std::uint64_t g_seen_rows = 0;
int32_t g_calls = 0;
float g_seen_dt = 0.0F;

int32_t g_pass_calls = 0;

// OOB child probe (split helper: keeps the editor section within the
// nesting budget).
auto CheckChildOob(const EngineApiTable* api, EngineEditorSession* ed, uint64_t root) -> int {
  uint64_t got = 0;
  return check(api->editor_get_child(ed, root, 1, &got) != 0, "editor: child OOB rejected");
}

// In-place edit probe (split helper: keeps the editor section within
// the nesting budget).
auto CheckInPlaceEdit(const EngineApiTable* api, EngineEditorSession* ed, uint64_t root) -> int {
  std::array<float, 10> back{};
  const int rc = api->editor_get_local(ed, root, back.data());
  if (rc != 0) {
    return check(false, "editor: in-place edit visible");
  }
  return check(back[0] == 9.0F, "editor: in-place edit visible");
}

// Set/get round-trip probe (split helper: keeps the editor section within
// the nesting budget).
auto CheckPoseRoundTrip(const EngineApiTable* api, EngineEditorSession* ed, uint64_t root) -> int {
  std::array<float, 10> back{};
  const int rc = api->editor_get_local(ed, root, back.data());
  if (rc != 0) {
    return check(false, "editor: get local round-trip");
  }
  return check(back[0] == 1.0F && back[2] == 3.0F, "editor: get local round-trip");
}

// Post-restore pose probe (split helper: keeps the editor section within
// the nesting budget).
auto CheckRestorePose(const EngineApiTable* api, EngineEditorSession* ed, uint64_t root) -> int {
  std::array<float, 10> back{};
  const int rc = api->editor_get_local(ed, root, back.data());
  if (rc != 0) {
    return check(false, "editor: restore replays pre-PIE pose");
  }
  return check(back[0] == 9.0F, "editor: restore replays pre-PIE pose");
}

// Hierarchy walk probe: spawn root + child, reparent, walk, prefab flag,
// cycle guard (reparenting root under its descendant must fail).
auto CheckEditorHierarchy(const EngineApiTable* api, EngineEditorSession* ed) -> int {
  int failures = 0;
  uint64_t root = 0;
  uint64_t child = 0;
  failures += check(api->editor_spawn(ed, "Root", &root) == 0 && root != 0, "editor: spawn root");
  failures += check(api->editor_spawn(ed, "Child", &child) == 0 && child != 0, "editor: spawn child");
  failures += check(api->editor_set_parent(ed, child, root) == 0, "editor: reparent");
  uint64_t nc = 0;
  failures += check(api->editor_count_children(ed, root, &nc) == 0 && nc == 1, "editor: one child");
  uint64_t got = 0;
  failures += check(api->editor_get_child(ed, root, 0, &got) == 0 && got == child, "editor: child id");
  failures += CheckChildOob(api, ed, root);
  uint64_t nr = 0;
  failures += check(api->editor_count_roots(ed, &nr) == 0 && nr >= 1, "editor: roots >= 1");
  uint64_t r0 = 0;
  failures += check(api->editor_get_root(ed, 0, &r0) == 0 && r0 != 0, "editor: get root");
  failures += check(api->editor_is_prefab_instance(ed, root) == 0, "editor: not a prefab");
  failures += check(api->editor_is_prefab_instance(ed, 999999001) < 0, "editor: unknown entity rejected");
  failures += check(api->editor_set_parent(ed, root, child) == -4, "editor: cycle reparent rejected");
  // Duplicate-name policy: second spawn with the same name allocates a
  // FRESH entity (never resolves to the existing row).
  uint64_t dup = 0;
  failures += check(api->editor_spawn(ed, "Root", &dup) == 0 && dup != 0 && dup != root, "editor: duplicate name spawns fresh entity");
  return failures;
}

// Pose edit probe: set/get round-trip + in-place edit burst.
auto CheckEditorPose(const EngineApiTable* api, EngineEditorSession* ed) -> int {
  int failures = 0;
  uint64_t nr = 0;
  failures += check(api->editor_count_roots(ed, &nr) == 0 && nr > 0, "editor: pose needs a root");
  uint64_t root = 0;
  failures += check(api->editor_get_root(ed, 0, &root) == 0 && root != 0, "editor: pose root id");
  if (failures != 0) {
    return failures;
  }
  const std::array<float, 10> pose{1, 2, 3, 0, 0, 0, 1, 1, 1, 1};
  failures += check(api->editor_set_local(ed, root, pose.data()) == 0, "editor: set local");
  failures += CheckPoseRoundTrip(api, ed, root);
  auto* slot = api->editor_begin_edit(ed, root);
  failures += check(slot != nullptr, "editor: begin edit");
  if (slot != nullptr) {
    slot->px = 9.0F;
    failures += check(api->editor_end_edit(ed) == 0, "editor: end edit");
  }
  failures += CheckInPlaceEdit(api, ed, root);
  return failures;
}

// PIE probe: snapshot, mutate, restore, discard (idempotent on NULL).
// Covers the recycled-generation edge: delete a snapshot entity, spawn a
// replacement (index reuse, newer generation), then restore — extras must
// die first so make_alive can resurrect the original id.
auto CheckEditorPie(const EngineApiTable* api, EngineEditorSession* ed) -> int {
  int failures = 0;
  uint64_t root = 0;
  failures += check(api->editor_get_root(ed, 0, &root) == 0 && root != 0, "editor: PIE root id");
  if (failures != 0) {
    return failures;
  }
  EngineEditorSnapshot* snap = api->editor_snapshot_take(ed);
  failures += check(snap != nullptr, "editor: snapshot take");
  const std::array<float, 10> moved{50, 50, 50, 0, 0, 0, 1, 2, 2, 2};
  failures += check(api->editor_set_local(ed, root, moved.data()) == 0, "editor: PIE mutate");
  failures += check(api->editor_snapshot_restore(ed, snap) == 0, "editor: snapshot restore");
  failures += CheckRestorePose(api, ed, root);
  api->editor_snapshot_discard(snap);
  api->editor_snapshot_discard(nullptr);  // idempotent on NULL
  return failures;
}

int32_t RenderPassCountingCb(uint32_t pass_index, void* /*user_data*/) {
  (void)pass_index;
  ++g_pass_calls;
  return 0;
}

int32_t RenderPassFailingCb(uint32_t /*pass_index*/, void* /*user_data*/) {
  return 7;
}

void CountingCallback(const EcsChunkView* view, float dt) {
  g_seen_dt += dt;
  if (view == nullptr || view->local == nullptr) {
    return;
  }
  g_calls += 1;
  g_seen_rows += static_cast<std::uint64_t>(view->count);
}

}  // namespace

auto main() -> int {
  int failures = 0;

  // --- 1. Entry point + self-description ------------------------------------
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr, "abi: engine_get_api_table non-NULL");
  if (api == nullptr) {
    return 1;
  }
  failures += check(api->size == sizeof(EngineApiTable), "abi: table size self-describes");
  failures += check(api->abi_version_major == ENGINE_ABI_VERSION_MAJOR, "abi: major version");
  failures += check(api->abi_version_minor == ENGINE_ABI_VERSION_MINOR, "abi: minor version");
  failures += check(api->abi_version_patch == ENGINE_ABI_VERSION_PATCH, "abi: patch version");
  failures += check(api->column_stride == ENGINE_ABI_COLUMN_STRIDE, "abi: column stride 64");
  failures += check(api->column_stride == 64, "abi: stride literal 64");
  failures += check(api->world_create != nullptr, "abi: world_create set");
  failures += check(api->world_destroy != nullptr, "abi: world_destroy set");
  failures += check(api->world_bulk_create != nullptr, "abi: world_bulk_create set");
  failures += check(api->world_propagate != nullptr, "abi: world_propagate set");
  failures += check(api->register_system != nullptr, "abi: register_system set");
  failures += check(api->run_systems != nullptr, "abi: run_systems set");
  failures += check(api->renderpass_create != nullptr, "abi: renderpass_create set");
  failures += check(api->renderpass_destroy != nullptr, "abi: renderpass_destroy set");
  failures += check(api->renderpass_add_image != nullptr, "abi: renderpass_add_image set");
  failures += check(api->renderpass_add_pass != nullptr, "abi: renderpass_add_pass set");
  failures += check(api->renderpass_compile != nullptr, "abi: renderpass_compile set");
  failures += check(api->renderpass_run_frame != nullptr, "abi: renderpass_run_frame set");
  failures += check(api->renderpass_pass_result != nullptr, "abi: renderpass_pass_result set");
  failures += check(api->renderpass_failed_count != nullptr, "abi: renderpass_failed_count set");
  failures += check(api->physics_raycast != nullptr, "abi: physics_raycast set");
  failures += check(api->physics_sphere_cast != nullptr, "abi: physics_sphere_cast set");
  failures += check(api->physics_overlap_sphere != nullptr, "abi: physics_overlap_sphere set");
  failures += check(api->physics_raycast_batch != nullptr, "abi: physics_raycast_batch set");
  failures += check(api->physics2d_create != nullptr, "abi: physics2d_create set");
  failures += check(api->physics2d_destroy != nullptr, "abi: physics2d_destroy set");
  failures += check(api->physics2d_body_box != nullptr, "abi: physics2d_body_box set");
  failures += check(api->physics2d_body_circle != nullptr, "abi: physics2d_body_circle set");
  failures += check(api->physics2d_body_polygon != nullptr, "abi: physics2d_body_polygon set");
  failures += check(api->physics2d_body_destroy != nullptr, "abi: physics2d_body_destroy set");
  failures += check(api->physics2d_step != nullptr, "abi: physics2d_step set");
  failures += check(api->physics2d_drain != nullptr, "abi: physics2d_drain set");
  failures += check(api->audio_create != nullptr, "abi: audio_create set");
  failures += check(api->audio_destroy != nullptr, "abi: audio_destroy set");
  failures += check(api->audio_set_bus_volume_db != nullptr, "abi: audio_set_bus_volume_db set");
  failures += check(api->audio_get_bus_volume_db != nullptr, "abi: audio_get_bus_volume_db set");
  failures += check(api->audio_set_lpf_cutoff != nullptr, "abi: audio_set_lpf_cutoff set");
  failures += check(api->audio_set_lpf_enabled != nullptr, "abi: audio_set_lpf_enabled set");
  failures += check(api->audio_set_delay != nullptr, "abi: audio_set_delay set");
  failures += check(api->audio_set_delay_enabled != nullptr, "abi: audio_set_delay_enabled set");
  failures += check(api->audio_render_sine_db != nullptr, "abi: audio_render_sine_db set");
  failures += check(api->ui_create != nullptr, "abi: ui_create set");
  failures += check(api->ui_destroy != nullptr, "abi: ui_destroy set");
  failures += check(api->ui_load != nullptr, "abi: ui_load set");
  failures += check(api->ui_set_text != nullptr, "abi: ui_set_text set");
  failures += check(api->ui_get_text != nullptr, "abi: ui_get_text set");
  failures += check(api->ui_has_element != nullptr, "abi: ui_has_element set");
  failures += check(api->ui_inject_click != nullptr, "abi: ui_inject_click set");
  failures += check(api->ui_render != nullptr, "abi: ui_render set");
  failures += check(api->ui_copy_frame != nullptr, "abi: ui_copy_frame set");
  failures += check(api->ui_click_count != nullptr, "abi: ui_click_count set");
  failures += check(api->ui_consume_clicks != nullptr, "abi: ui_consume_clicks set");

  // --- 2. Layout pins ---------------------------------------------------------
  failures += check(sizeof(EngineLocalTransform) == 64, "abi: EngineLocalTransform 64B");
  failures += check(alignof(EngineLocalTransform) == 64, "abi: EngineLocalTransform align 64");
  failures += check(sizeof(engine::ecs::LocalTransform) == sizeof(EngineLocalTransform),
                    "abi: LocalTransform matches ecs mirror");
  failures += check(sizeof(EngineWorldTransform) == 64, "abi: EngineWorldTransform 64B");
  failures += check(sizeof(engine::ecs::WorldTransform) == sizeof(EngineWorldTransform),
                    "abi: WorldTransform matches ecs mirror");
  failures += check(sizeof(EcsChunkView) == 48, "abi: EcsChunkView 48B");
  failures += check(offsetof(EcsChunkView, count) == 24, "abi: EcsChunkView.count @ 24");
  failures += check(offsetof(EcsChunkView, local) == 0, "abi: EcsChunkView.local @ 0");
  failures += check(offsetof(EcsChunkView, world) == 8, "abi: EcsChunkView.world @ 8");
  failures += check(offsetof(EcsChunkView, entities) == 16, "abi: EcsChunkView.entities @ 16");
  failures += check(offsetof(EcsChunkView, user_data) == 40, "abi: EcsChunkView.user_data @ 40");
  failures += check(sizeof(EngineApiTable) == 928, "abi: EngineApiTable 928B (ABI 4.13 game UI tail)");
  failures += check(sizeof(EngineInputState) == 1668, "abi: EngineInputState 1668B");
  failures += check(offsetof(EngineInputState, mouse_x) == 1536, "abi: EngineInputState.mouse_x @ 1536");
  failures += check(offsetof(EngineInputState, mouse_buttons) == 1552, "abi: EngineInputState.mouse_buttons @ 1552");
  failures += check(offsetof(EngineInputState, pad_axes) == 1556, "abi: EngineInputState.pad_axes @ 1556");
  failures += check(offsetof(EngineInputState, pad_buttons) == 1652, "abi: EngineInputState.pad_buttons @ 1652");
  failures += check(offsetof(EngineApiTable, input_get_state) == 360, "abi: input_get_state @ 360 (P2-02 tail)");
  failures += check(api->input_get_state != nullptr, "abi: input_get_state set");
  failures += check(api->input_get_state(nullptr) == nullptr, "abi: input_get_state null renderer -> null");
  // P1-09 renderer tail (ABI 4.4): 5 appended entries, all earlier frozen.
  failures += check(api->renderer_create != nullptr, "abi: renderer_create set");
  failures += check(api->renderer_destroy != nullptr, "abi: renderer_destroy set");
  failures += check(api->renderer_frame != nullptr, "abi: renderer_frame set");
  failures += check(api->renderer_mesh_create != nullptr, "abi: renderer_mesh_create set");
  failures += check(api->renderer_screenshot != nullptr, "abi: renderer_screenshot set");
  failures += check(offsetof(EngineApiTable, renderer_create) == 368,
                    "abi: renderer_create @ 368 (4.4 tail)");
  failures += check(offsetof(EngineApiTable, renderer_screenshot) == 400,
                    "abi: renderer_screenshot @ 400 (4.4 tail)");
  // P6-03 navigation tail (ABI 4.5): 8 appended entries, all earlier frozen.
  failures += check(offsetof(EngineApiTable, nav_create) == 408,
                    "abi: nav_create @ 408 (4.5 tail)");
  failures += check(offsetof(EngineApiTable, nav_agent_reached) == 464,
                    "abi: nav_agent_reached @ 464 (4.5 tail end)");
  // P3-02/P3-06 offscreen + picking tail (ABI 4.6): 3 appended entries.
  failures += check(api->renderer_create_offscreen != nullptr,
                    "abi: renderer_create_offscreen set");
  failures += check(api->renderer_render_to_buffer != nullptr,
                    "abi: renderer_render_to_buffer set");
  failures += check(api->renderer_pick != nullptr, "abi: renderer_pick set");
  failures += check(offsetof(EngineApiTable, renderer_create_offscreen) == 472,
                    "abi: renderer_create_offscreen @ 472 (4.6 tail)");
  failures += check(offsetof(EngineApiTable, renderer_render_to_buffer) == 480,
                    "abi: renderer_render_to_buffer @ 480 (4.6 tail)");
  failures += check(offsetof(EngineApiTable, renderer_pick) == 488,
                    "abi: renderer_pick @ 488 (4.6 tail)");
  // P3-08 project-drop tail (ABI 4.7): 2 appended entries, all earlier frozen.
  failures += check(api->project_drop_asset != nullptr, "abi: project_drop_asset set");
  failures += check(api->editor_delete_entity != nullptr, "abi: editor_delete_entity set");
  failures += check(offsetof(EngineApiTable, project_drop_asset) == 496,
                    "abi: project_drop_asset @ 496 (4.7 tail)");
  failures += check(offsetof(EngineApiTable, editor_delete_entity) == 504,
                    "abi: editor_delete_entity @ 504 (4.7 tail)");
  // P3-09 console/profiler tail (ABI 4.8): 12 appended entries, all earlier frozen.
  failures += check(api->log_write != nullptr, "abi: log_write set");
  failures += check(api->log_set_sink != nullptr, "abi: log_set_sink set");
  failures += check(api->log_read != nullptr, "abi: log_read set");
  failures += check(api->log_count != nullptr, "abi: log_count set");
  failures += check(api->log_clear != nullptr, "abi: log_clear set");
  failures += check(api->profile_reset != nullptr, "abi: profile_reset set");
  failures += check(api->profile_frame != nullptr, "abi: profile_frame set");
  failures += check(api->profile_count != nullptr, "abi: profile_count set");
  failures += check(api->profile_frame_time != nullptr, "abi: profile_frame_time set");
  failures += check(api->profile_system_time != nullptr, "abi: profile_system_time set");
  failures += check(api->profile_system_count != nullptr, "abi: profile_system_count set");
  failures += check(api->renderer_gpu_time != nullptr, "abi: renderer_gpu_time set");
  failures += check(offsetof(EngineApiTable, log_write) == 512,
                    "abi: log_write @ 512 (4.8 tail)");
  failures += check(offsetof(EngineApiTable, renderer_gpu_time) == 600,
                    "abi: renderer_gpu_time @ 600 (4.8 tail)");
  // P6-07 2D physics tail (ABI 4.9): 8 appended entries after the 4.8 tail.
  failures += check(offsetof(EngineApiTable, physics2d_create) == 608,
                    "abi: physics2d_create @ 608 (4.9 tail)");
  failures += check(offsetof(EngineApiTable, physics2d_drain) == 664,
                    "abi: physics2d_drain @ 664 (4.9 tail end)");
  // P5-06 audio mixer tail (ABI 4.11): 9 appended entries after the 4.10
  // hierarchy tail (editor_snapshot_restore_subtree @ 704).
  failures += check(offsetof(EngineApiTable, audio_create) == 712,
                    "abi: audio_create @ 712 (4.11 tail)");
  failures += check(offsetof(EngineApiTable, audio_render_sine_db) == 776,
                    "abi: audio_render_sine_db @ 776 (4.11 tail end)");
  // P6-04 character-controller tail (ABI 4.12): 7 appended entries after
  // the 4.11 audio mixer tail (audio_render_sine_db @ 776).
  failures += check(sizeof(EngineCharacterConfig) == 24,
                    "abi: EngineCharacterConfig 24B");
  failures += check(sizeof(EngineCharacterState) == 24,
                    "abi: EngineCharacterState 24B");
  failures += check(offsetof(EngineCharacterState, grounded) == 12,
                    "abi: EngineCharacterState.grounded @ 12");
  failures += check(offsetof(EngineCharacterState, ground_y) == 16,
                    "abi: EngineCharacterState.ground_y @ 16");
  failures += check(offsetof(EngineCharacterState, vy) == 20,
                    "abi: EngineCharacterState.vy @ 20");
  failures += check(api->character_create != nullptr,
                    "abi: character_create set");
  failures += check(api->character_destroy != nullptr,
                    "abi: character_destroy set");
  failures += check(api->character_move != nullptr, "abi: character_move set");
  failures += check(api->character_teleport != nullptr,
                    "abi: character_teleport set");
  failures += check(api->character_set_slope != nullptr,
                    "abi: character_set_slope set");
  failures += check(api->character_set_step != nullptr,
                    "abi: character_set_step set");
  failures += check(api->character_get_pos != nullptr,
                    "abi: character_get_pos set");
  failures += check(offsetof(EngineApiTable, character_create) == 784,
                    "abi: character_create @ 784 (4.12 tail)");
  failures += check(offsetof(EngineApiTable, character_get_pos) == 832,
  // P5-04 game UI tail (ABI 4.13): 11 appended entries after the 4.12
  // character tail (character_get_pos @ 832).
  failures += check(offsetof(EngineApiTable, ui_create) == 840,
                    "abi: ui_create @ 840 (4.13 tail)");
  failures += check(offsetof(EngineApiTable, ui_consume_clicks) == 920,
                    "abi: ui_consume_clicks @ 920 (4.13 tail end)");
                    "abi: character_get_pos @ 832 (4.12 tail end)");
  failures += check(sizeof(EngineCollisionEvent2D) == 32, "abi: EngineCollisionEvent2D 32B");
  failures += check(offsetof(EngineCollisionEvent2D, point_x) == 16,
                    "abi: EngineCollisionEvent2D.point_x @ 16");
  failures += check(offsetof(EngineCollisionEvent2D, kind) == 28,
                    "abi: EngineCollisionEvent2D.kind @ 28");
  failures += check(sizeof(EngineRigidbody2D) == 24, "abi: EngineRigidbody2D 24B");
  failures += check(sizeof(EngineBoxCollider2D) == 24, "abi: EngineBoxCollider2D 24B");
  failures += check(sizeof(EngineCircleCollider2D) == 28, "abi: EngineCircleCollider2D 28B");
  failures += check(sizeof(EnginePolygonCollider2D) == 64, "abi: EnginePolygonCollider2D 64B");
  failures += check(offsetof(EngineApiTable, run_systems) == 64,
                    "abi: run_systems @ 64 (M1/M2 head frozen)");
  failures += check(offsetof(EngineApiTable, renderpass_create) == 72,
                    "abi: renderpass_create @ 72 (M3-03 tail)");
  failures += check(offsetof(EngineApiTable, physics_raycast) == 136,
                    "abi: physics_raycast @ 136 (M4-03 tail)");
  failures += check(sizeof(EngineRay) == 24, "abi: EngineRay 24B");
  failures += check(sizeof(EngineHit) == 48, "abi: EngineHit 48B");
  failures += check(offsetof(EngineHit, fraction) == 12, "abi: EngineHit.fraction @ 12");
  failures += check(offsetof(EngineHit, user_data) == 16, "abi: EngineHit.user_data @ 16");
  failures += check(offsetof(EngineHit, hit) == 40, "abi: EngineHit.hit @ 40");
  failures += check(api->editor_create != nullptr, "abi: editor_create set");
  failures += check(api->editor_destroy != nullptr, "abi: editor_destroy set");
  failures += check(api->editor_spawn != nullptr, "abi: editor_spawn set");
  failures += check(api->editor_set_name != nullptr, "abi: editor_set_name set");
  failures += check(api->editor_set_parent != nullptr, "abi: editor_set_parent set");
  failures += check(api->editor_snapshot_take != nullptr, "abi: editor_snapshot_take set");
  failures += check(api->editor_snapshot_restore != nullptr, "abi: editor_snapshot_restore set");
  failures += check(api->editor_snapshot_discard != nullptr, "abi: editor_snapshot_discard set");
  failures += check(api->editor_destroy_entity != nullptr, "abi: editor_destroy_entity set");
  failures += check(api->editor_get_name != nullptr, "abi: editor_get_name set");
  failures += check(api->editor_duplicate != nullptr, "abi: editor_duplicate set");
  failures += check(api->editor_snapshot_take_subtree != nullptr, "abi: editor_snapshot_take_subtree set");
  failures += check(api->editor_snapshot_restore_subtree != nullptr, "abi: editor_snapshot_restore_subtree set");
  failures += check(offsetof(EngineApiTable, systems_run) == 352,
                    "abi: systems_run @ 352 (4.2 tail end)");
  // P3-03 hierarchy tail (ABI 4.10): 5 appended entries after the 4.9 tail.
  failures += check(offsetof(EngineApiTable, editor_destroy_entity) == 672,
                    "abi: editor_destroy_entity @ 672 (4.10 tail)");
  failures += check(offsetof(EngineApiTable, editor_snapshot_restore_subtree) == 704,
                    "abi: editor_snapshot_restore_subtree @ 704 (4.10 tail end)");
  failures += check(offsetof(EngineApiTable, editor_create) == 168,
                    "abi: editor_create @ 168 (M5-02 tail)");
  failures += check(api->nav_create != nullptr, "abi: nav_create set");
  failures += check(api->nav_destroy != nullptr, "abi: nav_destroy set");
  failures += check(api->nav_bake != nullptr, "abi: nav_bake set");
  failures += check(api->nav_agent_add != nullptr, "abi: nav_agent_add set");
  failures += check(api->nav_agent_goto != nullptr, "abi: nav_agent_goto set");
  failures += check(api->nav_agent_update != nullptr, "abi: nav_agent_update set");
  failures += check(api->nav_agent_pos != nullptr, "abi: nav_agent_pos set");
  failures += check(api->nav_agent_reached != nullptr, "abi: nav_agent_reached set");
  failures += check(sizeof(EngineNavBox) == 24, "abi: EngineNavBox 24B");
  failures += check(sizeof(EngineNavBakeConfig) == 48, "abi: EngineNavBakeConfig 48B");
  failures += check(sizeof(EngineNavStats) == 8, "abi: EngineNavStats 8B");
  failures += check(sizeof(EngineNavAgent) == 64, "abi: EngineNavAgent 64B");
  failures += check(alignof(EngineNavAgent) == 64, "abi: EngineNavAgent align 64");
  failures += check(sizeof(FrameDispatch) == 4, "abi: FrameDispatch 4B");
  failures += check(offsetof(FrameDispatch, dt) == 0, "abi: FrameDispatch.dt @ 0");
  failures += check(sizeof(EngineVelocity) == 16, "abi: EngineVelocity 16B");
  failures += check(alignof(EngineVelocity) == 16, "abi: EngineVelocity align 16");

  // --- 3. Lifecycle round-trip ------------------------------------------------
  EngineWorld* world = api->world_create();
  failures += check(world != nullptr, "abi: world_create");
  if (world == nullptr) {
    return 1;
  }
  constexpr std::uint64_t kRows = 4096;
  const std::uint64_t created = api->world_bulk_create(world, kRows);
  failures += check(created == kRows, "abi: bulk_create 4096");
  failures += check(api->register_system(world, "counter", &CountingCallback, nullptr) == 0,
                    "abi: register_system ok");
  failures += check(api->register_system(world, "null-cb", nullptr, nullptr) != 0,
                    "abi: null callback rejected");
  const FrameDispatch frame_dt{1.0F / 60.0F};
  failures += check(api->run_systems(world, &frame_dt) == 0, "abi: run_systems ok");
  failures += check(g_calls >= 1, "abi: callback invoked");
  failures += check(g_seen_rows == kRows, "abi: chunk counts sum to 4096");
  failures += check(g_seen_dt > 0.0F, "abi: callbacks received nonzero dt");
  failures += check(api->world_propagate(world) == 0, "abi: propagate ok");
  g_seen_dt = 0.0F;
  failures += check(api->run_systems(world, nullptr) == 0, "abi: run_systems null dispatch ok (zero dt)");
  failures += check(g_seen_dt == 0.0F, "abi: null dispatch delivered zero dt");
  failures += check(api->run_systems(nullptr, nullptr) != 0, "abi: run_systems null world rejected");
  failures += check(api->world_bulk_create(nullptr, 8) == 0, "abi: bulk_create null world -> 0");
  api->world_destroy(world);
  api->world_destroy(nullptr);  // idempotent on NULL

  // --- 3b. Console + profiler round-trip (P3-09 ABI 4.8 tail) ---------------
  EngineWorld* cworld = api->world_create();
  failures += check(cworld != nullptr, "console: world_create");
  if (cworld != nullptr) {
    failures += check(api->log_count(cworld) == 0, "console: starts empty");
    failures += check(api->log_write(0, "hello") == 0, "console: log_write ok");
    failures += check(api->log_count(cworld) == 1, "console: count == 1");
    char buf[64];
    int32_t lvl = -1;
    failures += check(api->log_read(cworld, 0, &lvl, buf, sizeof(buf)) == 0, "console: log_read ok");
    failures += check(lvl == 0 && std::strcmp(buf, "hello") == 0, "console: text matches");
    failures += check(api->log_write(9, "bad") != 0, "console: bad level rejected");
    failures += check(api->log_write(0, nullptr) != 0, "console: null msg rejected");
    api->log_clear(cworld);
    failures += check(api->log_count(cworld) == 0, "console: clear empties");
    failures += check(api->profile_count(cworld) == 0, "profiler: starts empty");
    failures += check(api->profile_frame(cworld, 16.6F) == 0, "profiler: record ok");
    failures += check(api->profile_count(cworld) == 1, "profiler: count == 1");
    const float ft = api->profile_frame_time(cworld, 0);
    failures += check(ft > 16.5F && ft < 16.7F, "profiler: frame time matches");
    failures += check(api->profile_frame_time(cworld, 99) == -1.0F, "profiler: OOB frame -> -1");
    api->profile_reset(cworld);
    failures += check(api->profile_count(cworld) == 0, "profiler: reset empties");
    api->world_destroy(cworld);
    failures += check(api->log_write(0, "after-destroy") == 0, "console: ring is process-global");
  }

  // --- 4. RenderPass registry round-trip (M3-03 GraphicsCompositor) --------
  EngineRenderPassRegistry* reg = api->renderpass_create();
  failures += check(reg != nullptr, "renderpass: create");
  if (reg != nullptr) {
    failures += check(api->renderpass_add_image(reg, "visibility", 1280, 720, 1) == 0,
                      "renderpass: add visibility image");
    failures += check(api->renderpass_add_image(reg, "hdr", 1280, 720, 1) == 0,
                      "renderpass: add hdr image");
    failures += check(api->renderpass_add_image(reg, "visibility", 64, 64, 1) != 0,
                      "renderpass: dup image rejected");
    const char* vig_reads[1] = {"visibility"};
    failures += check(api->renderpass_add_pass(reg, "vignette", vig_reads, 1, vig_reads, 1,
                                              &RenderPassCountingCb, nullptr) == 0,
                      "renderpass: add custom pass");
    failures += check(api->renderpass_add_pass(reg, "vignette", nullptr, 0, nullptr, 0,
                                              &RenderPassCountingCb, nullptr) != 0,
                      "renderpass: dup pass rejected");
    failures += check(api->renderpass_add_pass(reg, "nullcb", nullptr, 0, nullptr, 0,
                                              nullptr, nullptr) != 0,
                      "renderpass: null callback rejected");
    failures += check(api->renderpass_run_frame(reg) != 0,
                      "renderpass: run before compile rejected");
    failures += check(api->renderpass_compile(reg) == 0, "renderpass: compile ok");
    failures += check(api->renderpass_run_frame(reg) == 0, "renderpass: run_frame ok");
    failures += check(g_pass_calls == 1, "renderpass: custom callback invoked once");
    failures += check(api->renderpass_pass_result(reg, 0) == 0,
                      "renderpass: pass result ok");
    failures += check(api->renderpass_pass_result(reg, 99) == -1,
                      "renderpass: pass result OOB -> -1");
    failures += check(api->renderpass_failed_count(reg) == 0,
                      "renderpass: no failures");
    // Fault isolation: a failing pass reports but does not stop the frame.
    const char* hdr_w[1] = {"hdr"};
    failures += check(api->renderpass_add_pass(reg, "grain", hdr_w, 1, hdr_w, 1,
                                              &RenderPassFailingCb, nullptr) == 0,
                      "renderpass: add failing pass");
    failures += check(api->renderpass_compile(reg) == 0, "renderpass: recompile ok");
    failures += check(api->renderpass_run_frame(reg) == 0,
                      "renderpass: run with failing pass ok");
    failures += check(api->renderpass_failed_count(reg) == 1,
                      "renderpass: one failure isolated");
    failures += check(api->renderpass_pass_result(reg, 1) == 7,
                      "renderpass: failing rc recorded");
    api->renderpass_destroy(reg);
    api->renderpass_destroy(nullptr);  // idempotent on NULL

// --- 5. Editor session round-trip (M5-02 hierarchy + edits + PIE) --------
  EngineWorld* eworld = api->world_create();
  failures += check(eworld != nullptr, "editor: world_create");
  if (eworld != nullptr) {
    EngineEditorSession* ed = api->editor_create(eworld);
    failures += check(ed != nullptr, "editor: session create");
    failures += check(api->editor_create(nullptr) == nullptr, "editor: null world -> null");
    if (ed != nullptr) {
      failures += CheckEditorHierarchy(api, ed);
      failures += CheckEditorPose(api, ed);
      failures += CheckEditorPie(api, ed);
      api->editor_destroy(ed);
    }
    api->world_destroy(eworld);
    api->editor_destroy(nullptr);  // idempotent on NULL
  }
    failures += check(api->renderpass_run_frame(nullptr) != 0,
                      "renderpass: null registry rejected");
    failures += check(api->renderpass_failed_count(nullptr) == -1,
                      "renderpass: null failed_count -> -1");
  }

  std::printf(failures == 0 ? "test_abi_layout: all checks passed\n"
                            : "test_abi_layout: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
