// net_session.cpp — P7-04 implementation; see header for the protocol.
//
// Wire formats (little-endian, host order on x86/ARM CI — the acceptance
// peers are in-process on one machine; no cross-endian support claimed):
//
// Snapshot packet (channel 0, unreliable sequenced via ENet):
//   u32 seq | u16 row_count | rows...
//   row: u64 entity | u16 mask | float words[popcount(mask)]
//   (words are the changed subset of the 10 replicated LocalTransform
//   words, in word-index order)
//
// Control packet (channel 1, reliable): u8 kind | payload...
//   kind 0 NAK: u64 entity (server re-bases that entity to full send)
//   kind 1 RPC: u32 id | u32 size | bytes[size]
//
// ENet already sequences channel-0 packets (unsequenced-send flag means
// "drop stale", not "unordered"): the adapter-level u32 seq detects gaps
// for stats; recovery is next-tick resend (snapshot_pull applies whatever
// arrives; the convergence test resends every tick).
#include "engine/net/net_session.hpp"

#include <cstring>
#include <mutex>

#include "enet/enet.h"

namespace engine::net {

namespace {

// ENet one-time init (refcounted: N sessions share one process).
// Mutex-guarded: sessions may be created/destroyed on different threads
// (one session per thread); an unguarded refcount could double-init ENet
// or deinitialize (WSACleanup on Windows) under a live session.
// Function-local statics (constructed on first use, destroyed at exit);
// the refcount itself must stay mutable (it tracks live sessions).
auto EnetState() noexcept -> std::pair<int&, std::mutex&> {
  static int refs = 0;
  static std::mutex mutex;
  return {refs, mutex};
}

auto EnetUp() noexcept -> bool {
  auto [refs, mutex] = EnetState();
  std::lock_guard<std::mutex> lock(mutex);
  if (refs == 0) {
    if (enet_initialize() != 0) {
      return false;
    }
  }
  ++refs;
  return true;
}

auto EnetDown() noexcept -> void {
  auto [refs, mutex] = EnetState();
  std::lock_guard<std::mutex> lock(mutex);
  if (refs > 0) {
    --refs;
    if (refs == 0) {
      enet_deinitialize();
    }
  }
}

auto FloatEq(float a, float b) noexcept -> bool {
  // Bit-exact compare: the convergence test writes identical floats, and
  // bit-exactness keeps delta masks stable (no epsilon flapping).
  std::uint32_t ua = 0;
  std::uint32_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

auto PopCount(std::uint16_t m) noexcept -> std::uint32_t {
  std::uint32_t n = 0;
  while (m != 0U) {
    n += (m & 1U);
    m = static_cast<std::uint16_t>(m >> 1);
  }
  return n;
}

}  // namespace

// Heap state: ~900KB of baseline/pending/RPC tables. Lives behind
// unique_ptr so NetSession itself stays small (Windows 1MB stack).
struct NetSession::State {
  NetSession::BaseRow sent_base[NetSession::kBaseSlots]{};
  NetSession::BaseRow recv_base[NetSession::kBaseSlots]{};
  NetSession::PendingRow pending[NetSession::kPendingCap]{};
  NetSession::PendingRpc rpcs[NetSession::kRpcCap]{};
};

NetSession::NetSession() noexcept = default;

NetSession::~NetSession() noexcept { destroy(); }

auto NetSession::create_server(std::uint16_t port) noexcept -> bool {
  if (host_ != nullptr) {
    return false;
  }
  if (!EnetUp()) {
    return false;
  }
  try {
    state_ = std::make_unique<State>();
  } catch (...) {
    EnetDown();
    return false;
  }
  ENetAddress addr{};
  // Loopback-only bind (matches the header/ABI/managed docs): a server
  // bound to all interfaces would accept snapshots/RPCs from the network.
  enet_address_set_host(&addr, "127.0.0.1");
  addr.port = port;
  host_ = enet_host_create(&addr, 32, 2, 0, 0);
  if (host_ == nullptr) {
    state_.reset();
    EnetDown();
    return false;
  }
  is_server_ = true;
  return true;
}

auto NetSession::create_client(std::uint16_t port) noexcept -> bool {
  if (host_ != nullptr) {
    return false;
  }
  if (!EnetUp()) {
    return false;
  }
  try {
    state_ = std::make_unique<State>();
  } catch (...) {
    EnetDown();
    return false;
  }
  host_ = enet_host_create(nullptr, 1, 2, 0, 0);
  if (host_ == nullptr) {
    state_.reset();
    EnetDown();
    return false;
  }
  ENetAddress addr{};
  // Loopback: the acceptance peers are in-process on one machine.
  enet_address_set_host(&addr, "127.0.0.1");
  addr.port = port;
  server_peer_ = enet_host_connect(host_, &addr, 2, 0);
  if (server_peer_ == nullptr) {
    enet_host_destroy(host_);
    host_ = nullptr;
    state_.reset();
    EnetDown();
    return false;
  }
  is_server_ = false;
  return true;
}

auto NetSession::destroy() noexcept -> void {
  if (host_ == nullptr) {
    return;
  }
  if (!is_server_ && server_peer_ != nullptr) {
    enet_peer_disconnect(server_peer_, 0);
    // Best-effort flush of the disconnect (non-blocking: 3 polls).
    ENetEvent ev{};
    for (int i = 0; i < 3; ++i) {
      enet_host_service(host_, &ev, 0);
    }
    server_peer_ = nullptr;
  }
  enet_host_destroy(host_);
  host_ = nullptr;
  state_.reset();
  pending_count_ = 0;
  rpc_count_ = 0;
  // Fresh handshake state (seq/NAK flags ride the session, not the socket:
  // a rebound create_server must not inherit them).
  send_seq_ = 0;
  recv_seq_ = 0;
  have_snapshot_ = false;
  need_nak_ = false;
  resync_all_ = false;
  EnetDown();
}

auto NetSession::connected() const noexcept -> bool {
  if (host_ == nullptr) {
    return false;
  }
  if (is_server_) {
    return host_->connectedPeers > 0;
  }
  return server_peer_ != nullptr &&
         server_peer_->state == ENET_PEER_STATE_CONNECTED;
}

auto NetSession::poll(std::uint32_t timeout_ms) noexcept -> bool {
  if (host_ == nullptr) {
    return false;
  }
  ENetEvent ev{};
  // Drain all pending events; a single service() call returns one.
  int rc = enet_host_service(host_, &ev, timeout_ms);
  while (rc > 0) {
    switch (ev.type) {
      case ENET_EVENT_TYPE_RECEIVE:
        on_receive(ev.channelID, ev.packet->data, ev.packet->dataLength);
        enet_packet_destroy(ev.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        if (!is_server_ && ev.peer == server_peer_) {
          server_peer_ = nullptr;
        }
        break;
      default:
        break;
    }
    rc = enet_host_service(host_, &ev, 0);
  }
  // Gap seen on the snapshot channel: emit one NAK-all on the reliable
  // control channel (idempotent; the server re-bases once per push).
  if (need_nak_) {
    need_nak_ = false;
    std::uint8_t nak[1] = {0};  // kind 0, no entity = all
    ENetPacket* pkt = enet_packet_create(nak, 1, ENET_PACKET_FLAG_RELIABLE);
    if (pkt != nullptr) {
      if (is_server_) {
        enet_host_broadcast(host_, kControlChannel, pkt);
      } else if (server_peer_ != nullptr) {
        enet_peer_send(server_peer_, kControlChannel, pkt);
      } else {
        enet_packet_destroy(pkt);
      }
      enet_host_flush(host_);
    }
  }
  return true;
}

auto NetSession::base_slot(std::uint64_t entity) noexcept -> std::uint32_t {
  // Direct-map: low 12 bits. In-process test scale (100 entities);
  // collisions re-base to full (known=false on entity mismatch).
  return static_cast<std::uint32_t>(entity & (kBaseSlots - 1));
}

auto NetSession::should_drop() noexcept -> bool {
  if (loss_p_ <= 0.0F) {
    return false;
  }
  // Cheap LCG: state = state * a + c (glibc constants), take high bits.
  loss_state_ = loss_state_ * 1103515245U + 12345U;
  const auto roll = (loss_state_ >> 8) & 0xFFFFFFU;
  return (static_cast<float>(roll) / 16777216.0F) < loss_p_;
}

auto NetSession::snapshot_push(const std::uint64_t* entities,
                               const float (*rows)[10], std::uint32_t count,
                               bool force_full) noexcept -> std::uint32_t {
  if (host_ == nullptr || (entities == nullptr && count > 0) ||
      (rows == nullptr && count > 0) || count > kMaxSnapshotRows) {
    return 0;
  }
  // Encode: seq + row_count + rows. Worst case 6 + count*(8+2+40).
  // Stack-bounded: snapshots are tick-sized (acceptance: 100 rows).
  // The wire packet caps at 256 full rows (12.8KB, far under ENet's 32MB
  // maximum); larger pushes truncate with the encoded count rewritten
  // below, so the receiver never reads past the buffer. kMaxSnapshotRows
  // stays the API-level batch cap (callers split larger frames across
  // ticks).
  constexpr std::size_t kBufSize = 6 + 256U * (8U + 2U + 40U);
  std::uint8_t buf[kBufSize]{};
  const std::uint32_t seq = send_seq_++;
  const std::uint32_t queued = encode_rows(entities, rows, count, force_full,
                                           buf, kBufSize, seq);

  if (should_drop()) {
    ++dropped_;
    return queued;  // counted as sent-side work, packet never hit ENet
  }
  // A NAK-all resync re-bases every entity below (consumed here so the
  // request affects exactly one push).
  if (resync_all_) {
    resync_all_ = false;
  }
  // Walk the encoded rows: header (6) + per-row (8 entity + 2 mask +
  // 4 * popcount(mask)) for exactly `queued` rows. The buffer always
  // holds them — encode_rows only counts rows it fully wrote.
  const std::size_t bytes = packet_bytes(buf, kBufSize, queued);
  ENetPacket* pkt =
      enet_packet_create(buf, bytes, ENET_PACKET_FLAG_UNSEQUENCED);
  if (pkt == nullptr) {
    return 0;
  }
  if (is_server_) {
    enet_host_broadcast(host_, kSnapshotChannel, pkt);
  } else if (server_peer_ != nullptr) {
    enet_peer_send(server_peer_, kSnapshotChannel, pkt);
  } else {
    enet_packet_destroy(pkt);
    return 0;
  }
  enet_host_flush(host_);
  sent_ += queued;
  return queued;
}

// Serialize rows into buf (header + row records); returns rows encoded.
// Truncates (stops) when the buffer cannot hold another full row.
auto NetSession::encode_rows(const std::uint64_t* entities,
                             const float (*rows)[10], std::uint32_t count,
                             bool force_full, std::uint8_t* buf,
                             std::size_t cap, std::uint32_t seq) noexcept
    -> std::uint32_t {
  std::memcpy(buf, &seq, 4);
  const auto n16 = static_cast<std::uint16_t>(count);
  std::memcpy(buf + 4, &n16, 2);
  std::size_t off = 6;
  std::uint32_t queued = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t before = off;
    std::size_t next = off;
    if (!encode_row(entities[i], rows[i], force_full, buf, cap, off, next)) {
      break;
    }
    off = next;
    if (next != before) {
      ++queued;  // skipped (delta-identical) rows do not advance off
    }
  }
  if (queued != count) {
    // Truncated to fit the wire packet: rewrite the header count to the
    // rows actually encoded so the receiver never over-reads.
    const auto q16 = static_cast<std::uint16_t>(queued);
    std::memcpy(buf + 4, &q16, 2);
  }
  return queued;
}

// Serialize one row at off (entity + mask + changed words). Returns false
// when the row does not fit (caller truncates); advances *next past the
// row on success without touching the baseline on failure.
auto NetSession::encode_row(std::uint64_t ent, const float row[10],
                            bool force_full, std::uint8_t* buf,
                            std::size_t cap, std::size_t off,
                            std::size_t& next) noexcept -> bool {
  if (off + 8 + 2 + 40 > cap) {
    return false;
  }
  BaseRow& base = state_->sent_base[base_slot(ent)];
  if (base.entity != ent || !base.known) {
    base.entity = ent;
    base.known = false;  // full send below re-establishes
  }
  std::uint16_t mask = 0;
  const bool full_row = force_full || resync_all_ || !base.known;
  if (full_row) {
    mask = 0x3FFU;  // all 10 words
  } else {
    mask = delta_mask(row, base.words);
    if (mask == 0) {
      next = off;  // unchanged: skip the row entirely (delta win)
      return true;
    }
  }
  std::memcpy(buf + off, &ent, 8);
  off += 8;
  std::memcpy(buf + off, &mask, 2);
  off += 2;
  for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
    if ((mask & (1U << w)) != 0U) {
      std::memcpy(buf + off, &row[w], 4);
      off += 4;
      base.words[w] = row[w];
    }
  }
  base.entity = ent;
  base.known = true;
  next = off;
  return true;
}

// Changed-word bitmask of row vs baseline (0 = identical).
auto NetSession::delta_mask(const float row[10],
                            const float base[10]) noexcept -> std::uint16_t {
  std::uint16_t mask = 0;
  for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
    if (!FloatEq(row[w], base[w])) {
      mask |= static_cast<std::uint16_t>(1U << w);
    }
  }
  return mask;
}

// Exact wire length of the encoded packet: header (6) + per-row records
// for exactly `queued` rows. The rows are in buf (encode_rows fully
// wrote each one it counted); re-walk the record shapes instead of
// trusting the used offset, so truncation can never over-read.
auto NetSession::packet_bytes(const std::uint8_t* buf, std::size_t cap,
                              std::uint32_t queued) noexcept -> std::size_t {
  std::size_t off = 6;
  for (std::uint32_t i = 0; i < queued; ++i) {
    if (off + 10 > cap) {
      break;  // defensive: never walk past the buffer
    }
    std::uint16_t mask = 0;
    std::memcpy(&mask, buf + off + 8, 2);
    off += 10 + static_cast<std::size_t>(PopCount(mask)) * 4U;
  }
  return off <= cap ? off : cap;
}

auto NetSession::snapshot_pull(std::uint64_t* out_entities,
                               float (*out_rows)[10],
                               std::uint32_t capacity) noexcept
    -> std::uint32_t {
  if ((out_entities == nullptr && capacity > 0) ||
      (out_rows == nullptr && capacity > 0)) {
    return 0;
  }
  std::uint32_t n = pending_count_ < capacity ? pending_count_ : capacity;
  for (std::uint32_t i = 0; i < n; ++i) {
    out_entities[i] = state_->pending[i].entity;
    for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
      out_rows[i][w] = state_->pending[i].words[w];
    }
  }
  // Compact the remainder (packets may arrive faster than ticks drain).
  std::uint32_t rest = pending_count_ - n;
  for (std::uint32_t i = 0; i < rest && i < kPendingCap; ++i) {
    state_->pending[i] = state_->pending[n + i];
  }
  pending_count_ = rest;
  received_ += n;
  return n;
}

auto NetSession::rpc_send(std::uint32_t id,
                          std::span<const std::byte> payload) noexcept
    -> bool {
  // Empty payloads are indistinguishable from "none pending" on the poll
  // side (poll returns 0 for both), so reject them at send: the managed
  // wrapper documents non-empty payloads only.
  if (host_ == nullptr || payload.empty() ||
      payload.size() > kMaxRpcBytes) {
    return false;
  }
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::uint8_t buf[1 + 4 + 4 + kMaxRpcBytes]{};
  buf[0] = 1;  // kind = RPC
  std::memcpy(buf + 1, &id, 4);
  std::memcpy(buf + 5, &size, 4);
  std::memcpy(buf + 9, payload.data(), size);
  ENetPacket* pkt =
      enet_packet_create(buf, 9 + size, ENET_PACKET_FLAG_RELIABLE);
  if (pkt == nullptr) {
    return false;
  }
  if (is_server_) {
    enet_host_broadcast(host_, kControlChannel, pkt);
  } else if (server_peer_ != nullptr) {
    enet_peer_send(server_peer_, kControlChannel, pkt);
  } else {
    enet_packet_destroy(pkt);
    return false;
  }
  enet_host_flush(host_);
  ++sent_;
  return true;
}

auto NetSession::rpc_poll(std::uint32_t* out_id,
                          std::span<std::byte> out) noexcept -> std::int32_t {
  if (out_id == nullptr) {
    return -1;
  }
  if (rpc_count_ == 0) {
    return 0;
  }
  const PendingRpc& r = state_->rpcs[0];
  if (r.size > out.size()) {
    return -2;  // caller buffer too small; entry kept
  }
  *out_id = r.id;
  if (r.size > 0) {
    std::memcpy(out.data(), r.data, r.size);
  }
  for (std::uint32_t i = 1; i < rpc_count_; ++i) {
    state_->rpcs[i - 1] = state_->rpcs[i];
  }
  --rpc_count_;
  ++received_;
  return static_cast<std::int32_t>(r.size);
}

auto NetSession::set_loss(float probability, std::uint32_t seed) noexcept
    -> void {
  loss_p_ = probability < 0.0F   ? 0.0F
            : probability > 1.0F ? 1.0F
                                 : probability;
  loss_state_ = seed;
}

auto NetSession::on_receive(std::uint8_t channel, const std::uint8_t* data,
                            std::size_t len) noexcept -> void {
  if (data == nullptr) {
    return;
  }
  if (channel == kSnapshotChannel) {
    on_snapshot(data, len);
    return;
  }
  if (channel == kControlChannel) {
    on_control(data, len);
    return;
  }
}

// Snapshot channel: seq + row_count + rows. Gap detection flags NAK-all
// (server re-bases to full on the next push); dropped rows from a full
// pending queue or a truncated tail also NAK.
auto NetSession::on_snapshot(const std::uint8_t* data,
                             std::size_t len) noexcept -> void {
  if (len < 6) {
    return;
  }
  std::uint32_t seq = 0;
  std::uint16_t rows = 0;
  std::memcpy(&seq, data, 4);
  std::memcpy(&rows, data + 4, 2);
  // Stale/duplicate guard: ENet unsequenced delivery already drops
  // older datagrams; the adapter seq is a gap detector. On a gap the
  // client may hold deltas for a baseline it never received, so flag
  // a NAK-all: the server's next push re-bases to full rows and the
  // streams reconverge (polled out at the end of poll()).
  if (have_snapshot_ && seq != recv_seq_ + 1) {
    need_nak_ = true;
  }
  recv_seq_ = seq;
  have_snapshot_ = true;
  std::size_t off = 6;
  for (std::uint16_t r = 0; r < rows; ++r) {
    if (!decode_row(data, len, off)) {
      need_nak_ = true;  // dropped rows: ask the server to re-base full
      break;
    }
  }
}

// Decode one snapshot row at *off (entity + mask + changed words),
// advancing off past it. Returns false when the packet is truncated or
// the pending queue is full (caller NAKs).
auto NetSession::decode_row(const std::uint8_t* data, std::size_t len,
                            std::size_t& off) noexcept -> bool {
  if (off + 10 > len || pending_count_ >= kPendingCap) {
    return false;
  }
  std::uint64_t ent = 0;
  std::uint16_t mask = 0;
  std::memcpy(&ent, data + off, 8);
  off += 8;
  std::memcpy(&mask, data + off, 2);
  off += 2;
  const std::uint32_t words = PopCount(mask);
  if (off + words * 4U > len) {
    return false;  // truncated: drop the tail of this packet
  }
  BaseRow& base = state_->recv_base[base_slot(ent)];
  if (base.entity != ent || !base.known) {
    reset_baseline(base, ent);
  }
  for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
    if ((mask & (1U << w)) != 0U) {
      std::memcpy(&base.words[w], data + off, 4);
      off += 4;
    }
  }
  PendingRow& p = state_->pending[pending_count_++];
  p.entity = ent;
  for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
    p.words[w] = base.words[w];
  }
  return true;
}

// New entity: identity baseline (pos 0, quat identity, scale 1).
auto NetSession::reset_baseline(BaseRow& base,
                               std::uint64_t ent) noexcept -> void {
  base.entity = ent;
  for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
    base.words[w] = 0.0F;
  }
  base.words[6] = 1.0F;  // qw
  base.words[7] = 1.0F;  // sx
  base.words[8] = 1.0F;  // sy
  base.words[9] = 1.0F;  // sz
  base.known = true;
}

auto NetSession::on_control(const std::uint8_t* data,
                            std::size_t len) noexcept -> void {
  if (len < 1) {
    return;
  }
  const std::uint8_t kind = data[0];
  if (kind == 0) {  // NAK: len 1 = all entities, len 9 = one entity
    if (len == 1) {
      resync_all_ = true;
      return;
    }
    if (len < 9) {
      return;
    }
    std::uint64_t ent = 0;
    std::memcpy(&ent, data + 1, 8);
    BaseRow& base = state_->sent_base[base_slot(ent)];
    if (base.entity == ent) {
      base.known = false;
    }
    return;
  }
  if (kind == 1) {  // RPC
    if (len < 9 || rpc_count_ >= kRpcCap) {
      return;
    }
    std::uint32_t id = 0;
    std::uint32_t size = 0;
    std::memcpy(&id, data + 1, 4);
    std::memcpy(&size, data + 5, 4);
    if (size > kMaxRpcBytes || 9 + size > len) {
      return;
    }
    PendingRpc& r = state_->rpcs[rpc_count_++];
    r.id = id;
    r.size = size;
    if (size > 0) {
      std::memcpy(r.data, data + 9, size);
    }
    return;
  }
}

}  // namespace engine::net
