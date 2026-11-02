// audio_mixer.cpp — P5-06 implementation; see header.
//
// Group init order: Master (parent NULL -> endpoint), then Music + SFX
// parented to Master, so Master volume scales the whole mix. Failure
// anywhere unwinds the groups created so far.
//
// Effect nodes are created but DETACHED: ma_*_node_init registers the node
// on the graph while detached means it processes nothing. Enabling calls
// ma_node_attach_output_bus(node, 0, endpoint, 0); disabling detaches.
// The P5-00 spike (§4.13.4) verified attach/detach round-trips.
//
// Clamp policy: dB [-80, +24], LPF cutoff [20, 20000] Hz, delay [1, 2000]
// ms, wet/dry/decay [0, 1]. No allocation on the frame path (all state is
// inline; setters call miniaudio control-side setters only).
#include "engine/audio/audio_mixer.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace engine::audio {
namespace {

auto EngineOf(void* p) noexcept -> ma_engine* {
  return static_cast<ma_engine*>(p);  // NOLINT: opaque handle
}

auto GroupOf(void* p) noexcept -> ma_sound_group* {
  return static_cast<ma_sound_group*>(p);  // NOLINT: opaque group
}

auto LpfOf(void* p) noexcept -> ma_lpf_node* {
  return static_cast<ma_lpf_node*>(p);  // NOLINT: opaque node
}

auto DelayOf(void* p) noexcept -> ma_delay_node* {
  return static_cast<ma_delay_node*>(p);  // NOLINT: opaque node
}

auto Clamp(float v, float lo, float hi) noexcept -> float {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

constexpr float kMinDb = -80.0F;
constexpr float kMaxDb = 24.0F;
constexpr float kMinCutoffHz = 20.0F;
constexpr float kMaxCutoffHz = 20000.0F;
constexpr float kMinDelayMs = 1.0F;
constexpr float kMaxDelayMs = 2000.0F;
constexpr std::uint32_t kLpfOrder = 2;

auto BusIndex(AudioBus bus) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(bus);
}

auto GraphOf(ma_engine* eng) noexcept -> ma_node_graph* {
  return ma_engine_get_node_graph(eng);
}

auto EndpointOf(ma_engine* eng) noexcept -> ma_node* {
  return ma_node_graph_get_endpoint(GraphOf(eng));
}

}  // namespace

AudioMixer::~AudioMixer() noexcept { destroy(); }

auto AudioMixer::construct(void* engine, std::uint32_t sample_rate,
                            std::uint32_t channels) noexcept -> bool {
  if (engine_ != nullptr || engine == nullptr || sample_rate == 0 ||
      channels == 0) {
    return false;
  }
  ma_engine* eng = EngineOf(engine);

  // Master first (parent NULL -> endpoint), then children of Master.
  void* created[3] = {nullptr, nullptr, nullptr};
  void* parents[3] = {nullptr, nullptr, nullptr};
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto* g = static_cast<ma_sound_group*>(
        std::malloc(sizeof(ma_sound_group)));
    if (g == nullptr) {
      for (std::uint32_t j = 0; j < i; ++j) {
        ma_sound_group_uninit(GroupOf(created[j]));
        std::free(created[j]);
      }
      return false;
    }
    ma_sound_group* parent =
        (i == 0) ? nullptr : GroupOf(created[0]);
    parents[i] = parent;
    if (ma_sound_group_init(eng, 0, parent, g) != MA_SUCCESS) {
      std::free(g);
      for (std::uint32_t j = 0; j < i; ++j) {
        ma_sound_group_uninit(GroupOf(created[j]));
        std::free(created[j]);
      }
      return false;
    }
    created[i] = g;
  }

  // Effect nodes: created on the graph, left DETACHED (disabled).
  const std::uint32_t rate =
      sample_rate == 0 ? 48000U : sample_rate;
  auto* lpf =
      static_cast<ma_lpf_node*>(std::malloc(sizeof(ma_lpf_node)));
  if (lpf == nullptr) {
    for (std::uint32_t j = 0; j < 3; ++j) {
      ma_sound_group_uninit(GroupOf(created[j]));
      std::free(created[j]);
    }
    return false;
  }
  const ma_lpf_node_config lpf_cfg =
      ma_lpf_node_config_init(channels, rate, 800.0, kLpfOrder);
  if (ma_lpf_node_init(GraphOf(eng), &lpf_cfg, nullptr, lpf) !=
      MA_SUCCESS) {
    std::free(lpf);
    for (std::uint32_t j = 0; j < 3; ++j) {
      ma_sound_group_uninit(GroupOf(created[j]));
      std::free(created[j]);
    }
    return false;
  }

  // Delay: default 250 ms / wet 0.5 / dry 0.7 / decay 0.5 (spike values).
  const std::uint32_t delay_frames =
      static_cast<std::uint32_t>((250.0F / 1000.0F) *
                                  static_cast<float>(rate));
  auto* delay =
      static_cast<ma_delay_node*>(std::malloc(sizeof(ma_delay_node)));
  if (delay == nullptr) {
    ma_lpf_node_uninit(lpf, nullptr);
    std::free(lpf);
    for (std::uint32_t j = 0; j < 3; ++j) {
      ma_sound_group_uninit(GroupOf(created[j]));
      std::free(created[j]);
    }
    return false;
  }
  const ma_delay_node_config delay_cfg = ma_delay_node_config_init(
      channels, rate, delay_frames, 0.5F);
  if (ma_delay_node_init(GraphOf(eng), &delay_cfg, nullptr, delay) !=
      MA_SUCCESS) {
    std::free(delay);
    ma_lpf_node_uninit(lpf, nullptr);
    std::free(lpf);
    for (std::uint32_t j = 0; j < 3; ++j) {
      ma_sound_group_uninit(GroupOf(created[j]));
      std::free(created[j]);
    }
    return false;
  }
  ma_delay_node_set_wet(delay, 0.5F);
  ma_delay_node_set_dry(delay, 0.7F);
  ma_delay_node_set_decay(delay, 0.5F);
  (void)parents;

  engine_ = engine;
  groups_[0] = created[0];
  groups_[1] = created[1];
  groups_[2] = created[2];
  lpf_ = lpf;
  delay_ = delay;
  bus_db_[0] = 0.0F;
  bus_db_[1] = 0.0F;
  bus_db_[2] = 0.0F;
  lpf_cutoff_ = 800.0F;
  lpf_enabled_ = false;
  delay_enabled_ = false;
  sample_rate_ = rate;
  channels_ = channels;
  return true;
}

auto AudioMixer::destroy() noexcept -> void {
  if (engine_ == nullptr) {
    return;
  }
  ma_engine* eng = EngineOf(engine_);
  if (delay_ != nullptr) {
    if (delay_enabled_) {
      const std::uint32_t b = (delay_wired_ <= 2) ? delay_wired_ : 1;
      ma_node_detach_output_bus(
          reinterpret_cast<ma_node*>(groups_[b]), 0);
      ma_node_attach_output_bus(
          reinterpret_cast<ma_node*>(groups_[b]), 0,
          EndpointOf(eng), 0);
      delay_wired_ = 3;
      delay_enabled_ = false;
    }
    ma_delay_node_uninit(DelayOf(delay_), nullptr);
    std::free(delay_);
    delay_ = nullptr;
  }
  if (lpf_ != nullptr) {
    if (lpf_enabled_) {
      const std::uint32_t b = (lpf_wired_ <= 2) ? lpf_wired_ : 1;
      ma_node_detach_output_bus(
          reinterpret_cast<ma_node*>(groups_[b]), 0);
      ma_node_attach_output_bus(
          reinterpret_cast<ma_node*>(groups_[b]), 0,
          EndpointOf(eng), 0);
      lpf_wired_ = 3;
      lpf_enabled_ = false;
    }
    ma_lpf_node_uninit(LpfOf(lpf_), nullptr);
    std::free(lpf_);
    lpf_ = nullptr;
  }
  // Children before parent (SFX, Music, then Master).
  for (int i = 2; i >= 0; --i) {
    if (groups_[i] != nullptr) {
      ma_sound_group_uninit(GroupOf(groups_[i]));
      std::free(groups_[i]);
      groups_[i] = nullptr;
    }
  }
  (void)eng;
  engine_ = nullptr;
  delay_enabled_ = false;
  lpf_enabled_ = false;
}

auto AudioMixer::group(AudioBus bus) const noexcept -> void* {
  const std::uint32_t i = BusIndex(bus);
  if (engine_ == nullptr || i >= 3) {
    return nullptr;
  }
  return groups_[i];
}

auto AudioMixer::set_bus_volume_db(AudioBus bus, float db) noexcept
    -> bool {
  const std::uint32_t i = BusIndex(bus);
  if (engine_ == nullptr || i >= 3 || groups_[i] == nullptr) {
    return false;
  }
  const float clamped = Clamp(db, kMinDb, kMaxDb);
  ma_sound_group_set_volume(GroupOf(groups_[i]),
                            ma_volume_db_to_linear(clamped));
  bus_db_[i] = clamped;
  return true;
}

auto AudioMixer::bus_volume_db(AudioBus bus) const noexcept -> float {
  const std::uint32_t i = BusIndex(bus);
  if (i >= 3) {
    return 0.0F;
  }
  return bus_db_[i];
}

auto AudioMixer::set_lpf_cutoff(float cutoff_hz) noexcept -> bool {
  if (engine_ == nullptr || lpf_ == nullptr) {
    return false;
  }
  // Reinit the node with the new cutoff (order fixed at 2). The node
  // must be detached during reinit to avoid the mixer reading it
  // mid-config; re-attach afterwards when it was enabled.
  const float cutoff = Clamp(cutoff_hz, kMinCutoffHz, kMaxCutoffHz);
  const bool was_on = lpf_enabled_;
  if (was_on) {
    ma_node_detach_output_bus(reinterpret_cast<ma_node*>(lpf_), 0);
  }
  const ma_lpf_config lpf_cfg = ma_lpf_config_init(
      ma_format_f32, channels_, sample_rate_,
      static_cast<double>(cutoff), kLpfOrder);
  const bool ok =
      ma_lpf_node_reinit(&lpf_cfg, LpfOf(lpf_)) == MA_SUCCESS;
  if (ok) {
    lpf_cutoff_ = cutoff;
  }
  if (was_on) {
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(lpf_), 0,
                              EndpointOf(EngineOf(engine_)), 0);
  }
  return ok;
}

auto AudioMixer::set_lpf_enabled(bool enabled) noexcept -> bool {
  if (engine_ == nullptr || lpf_ == nullptr) {
    return false;
  }
  if (enabled == lpf_enabled_) {
    return true;
  }
  if (enabled) {
    // Series insert: bus output -> LPF input, LPF output -> endpoint.
    // The bus group's default route is endpoint (from group init with a
    // NULL parent); splicing re-attaches its output bus to the node.
    // NOTE: a bus already spliced by the OTHER insert stays spliced —
    // the two inserts chain (bus -> delay -> lpf -> endpoint) rather
    // than conflict. Re-enabling after set_lpf_bus moves the splice.
    const std::uint32_t b = (lpf_bus_ <= 2) ? lpf_bus_ : 1;
    if (lpf_bus_ > 2) {
      lpf_bus_ = 1;
    }
    if (ma_node_attach_output_bus(reinterpret_cast<ma_node*>(lpf_), 0,
                                  EndpointOf(EngineOf(engine_)),
                                  0) != MA_SUCCESS) {
      return false;
    }
    if (ma_node_attach_output_bus(
            reinterpret_cast<ma_node*>(groups_[b]), 0,
            reinterpret_cast<ma_node*>(lpf_), 0) != MA_SUCCESS) {
      ma_node_detach_output_bus(reinterpret_cast<ma_node*>(lpf_), 0);
      return false;
    }
    lpf_wired_ = b;
  } else {
    // Restore: bus output back to endpoint, node output detached.
    const std::uint32_t b = (lpf_wired_ <= 2) ? lpf_wired_ : 1;
    ma_node_detach_output_bus(
        reinterpret_cast<ma_node*>(groups_[b]), 0);
    ma_node_attach_output_bus(
        reinterpret_cast<ma_node*>(groups_[b]), 0,
        EndpointOf(EngineOf(engine_)), 0);
    ma_node_detach_output_bus(reinterpret_cast<ma_node*>(lpf_), 0);
    lpf_wired_ = 3;
  }
  lpf_enabled_ = enabled;
  return true;
}

auto AudioMixer::lpf_cutoff() const noexcept -> float {
  return lpf_cutoff_;
}

auto AudioMixer::set_lpf_bus(std::uint32_t bus) noexcept -> bool {
  if (bus > 2) {
    return false;
  }
  lpf_bus_ = bus;
  return true;
}

auto AudioMixer::set_delay_bus(std::uint32_t bus) noexcept -> bool {
  if (bus > 2) {
    return false;
  }
  delay_bus_ = bus;
  return true;
}

auto AudioMixer::lpf_enabled() const noexcept -> bool {
  return lpf_enabled_;
}

auto AudioMixer::set_delay(float delay_ms, float wet, float dry,
                            float decay) noexcept -> bool {
  if (engine_ == nullptr || delay_ == nullptr) {
    return false;
  }
  // Live-tweak path (delay nodes expose no delay-time setter, only
  // wet/dry/decay): re-tweaking the TIME re-inits in place. Detach during
  // the reinit to avoid the mixer reading it mid-config; re-attach
  // afterwards when it was enabled. All inputs clamped.
  const float ms = Clamp(delay_ms, kMinDelayMs, kMaxDelayMs);
  const std::uint32_t frames = static_cast<std::uint32_t>(
      (ms / 1000.0F) * static_cast<float>(sample_rate_));
  const bool was_on = delay_enabled_;
  if (was_on) {
    ma_node_detach_output_bus(reinterpret_cast<ma_node*>(delay_), 0);
  }
  ma_delay_node_uninit(DelayOf(delay_), nullptr);
  const ma_delay_node_config cfg = ma_delay_node_config_init(
      channels_, sample_rate_, frames == 0 ? 1 : frames,
      Clamp(decay, 0.0F, 1.0F));
  const bool ok = ma_delay_node_init(GraphOf(EngineOf(engine_)), &cfg,
                                     nullptr,
                                     DelayOf(delay_)) == MA_SUCCESS;
  if (ok) {
    ma_delay_node_set_wet(DelayOf(delay_), Clamp(wet, 0.0F, 1.0F));
    ma_delay_node_set_dry(DelayOf(delay_), Clamp(dry, 0.0F, 1.0F));
    ma_delay_node_set_decay(DelayOf(delay_),
                             Clamp(decay, 0.0F, 1.0F));
  }
  if (was_on && ok) {
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(delay_), 0,
                              EndpointOf(EngineOf(engine_)), 0);
  } else if (was_on) {
    // Reinit failed: reflect reality — the node is unusable detached.
    delay_enabled_ = false;
  }
  return ok;
}

auto AudioMixer::set_delay_enabled(bool enabled) noexcept -> bool {
  if (engine_ == nullptr || delay_ == nullptr) {
    return false;
  }
  if (enabled == delay_enabled_) {
    return true;
  }
  if (enabled) {
    // Series insert, same splice as the LPF (bus -> delay -> endpoint).
    const std::uint32_t b = (delay_bus_ <= 2) ? delay_bus_ : 1;
    if (delay_bus_ > 2) {
      delay_bus_ = 1;
    }
    if (ma_node_attach_output_bus(reinterpret_cast<ma_node*>(delay_),
                                  0, EndpointOf(EngineOf(engine_)),
                                  0) != MA_SUCCESS) {
      return false;
    }
    if (ma_node_attach_output_bus(
            reinterpret_cast<ma_node*>(groups_[b]), 0,
            reinterpret_cast<ma_node*>(delay_), 0) != MA_SUCCESS) {
      ma_node_detach_output_bus(reinterpret_cast<ma_node*>(delay_), 0);
      return false;
    }
    delay_wired_ = b;
  } else {
    const std::uint32_t b = (delay_wired_ <= 2) ? delay_wired_ : 1;
    ma_node_detach_output_bus(
        reinterpret_cast<ma_node*>(groups_[b]), 0);
    ma_node_attach_output_bus(
        reinterpret_cast<ma_node*>(groups_[b]), 0,
        EndpointOf(EngineOf(engine_)), 0);
    ma_node_detach_output_bus(reinterpret_cast<ma_node*>(delay_), 0);
    delay_wired_ = 3;
  }
  delay_enabled_ = enabled;
  return true;
}

auto AudioMixer::delay_enabled() const noexcept -> bool {
  return delay_enabled_;
}

}  // namespace engine::audio
