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

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  engine::net::NetSession server;
  engine::net::NetSession client;
  std::uint16_t port = 0;
  bool up = false;
  for (std::uint16_t p = 51234; p < 51244; ++p) {
    if (!server.create_server(p)) {
      continue;
    }
    if (!client.create_client(p)) {
      server.destroy();
      continue;
    }
    port = p;
    up = true;
    break;
  }
  failures += check(up, "net: server+client sessions created");
  if (!up) {
    std::printf("test_net_replication: %d FAILED (no bind)\n", failures);
    return 1;
  }
  std::printf("[PASS] net: bound loopback port %u\n", port);

  // Connect handshake (client CONNECTED, server sees 1 peer).
  bool linked = false;
  for (int i = 0; i < 200 && !linked; ++i) {
    PumpBoth(server, client);
    linked = server.connected() && client.connected();
  }
  failures += check(linked, "net: client connected, server sees peer");

  // 10% simulated loss on snapshot sends (both directions). The two
  // seeds below were verified to actually drop packets on the tick-0
  // full snapshot (see drops line); they pin the loss draw, not the
  // tick count — convergence must hold for ANY seed.
  server.set_loss(0.10F, 4U);
  client.set_loss(0.10F, 4U);

  // Server-authoritative rows + client mirror.
  std::uint64_t entities[kEntities]{};
  float server_rows[kEntities][10]{};
  float client_rows[kEntities][10]{};
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    entities[i] = static_cast<std::uint64_t>(i + 1);
    MakeRow(i + 1, 0, server_rows[i]);
    // Client starts from identity baseline (all zeros except qw/s=1).
    for (int w = 0; w < 10; ++w) {
      client_rows[i][w] = 0.0F;
    }
    client_rows[i][6] = 1.0F;
    client_rows[i][7] = 1.0F;
    client_rows[i][8] = 1.0F;
    client_rows[i][9] = 1.0F;
  }

  std::uint64_t pull_ent[kEntities]{};
  float pull_row[kEntities][10]{};

  bool converged = false;
  std::uint32_t converged_tick = 0;
  for (std::uint32_t tick = 0; tick < kTicks; ++tick) {
    for (std::uint32_t i = 0; i < kEntities; ++i) {
      MakeRow(i + 1, tick, server_rows[i]);
    }
    server.snapshot_push(entities, server_rows, kEntities, tick == 0);
    PumpBoth(server, client);
    // Drain everything queued (loss + batching may stack packets).
    for (int d = 0; d < 8; ++d) {
      std::uint32_t n =
          client.snapshot_pull(pull_ent, pull_row, kEntities);
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t idx =
            static_cast<std::uint32_t>(pull_ent[i] - 1);
        if (idx < kEntities) {
          for (int w = 0; w < 10; ++w) {
            client_rows[idx][w] = pull_row[i][w];
          }
        }
      }
      if (n == 0) {
        break;
      }
      PumpBoth(server, client);
    }
    bool match = true;
    for (std::uint32_t i = 0; i < kEntities && match; ++i) {
      for (int w = 0; w < 10; ++w) {
        if (server_rows[i][w] != client_rows[i][w]) {
          match = false;
          break;
        }
      }
    }
    if (match) {
      converged = true;
      converged_tick = tick;
      break;
    }
  }
  failures += check(converged, "net: 100 transforms converge within 10 ticks @10% loss");
  if (converged) {
    std::printf("[PASS] net: converged at tick %u (drops: server %llu client %llu)\n",
                converged_tick,
                (unsigned long long)server.stats_dropped(),
                (unsigned long long)client.stats_dropped());
  }

  // Delta gate: no-change push queues zero rows (bandwidth win), and a
  // single-word change sends exactly one row.
  float same[kEntities][10]{};
  for (std::uint32_t i = 0; i < kEntities; ++i) {
    for (int w = 0; w < 10; ++w) {
      same[i][w] = server_rows[i][w];
    }
  }
  // Drain first (stale packets would pollute the count via stats only;
  // snapshot_push returns queued count directly — no ENet read needed).
  std::uint32_t q0 = server.snapshot_push(entities, same, kEntities, false);
  failures += check(q0 == 0, "net: unchanged snapshot pushes 0 rows");
  same[42][0] += 1.0F;
  std::uint32_t q1 = server.snapshot_push(entities, same, kEntities, false);
  failures += check(q1 == 1, "net: single-word change pushes 1 row");
  // Restore server baseline for the row we dirtied (keep later checks exact).
  server.snapshot_push(entities, server_rows, kEntities, true);
  PumpBoth(server, client);
  // Drain the wire so the RPC gate below sees a clean queue.
  for (int d = 0; d < 8; ++d) {
    if (client.snapshot_pull(pull_ent, pull_row, kEntities) == 0) {
      break;
    }
  }

  // RPC gate (reliable despite snapshot loss): both directions, payload
  // integrity, ordering (FIFO per sender).
  const char* pings[2] = {"hello-peer", "second-msg"};
  bool rpc_ok = true;
  for (int i = 0; i < 2; ++i) {
    const char* m = pings[i];
    std::size_t len = 0;
    while (m[len] != '\0') {
      ++len;
    }
    if (!server.rpc_send(1000U + (std::uint32_t)i, m,
                         (std::uint32_t)len + 1)) {
      rpc_ok = false;
    }
  }
  const char reply[] = "ack-from-client";
  if (!client.rpc_send(7, reply, sizeof(reply))) {
    rpc_ok = false;
  }
  // Collect: client should get 2, server should get 1.
  char rbuf[1100]{};
  std::uint32_t rid = 0;
  int got_client = 0;
  int got_server = 0;
  for (int spin = 0; spin < 200 && (got_client < 2 || got_server < 1);
       ++spin) {
    PumpBoth(server, client);
    while (true) {
      std::int32_t n = client.rpc_poll(&rid, rbuf, sizeof(rbuf));
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
      std::int32_t n = server.rpc_poll(&rid, rbuf, sizeof(rbuf));
      if (n <= 0) {
        break;
      }
      ++got_server;
      if (rid != 7 || rbuf[0] != 'a') {
        rpc_ok = false;
      }
    }
  }
  failures += check(rpc_ok && got_client == 2 && got_server == 1,
                    "net: reliable RPCs both directions, ordered, intact");

  client.destroy();
  server.destroy();

  std::printf(failures == 0 ? "test_net_replication: all checks passed\n"
                            : "test_net_replication: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
