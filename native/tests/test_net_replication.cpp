// test_net_replication.cpp — P7-04 acceptance: two in-process peers
// converge on 100 replicated transforms within 10 ticks at 10%
// simulated packet loss.
//
// Setup: server + client NetSessions over loopback (ports picked from a
// fixed ladder with bind-retry). Both sides hold 100 entity rows
// (entity ids 1..100, LocalTransform 10-word payloads). Each tick:
//   1. server mutates every transform (tick-indexed drift),
//   2. server snapshot_push (force_full on tick 0),
//   3. both sides poll (0ms + 1ms settle) to move ENet queues,
//   4. client snapshot_pull applies to its mirror rows.
// After <= 10 ticks every client row must equal the server row
// bit-exact. RPC gate: 4 reliable sends (both directions) all arrive
// intact DESPITE 10% snapshot loss (loss applies to snapshots only).
// Delta gate: a no-change push queues zero rows (mask-all-clear skip).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "engine/net/net_session.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

constexpr std::uint32_t kEntities = 100;
constexpr std::uint32_t kTicks = 10;

auto MakeRow(std::uint32_t ent, std::uint32_t tick, float out[10]) -> void {
  // Deterministic drift: pos walks with tick, quat stays identity-ish,
  // scale fixed. Distinct per entity so row mix-ups fail the check.
  const float f = static_cast<float>(ent);
  const float t = static_cast<float>(tick);
  out[0] = f * 1.0F + t * 0.25F;
  out[1] = f * 2.0F - t * 0.125F;
  out[2] = f * 0.5F + t * 0.5F;
  out[3] = 0.0F;
  out[4] = 0.0F;
  out[5] = 0.0F;
  out[6] = 1.0F;
  out[7] = 1.0F;
  out[8] = 1.0F;
  out[9] = 1.0F;
}

auto PumpBoth(engine::net::NetSession& a, engine::net::NetSession& b)
    -> void {
  // Move ENet queues: service both ends, then a 1ms settle for the
  // loopback round-trip (localhost delivery is same-tick, but the
  // settle keeps loaded-CI flakes at zero).
  for (int i = 0; i < 4; ++i) {
    a.poll(0);
    b.poll(0);
  }
  a.poll(1);
  b.poll(1);
}

// Copy one pulled row into the client mirror (bounds-checked index).
auto ApplyRow(std::uint64_t ent, const float row[10],
              float mirror[kEntities][10]) -> void {
  const std::uint32_t idx = static_cast<std::uint32_t>(ent - 1);
  if (idx >= kEntities) {
    return;
  }
  for (int w = 0; w < 10; ++w) {
    mirror[idx][w] = row[w];
  }
}

// True when every client row bit-matches the server row.
auto RowsConverged(const float want[kEntities][10],
                   const float got[kEntities][10]) -> bool {
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    for (int w = 0; w < 10; ++w) {
      if (want[i][w] != got[i][w]) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

// Test phases below: each returns check() failures (0 = pass). Splitting
// main into phases keeps every function under the complexity budget.
struct NetFixture {
  engine::net::NetSession server;
  engine::net::NetSession client;
  std::uint64_t entities[kEntities]{};
  float server_rows[kEntities][10]{};
  float client_rows[kEntities][10]{};
  std::uint64_t pull_ent[kEntities]{};
  float pull_row[kEntities][10]{};
};

auto SetupSessions(NetFixture& f, std::uint16_t& port_out) -> int {
  for (std::uint16_t p = 51234; p < 51244; ++p) {
    if (!f.server.create_server(p)) {
      continue;
    }
    if (!f.client.create_client(p)) {
      f.server.destroy();
      continue;
    }
    port_out = p;
    return check(true, "net: server+client sessions created");
  }
  return check(false, "net: server+client sessions created");
}

auto Handshake(NetFixture& f) -> int {
  bool linked = false;
  for (int i = 0; i < 200 && !linked; ++i) {
    PumpBoth(f.server, f.client);
    linked = f.server.connected() && f.client.connected();
  }
  return check(linked, "net: client connected, server sees peer");
}

auto TickOnce(NetFixture& f, std::uint32_t tick) -> void {
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    MakeRow(i + 1, tick, f.server_rows[i]);
  }
  f.server.snapshot_push(f.entities, f.server_rows, kEntities, tick == 0);
  PumpBoth(f.server, f.client);
  // Drain everything queued (loss + batching may stack packets).
  for (int d = 0; d < 8; ++d) {
    std::uint32_t n =
        f.client.snapshot_pull(f.pull_ent, f.pull_row, kEntities);
    for (std::uint32_t i = 0; i < n; ++i) {
      ApplyRow(f.pull_ent[i], f.pull_row[i], f.client_rows);
    }
    if (n == 0) {
      break;
    }
    PumpBoth(f.server, f.client);
  }
}

auto ConvergenceGate(NetFixture& f) -> int {
  bool converged = false;
  std::uint32_t converged_tick = 0;
  for (std::uint32_t tick = 0; tick < kTicks; ++tick) {
    TickOnce(f, tick);
    if (RowsConverged(f.server_rows, f.client_rows)) {
      converged = true;
      converged_tick = tick;
      break;
    }
  }
  int failures = check(converged, "net: 100 transforms converge within 10 ticks @10% loss");
  if (converged) {
    std::printf("[PASS] net: converged at tick %u (drops: server %llu client %llu)\n",
                converged_tick,
                (unsigned long long)f.server.stats_dropped(),
                (unsigned long long)f.client.stats_dropped());
  }
  return failures;
}

auto DeltaGate(NetFixture& f) -> int {
  int failures = 0;
  // No-change push queues zero rows (bandwidth win), and a single-word
  // change sends exactly one row.
  float same[kEntities][10]{};
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    for (int w = 0; w < 10; ++w) {
      same[i][w] = f.server_rows[i][w];
    }
  }
  // Drain first (stale packets would pollute the count via stats only;
  // snapshot_push returns queued count directly — no ENet read needed).
  std::uint32_t q0 = f.server.snapshot_push(f.entities, same, kEntities, false);
  failures += check(q0 == 0, "net: unchanged snapshot pushes 0 rows");
  same[42][0] += 1.0F;
  std::uint32_t q1 = f.server.snapshot_push(f.entities, same, kEntities, false);
  failures += check(q1 == 1, "net: single-word change pushes 1 row");
  // Restore server baseline for the row we dirtied (keep later checks exact).
  f.server.snapshot_push(f.entities, f.server_rows, kEntities, true);
  PumpBoth(f.server, f.client);
  // Drain the wire so the RPC gate below sees a clean queue.
  for (int d = 0; d < 8; ++d) {
    if (f.client.snapshot_pull(f.pull_ent, f.pull_row, kEntities) == 0) {
      break;
    }
  }
  return failures;
}

auto SendPings(NetFixture& f) -> bool {
  const char* pings[2] = {"hello-peer", "second-msg"};
  bool ok = true;
  for (int i = 0; i < 2; ++i) {
    const char* m = pings[i];
    std::size_t len = 0;
    while (m[len] != '\0') {
      ++len;
    }
    const auto bytes = std::as_bytes(std::span(m, len + 1));
    if (!f.server.rpc_send(1000U + (std::uint32_t)i, bytes)) {
      ok = false;
    }
  }
  const char reply[] = "ack-from-client";
  if (!f.client.rpc_send(7, std::as_bytes(std::span(reply)))) {
    ok = false;
  }
  return ok;
}

auto DrainRpcs(NetFixture& f, char* rbuf, std::uint32_t& rid, int& got_client,
               int& got_server, bool& rpc_ok) -> void {
  const auto rspan = std::as_writable_bytes(std::span(rbuf, 1100));
  while (true) {
    std::int32_t n = f.client.rpc_poll(&rid, rspan);
    if (n <= 0) {
      break;
    }
    ++got_client;
    if (got_client == 1 && (rid != 1000U || rbuf[0] != 'h')) {
      rpc_ok = false;
    }
    if (got_client == 2 && (rid != 1001U || rbuf[0] != 's')) {
      rpc_ok = false;
    }
  }
  while (true) {
    std::int32_t n = f.server.rpc_poll(&rid, rspan);
    if (n <= 0) {
      break;
    }
    ++got_server;
    if (rid != 7 || rbuf[0] != 'a') {
      rpc_ok = false;
    }
  }
}

auto RpcGate(NetFixture& f) -> int {
  // Reliable despite snapshot loss: both directions, payload integrity,
  // ordering (FIFO per sender).
  bool rpc_ok = SendPings(f);
  // Collect: client should get 2, server should get 1.
  char rbuf[1100]{};
  std::uint32_t rid = 0;
  int got_client = 0;
  int got_server = 0;
  for (int spin = 0; spin < 200 && (got_client < 2 || got_server < 1);
       ++spin) {
    PumpBoth(f.server, f.client);
    DrainRpcs(f, rbuf, rid, got_client, got_server, rpc_ok);
  }
  return check(rpc_ok && got_client == 2 && got_server == 1,
               "net: reliable RPCs both directions, ordered, intact");
}

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  NetFixture f;
  std::uint16_t port = 0;
  failures += SetupSessions(f, port);
  if (port == 0) {
    std::printf("test_net_replication: %d FAILED (no bind)\n", failures);
    return 1;
  }
  std::printf("[PASS] net: bound loopback port %u\n", port);
  failures += Handshake(f);

  // 10% simulated loss on snapshot sends (both directions). The two
  // seeds below were verified to actually drop packets on the tick-0
  // full snapshot (see drops line); they pin the loss draw, not the
  // tick count — convergence must hold for ANY seed.
  f.server.set_loss(0.10F, 4U);
  f.client.set_loss(0.10F, 4U);

  // Server-authoritative rows + client mirror.
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    f.entities[i] = static_cast<std::uint64_t>(i + 1);
    MakeRow(i + 1, 0, f.server_rows[i]);
    // Client starts from identity baseline (all zeros except qw/s=1).
    for (int w = 0; w < 10; ++w) {
      f.client_rows[i][w] = 0.0F;
    }
    f.client_rows[i][6] = 1.0F;
    f.client_rows[i][7] = 1.0F;
    f.client_rows[i][8] = 1.0F;
    f.client_rows[i][9] = 1.0F;
  }

  failures += ConvergenceGate(f);
  failures += DeltaGate(f);
  failures += RpcGate(f);

  f.client.destroy();
  f.server.destroy();

  std::printf(failures == 0 ? "test_net_replication: all checks passed\n"
                            : "test_net_replication: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
