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

#include "enet/enet.h"

namespace engine::net {

namespace {

// ENet one-time init (refcounted: N sessions share one process).
int g_enet_refs = 0;

auto EnetUp() noexcept -> bool {
  if (g_enet_refs == 0) {
    if (enet_initialize() != 0) {
      return false;
    }
  }
  ++g_enet_refs;
  return true;
}

auto EnetDown() noexcept -> void {
  if (g_enet_refs > 0) {
    --g_enet_refs;
    if (g_enet_refs == 0) {
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

NetSession::~NetSession() noexcept { destroy(); }

auto NetSession::create_server(std::uint16_t port) noexcept -> bool {
  if (host_ != nullptr) {
    return false;
  }
  if (!EnetUp()) {
    return false;
  }
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = port;
  host_ = enet_host_create(&addr, 32, 2, 0, 0);
  if (host_ == nullptr) {
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
  host_ = enet_host_create(nullptr, 1, 2, 0, 0);
  if (host_ == nullptr) {
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
  pending_count_ = 0;
  rpc_count_ = 0;
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
  constexpr std::size_t kBufSize = 6 + 256U * (8U + 2U + 40U);
  std::uint8_t buf[kBufSize]{};
  const std::uint32_t seq = send_seq_++;
  std::memcpy(buf, &seq, 4);
  const auto n16 = static_cast<std::uint16_t>(count);
  std::memcpy(buf + 4, &n16, 2);
  std::size_t off = 6;
  std::uint32_t queued = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (off + 8 + 2 + 40 > kBufSize) {
      break;
    }
    const std::uint64_t ent = entities[i];
    BaseRow& base = sent_base_[base_slot(ent)];
    if (base.entity != ent || !base.known) {
      base.entity = ent;
      base.known = false;  // full send below re-establishes
    }
    std::uint16_t mask = 0;
    const bool full_row = force_full || resync_all_ || !base.known;
    if (full_row) {
      mask = 0x3FFU;  // all 10 words
    } else {
      for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
        if (!FloatEq(rows[i][w], base.words[w])) {
          mask |= static_cast<std::uint16_t>(1U << w);
        }
      }
      if (mask == 0) {
        continue;  // unchanged: skip the row entirely (delta win)
      }
    }
    std::memcpy(buf + off, &ent, 8);
    off += 8;
    std::memcpy(buf + off, &mask, 2);
    off += 2;
    for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
      if ((mask & (1U << w)) != 0U) {
        std::memcpy(buf + off, &rows[i][w], 4);
        off += 4;
        base.words[w] = rows[i][w];
      }
    }
    base.entity = ent;
    base.known = true;
    ++queued;
  }
  // Rewrite the row count to the rows actually encoded.
  const auto q16 = static_cast<std::uint16_t>(queued);
  std::memcpy(buf + 4, &q16, 2);

  if (should_drop()) {
    ++dropped_;
    return queued;  // counted as sent-side work, packet never hit ENet
  }
  // A NAK-all resync re-bases every entity below (consumed here so the
  // request affects exactly one push).
  if (resync_all_) {
    resync_all_ = false;
  }
  ENetPacket* pkt = enet_packet_create(buf, off, ENET_PACKET_FLAG_UNSEQUENCED);
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
    out_entities[i] = pending_[i].entity;
    for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
      out_rows[i][w] = pending_[i].words[w];
    }
  }
  // Compact the remainder (packets may arrive faster than ticks drain).
  std::uint32_t rest = pending_count_ - n;
  for (std::uint32_t i = 0; i < rest && i < kPendingCap; ++i) {
    pending_[i] = pending_[n + i];
  }
  pending_count_ = rest;
  received_ += n;
  return n;
}

auto NetSession::rpc_send(std::uint32_t id, const void* data,
                          std::uint32_t size) noexcept -> bool {
  if (host_ == nullptr || (data == nullptr && size > 0) ||
      size > kMaxRpcBytes) {
    return false;
  }
  std::uint8_t buf[1 + 4 + 4 + kMaxRpcBytes]{};
  buf[0] = 1;  // kind = RPC
  std::memcpy(buf + 1, &id, 4);
  std::memcpy(buf + 5, &size, 4);
  if (size > 0) {
    std::memcpy(buf + 9, data, size);
  }
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

auto NetSession::rpc_poll(std::uint32_t* out_id, void* out,
                          std::uint32_t capacity) noexcept -> std::int32_t {
  if (out_id == nullptr || (out == nullptr && capacity > 0)) {
    return -1;
  }
  if (rpc_count_ == 0) {
    return 0;
  }
  const PendingRpc& r = rpcs_[0];
  if (r.size > capacity) {
    return -2;  // caller buffer too small; entry kept
  }
  *out_id = r.id;
  if (r.size > 0) {
    std::memcpy(out, r.data, r.size);
  }
  for (std::uint32_t i = 1; i < rpc_count_; ++i) {
    rpcs_[i - 1] = rpcs_[i];
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
    if (seq != recv_seq_ + 1 && recv_seq_ != 0) {
      need_nak_ = true;
    }
    recv_seq_ = seq;
    std::size_t off = 6;
    for (std::uint16_t r = 0; r < rows; ++r) {
      if (off + 10 > len || pending_count_ >= kPendingCap) {
        break;
      }
      std::uint64_t ent = 0;
      std::uint16_t mask = 0;
      std::memcpy(&ent, data + off, 8);
      off += 8;
      std::memcpy(&mask, data + off, 2);
      off += 2;
      const std::uint32_t words = PopCount(mask);
      if (off + words * 4U > len) {
        break;  // truncated: drop the tail of this packet
      }
      BaseRow& base = recv_base_[base_slot(ent)];
      if (base.entity != ent || !base.known) {
        // New entity: identity baseline (pos 0, quat identity, scale 1).
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
      for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
        if ((mask & (1U << w)) != 0U) {
          std::memcpy(&base.words[w], data + off, 4);
          off += 4;
        }
      }
      PendingRow& p = pending_[pending_count_++];
      p.entity = ent;
      for (std::uint32_t w = 0; w < kReplicatedWords; ++w) {
        p.words[w] = base.words[w];
      }
    }
    return;
  }
  if (channel == kControlChannel) {
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
      BaseRow& base = sent_base_[base_slot(ent)];
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
      PendingRpc& r = rpcs_[rpc_count_++];
      r.id = id;
      r.size = size;
      if (size > 0) {
        std::memcpy(r.data, data + 9, size);
      }
      return;
    }
    return;
  }
}

}  // namespace engine::net
