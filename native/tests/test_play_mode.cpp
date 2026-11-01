// test_play_mode.cpp — P3-07 acceptance: play 60 frames with a falling
// sphere, stop, assert the sphere position equals the pre-play value
// exactly.
//
// Play model under test: EnterPlay takes an editor snapshot (the
// checkpoint), the game advances frames (gravity integrates the sphere's
// LocalTransform pose each tick — the same world_propagate game-systems
// path play uses), ExitPlay restores the snapshot and discards it.
// Position equality is bit-exact (==, not epsilon): restore replays the
// captured row bytes, no integration runs on exit.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/editor_bridge.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

// One falling-sphere tick: vy -= g*dt, y += vy*dt. Semi-implicit Euler,
// fixed dt — deterministic bit-for-bit on one machine.
constexpr float kGravity = 9.81F;
constexpr float kDt = 1.0F / 60.0F;

auto TickFall(engine::ecs::LocalTransform& pose, float& vy) noexcept -> void {
  vy -= kGravity * kDt;
  pose.py += vy * kDt;
}

auto PoseOf(ecs_world_t* world, std::uint64_t local_id, ecs_entity_t e)
    -> engine::ecs::LocalTransform* {
  return static_cast<engine::ecs::LocalTransform*>(ecs_get_mut_id(
      world, e, static_cast<ecs_entity_t>(local_id)));
}

}  // namespace

auto main() -> int {
  int failures = 0;

  engine::core::JobScheduler sched;
  failures += check(sched.init(), "play: scheduler init");
  engine::ecs::EcsWorld world;
  world.set_scheduler(&sched);
  failures += check(world.construct(), "play: world construct");
  ecs_world_t* raw = world.raw();
  const auto local = static_cast<ecs_entity_t>(world.local_id());
  const auto worldc = static_cast<ecs_entity_t>(world.world_id());

  // Falling sphere: identity pose at y=5, dropped from rest.
  ecs_entity_t sphere = ecs_new_w_id(raw, local);
  ecs_add_id(raw, sphere, worldc);
  auto* sp = static_cast<engine::ecs::LocalTransform*>(
      ecs_get_mut_id(raw, sphere, local));
  failures += check(sp != nullptr, "play: sphere pose writable");
  sp->px = 0.0F;
  sp->py = 5.0F;
  sp->pz = 0.0F;
  sp->qx = sp->qy = sp->qz = 0.0F;
  sp->qw = 1.0F;
  sp->sx = sp->sy = sp->sz = 1.0F;
  failures += check(world.propagate(), "play: propagate ok");

  // EnterPlay: checkpoint the whole world via the editor session.
  engine::editor::EditorSession session;
  failures += check(session.bind(raw), "play: session binds world");

  // Pre-play pose recorded BEFORE the snapshot (the acceptance anchor).
  std::array<float, 10> pre{};
  failures += check(session.get_local(sphere, pre.data()) == 0,
                    "play: pre-play pose readable");
  failures += check(pre[1] == 5.0F, "play: pre-play y is 5");

  auto* snap = session.snapshot_take();
  failures += check(snap != nullptr, "play: enter takes snapshot");

  // 60 play frames: systems+physics tick (gravity integrates the fall).
  float vy = 0.0F;
  for (int frame = 0; frame < 60; ++frame) {
    if (auto* pose = PoseOf(raw, world.local_id(), sphere); pose != nullptr) {
      TickFall(*pose, vy);
    } else {
      failures += check(false, "play: pose alive every frame");
      break;
    }
  }
  failures += check(world.propagate(), "play: propagate after 60 frames");

  std::array<float, 10> during{};
  failures += check(session.get_local(sphere, during.data()) == 0,
                    "play: during-play pose readable");
  failures += check(during[1] < 5.0F, "play: sphere fell during play");

  // Stop: restore the checkpoint, discard it.
  failures += check(snap != nullptr, "play: snapshot live at stop");
  if (snap != nullptr) {
    failures += check(session.snapshot_restore(snap) == 0,
                      "play: exit restores snapshot");
    delete snap;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) test-owned checkpoint
  }
  failures += check(world.propagate(), "play: propagate after restore");

  // Acceptance: position equals the pre-play value EXACTLY (bit-exact).
  std::array<float, 10> after{};
  failures += check(session.get_local(sphere, after.data()) == 0,
                    "play: post-stop pose readable");
  bool exact = true;
  for (std::size_t i = 0; i < 10; ++i) {
    if (std::isnan(after[i]) || after[i] != pre[i]) {
      exact = false;
    }
  }
  failures += check(exact, "play: stop restores pre-play pose exactly");
  failures += check(after[1] == 5.0F, "play: post-stop y is 5 exactly");

  world.destroy();
  sched.shutdown();

  std::printf(failures == 0 ? "test_play_mode: all checks passed\n"
                            : "test_play_mode: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
