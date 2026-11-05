// net_session.hpp — P7-04: ENet server/client sessions, snapshot
// replication with delta compression, reliable RPCs.
//
// One NetSession owns one ENetHost (server: bound address, up to 32
// peers; client: unbound, one server peer). Wire channels:
//   channel 0 — snapshots, sequenced-unreliable (ENET_PACKET_FLAG_
//               UNSEQUENCED_SEND on unreliable packets; ENet drops stale
//               datagrams past the newest sequence number).
//   channel 1 — control + RPCs, reliable ordered.
//
// Snapshots: the server serializes N entity rows per tick. Each row is
// (entity_id u64, changed_mask u16, changed float words). The changed
// mask is a bitset over the 10 replicated words of EngineLocalTransform
// (px,py,pz,qx,qy,qz,qw,sx,sy,sz); the first snapshot of an entity (or
// after a NAK resync) sends all 10 words. The client keeps the last
// applied value per entity and applies deltas idempotently; a NAK
// (control message) from the client re-bases the server to a full
// snapshot for the named entity. Sequence numbers (u32, wrapping)
// detect loss for the acceptance gate; retransmit is by next-tick
// full-row resend, not by ENet (channel 0 is unreliable by design).
//
// Simulated packet loss (acceptance: 10%) is injected on SEND in the
// adapter (uniform drop via a cheap LCG, seedable) so the convergence
// test needs no real network impairment.
//
// Threading: one session per thread; pump() drives enet_host_service.
// No malloc/new on the frame path is an AGENTS.md invariant for NATIVE
// FRAME code — sessions do socket I/O and may block in service(), so
// they are host tooling like the bridge (base flags, exceptions/RTTI
// on in tests). No exceptions cross the C ABI: entry points are
// noexcept and return null/0/errno-style codes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "engine/core/engine_core_export.hpp"

// MSVC C4251: NetSession is ENGINE_CORE_API (dllexport) and owns
// std::array members; the array internals don't need exporting (same
// push/pop pattern as the renderer's bindless_table.hpp etc.).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

struct _ENetHost;
struct _ENetPeer;

namespace engine::net {

// Replicated payload: EngineLocalTransform words 0..9
// (px,py,pz,qx,qy,qz,qw,sx,sy,sz). pad[6] is never replicated.
inline constexpr std::uint32_t kReplicatedWords = 10;
inline constexpr std::uint32_t kMaxSnapshotRows = 4096;
inline constexpr std::uint32_t kMaxRpcBytes = 1024;
// Wire channels: 0 = snapshot (unsequenced), 1 = control/RPC (reliable).
inline constexpr std::uint8_t kSnapshotChannel = 0;
inline constexpr std::uint8_t kControlChannel = 1;

class ENGINE_CORE_API NetSession {
 public:
  NetSession() noexcept;
  NetSession(const NetSession&) = delete;
  auto operator=(const NetSession&) -> NetSession& = delete;
  ~NetSession() noexcept;

  // Server: bind port on loopback, allow up to 32 peers / 2 channels.
  // Returns false on ENet init or host-create failure.
  auto create_server(std::uint16_t port) noexcept -> bool;
  // Client: unbound host; connect to 127.0.0.1:port. Returns false on
  // init/host-create failure. Connection completes asynchronously:
  // poll until connected() is true.
  auto create_client(std::uint16_t port) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return host_ != nullptr; }
  // True once at least one peer is connected (server: >=1 client;
  // client: server peer in CONNECTED state).
  [[nodiscard]] auto connected() const noexcept -> bool;
  // Pump ENet service with `timeout_ms` (0 = non-blocking). Returns
  // false on null host.
  auto poll(std::uint32_t timeout_ms) noexcept -> bool;

  // --- Snapshot replication -------------------------------------------
  // Server side: serialize one tick for `count` rows (entities[i] with
  // 10-word float payload rows[i]) and broadcast on channel 0. Delta is
  // computed against the last SENT value per entity (server baseline);
  // `force_full` sends all words (first tick / resync). Returns rows
  // queued, or 0 on null host / bad args. Loss injection applies.
  auto snapshot_push(const std::uint64_t* entities, const float (*rows)[10],
                     std::uint32_t count, bool force_full) noexcept
      -> std::uint32_t;
  // Client side: drain queued snapshots into out buffers (capacity rows).
  // Applies deltas onto the client's per-entity baseline so `rows[i]`
  // is the FULL 10-word value; unknown entities start from identity
  // (pos 0, quat identity, scale 1) before the delta applies. Returns
  // rows written. Emits a NAK for out-of-window sequences (server
  // re-bases to full on the next push).
  auto snapshot_pull(std::uint64_t* out_entities, float (*out_rows)[10],
                     std::uint32_t capacity) noexcept -> std::uint32_t;

  // --- RPCs ------------------------------------------------------------
  // Send a reliable RPC (id + payload bytes) on channel 1. Returns true
  // when queued. Loss injection does NOT apply (reliable by design).
  // Byte-span payload (never void*): empty spans are rejected (poll uses
  // 0 for "none pending").
  auto rpc_send(std::uint32_t id,
                std::span<const std::byte> payload) noexcept -> bool;
  // Drain one queued RPC: returns bytes written to `out` (>0), 0 when
  // none pending, negative errno-style on bad args. `out_id` receives
  // the RPC id.
  auto rpc_poll(std::uint32_t* out_id,
                std::span<std::byte> out) noexcept -> std::int32_t;

  // Uniform send-side drop probability [0,1] applied to snapshot sends
  // only (RPC/control stay reliable). Deterministic LCG, seedable.
  auto set_loss(float probability, std::uint32_t seed) noexcept -> void;

  // Test introspection: snapshots sent/received, RPCs sent/received,
  // simulated drops.
  [[nodiscard]] auto stats_sent() const noexcept -> std::uint64_t {
    return sent_;
  }
  [[nodiscard]] auto stats_received() const noexcept -> std::uint64_t {
    return received_;
  }
  [[nodiscard]] auto stats_dropped() const noexcept -> std::uint64_t {
    return dropped_;
  }

 private:
  // Heap state (pimpl): the baseline + pending + RPC tables are ~900KB.
  // They must NOT live inline — two stack NetSessions (1.8MB) overflow
  // the Windows 1MB main-thread stack (Linux 8MB masks it). The handle
  // stays small; create_* allocates, destroy() releases.
  struct State;
  std::unique_ptr<State> state_;

  _ENetHost* host_{nullptr};
  _ENetPeer* server_peer_{nullptr};  // client side only
  bool is_server_{false};

  // Loss injection (snapshot channel only).
  float loss_p_{0.0F};
  std::uint32_t loss_state_{0x12345678U};

  // Server baseline: last-sent value per entity (ring of 4096 slots,
  // direct-mapped by entity id bits — in-process test scale).
  struct BaseRow {
    std::uint64_t entity{0};
    float words[10]{};
    bool known{false};
  };
  static constexpr std::uint32_t kBaseSlots = 4096;

  std::uint32_t send_seq_{0};
  std::uint32_t recv_seq_{0};
  // Whether any snapshot has been received yet (seq starts at 0, so
  // recv_seq_ == 0 is ambiguous without this).
  bool have_snapshot_{false};
  // Set when a sequence gap is seen; poll() emits a NAK-all on the
  // reliable control channel so the server re-bases to a full send.
  bool need_nak_{false};
  // Server side: set by NAK-all; the next push forces full rows.
  bool resync_all_{false};

  // Queued inbound snapshots (pump() fills; snapshot_pull drains).
  struct PendingRow {
    std::uint64_t entity{0};
    float words[10]{};
  };
  static constexpr std::uint32_t kPendingCap = 4096;
  std::uint32_t pending_count_{0};

  // Queued inbound RPCs.
  struct PendingRpc {
    std::uint32_t id{0};
    std::uint32_t size{0};
    std::uint8_t data[kMaxRpcBytes]{};
  };
  static constexpr std::uint32_t kRpcCap = 256;
  std::uint32_t rpc_count_{0};

  std::uint64_t sent_{0};
  std::uint64_t received_{0};
  std::uint64_t dropped_{0};

  auto should_drop() noexcept -> bool;
  static auto base_slot(std::uint64_t entity) noexcept -> std::uint32_t;
  // ENet event pump: routes channel-0 packets to pending_, channel-1
  // to rpcs_/control. Called by poll().
  auto on_receive(std::uint8_t channel, const std::uint8_t* data,
                  std::size_t len) noexcept -> void;
  auto on_snapshot(const std::uint8_t* data,
                   std::size_t len) noexcept -> void;
  auto decode_row(const std::uint8_t* data, std::size_t len,
                  std::size_t& off) noexcept -> bool;
  auto reset_baseline(BaseRow& base, std::uint64_t ent) noexcept -> void;
  auto encode_rows(const std::uint64_t* entities, const float (*rows)[10],
                   std::uint32_t count, bool force_full, std::uint8_t* buf,
                   std::size_t cap, std::uint32_t seq) noexcept
      -> std::uint32_t;
  auto encode_row(std::uint64_t ent, const float row[10], bool force_full,
                  std::uint8_t* buf, std::size_t cap, std::size_t off,
                  std::size_t& next) noexcept -> bool;
  static auto delta_mask(const float row[10],
                         const float base[10]) noexcept -> std::uint16_t;
  static auto packet_bytes(const std::uint8_t* buf, std::size_t cap,
                           std::uint32_t queued) noexcept -> std::size_t;
  auto on_control(const std::uint8_t* data,
                  std::size_t len) noexcept -> void;
};

}  // namespace engine::net

#ifdef _MSC_VER
#pragma warning(pop)
#endif
