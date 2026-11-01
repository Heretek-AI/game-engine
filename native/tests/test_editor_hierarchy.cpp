// test_editor_hierarchy.cpp — P3-03 native acceptance: duplicate() under a
// parent keeps the ChildOf tree walkable (count/get children, destroy),
// and subtree snapshots restore deleted rows with their original ids.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/editor_bridge.hpp"
#include "flecs.h"

namespace {

auto failures = 0;

auto check(bool ok, const char* name) -> void {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto name_of(const engine::editor::EditorSession& s, uint64_t e) -> std::string {
  std::array<char, 64> buf{};
  if (s.get_name(e, buf.data(), buf.size()) != 0) {
    return "<err>";
  }
  return std::string(buf.data());
}

}  // namespace

auto main() -> int {
  engine::core::JobScheduler sched;
  check(sched.init(), "hier: scheduler init");
  engine::ecs::EcsWorld world;
  world.set_scheduler(&sched);
  check(world.construct(), "hier: world construct");
  engine::editor::EditorSession session;
  check(session.bind(world.raw()), "hier: session bind");
  const auto alive = [&world](uint64_t e) { return ecs_is_alive(world.raw(), static_cast<ecs_entity_t>(e)); };

  uint64_t a = 0;
  uint64_t b = 0;
  check(session.spawn("A", &a) == 0 && a != 0, "hier: spawn A");
  check(session.spawn("B", &b) == 0 && b != 0, "hier: spawn B");
  check(session.set_parent(b, a) == 0, "hier: B under A");

  // Duplicate a named child: the copy lands under the same parent.
  uint64_t copy = 0;
  check(session.duplicate(b, &copy) == 0 && copy != 0 && copy != b, "hier: duplicate B");
  check(name_of(session, copy) == "B Copy", "hier: copy named 'B Copy'");

  uint64_t n = 0;
  check(session.count_children(a, &n) == 0 && n == 2, "hier: A has 2 children after duplicate");
  uint64_t c0 = 0;
  uint64_t c1 = 0;
  check(session.get_child(a, 0, &c0) == 0 && session.get_child(a, 1, &c1) == 0,
        "hier: walk A's children");
  check((c0 == b && c1 == copy) || (c0 == copy && c1 == b), "hier: children are B and its copy");

  // Duplicate twice: names stay unique so the second copy is not rejected.
  uint64_t copy2 = 0;
  check(session.duplicate(b, &copy2) == 0 && copy2 != 0 && copy2 != copy, "hier: second duplicate of B");
  check(session.count_children(a, &n) == 0 && n == 3, "hier: A has 3 children");
  check(name_of(session, copy2) == "B Copy 2", "hier: second copy named 'B Copy 2'");

  // Destroy the copy, then delete A's subtree and restore it from a snapshot.
  check(session.destroy_entity(copy) == 0, "hier: destroy copy");
  check(session.count_children(a, &n) == 0 && n == 2, "hier: A back to 2 children");
  auto* snap = session.snapshot_take_subtree(a);
  check(snap != nullptr, "hier: subtree snapshot of A");
  check(session.destroy_entity(a) == 0, "hier: destroy A (cascades)");
  check(!alive(a) && !alive(b), "hier: A and B gone");
  check(session.snapshot_restore_subtree(snap, a) == 0, "hier: restore A subtree");
  check(alive(a) && alive(b) && alive(copy2), "hier: A, B, copy restored with original ids");
  check(session.count_children(a, &n) == 0 && n == 2, "hier: A has 2 children after restore");
  check(name_of(session, b) == "B", "hier: B name restored");
  delete snap;  // NOLINT(cppcoreguidelines-owning-memory) test owns the snapshot (bridge discards via delete too)

  if (failures == 0) {
    std::printf("test_editor_hierarchy: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}
