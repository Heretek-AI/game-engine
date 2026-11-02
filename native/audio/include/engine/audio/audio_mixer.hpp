// audio_mixer.hpp — P5-06: mixer buses + insert effects.
//
// Owns the three mixer buses (Master, Music, SFX) as ma_sound_group nodes
// on the engine's node graph, plus two graph-level insert effects (a
// low-pass filter node and a delay node), all created at construct time.
//
// Routing model (matches the P5-00 miniaudio spike, §4.13.4):
//   * Master group parents to the engine endpoint (parent = NULL).
//   * Music + SFX groups parent to Master.
//   * Voices route through a bus by passing that bus as pGroup to
//     ma_sound_init_from_data_source (attach path).
//   * LPF + delay nodes live on the engine node graph, detached by
//     default (bit-clean mix); enabling attaches them to the endpoint.
//     Detached-by-default keeps the -6 dB acceptance path dominated only
//     by the bus volume, with no filter coloration.
//
// Frame-path rules: bus volume setters are miniaudio control-side calls
// (thread-safe setters); construct/destroy/uninit allocate or free only.
// noexcept throughout (this TU builds -fno-exceptions).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/audio/miniaudio.hpp"
#include "engine/core/engine_core_export.hpp"

namespace engine::audio {

// Bus ids — stable across the ABI (C# AudioBus mirrors these).
enum class AudioBus : std::uint32_t {
  kMaster = 0,
  kMusic = 1,
  kSfx = 2,
  kCount = 3
};

class ENGINE_CORE_API AudioMixer {
 public:
  AudioMixer() noexcept = default;
  AudioMixer(const AudioMixer&) = delete;
  auto operator=(const AudioMixer&) -> AudioMixer& = delete;
  ~AudioMixer() noexcept;

  // Create Master/Music/SFX groups + LPF + delay nodes on `engine_graph`.
  // `engine` must be an initialized ma_engine* (borrowed: must outlive
  // this). sample_rate/channels mirror the engine config used at
  // engine init. Returns false on any init failure or double-construct.
  auto construct(void* engine, std::uint32_t sample_rate,
                 std::uint32_t channels) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool {
    return engine_ != nullptr;
  }

  // Group handle for a bus (attach path passes it as pGroup). nullptr on
  // invalid bus or unconstructed mixer.
  [[nodiscard]] auto group(AudioBus bus) const noexcept -> void*;

  // Bus volume in dB [-80, +24]; converted with ma_volume_db_to_linear.
  // Out-of-range dB is clamped. Returns false on invalid bus / no mixer.
  auto set_bus_volume_db(AudioBus bus, float db) noexcept -> bool;
  [[nodiscard]] auto bus_volume_db(AudioBus bus) const noexcept -> float;

  // Low-pass insert: cutoff in Hz (20..20000, clamped). Disabled by
  // default (detached); set_lpf_enabled attaches/detaches the node.
  // Detached-by-default keeps the mix bit-clean; enabling inserts the
  // node in SERIES with one bus (re-wires that bus's output through the
  // node), so the whole bus signal passes through the filter (not mixed
  // in parallel). Only one bus is filtered at a time; enabling with a
  // different bus moves the insert. Delay works the same way.
  auto set_lpf_cutoff(float cutoff_hz) noexcept -> bool;
  auto set_lpf_enabled(bool enabled) noexcept -> bool;
  // Selects which bus the LPF insert filters (0 Master, 1 Music, 2 SFX).
  // Takes effect on the next set_lpf_enabled(true); ignored otherwise.
  auto set_lpf_bus(std::uint32_t bus) noexcept -> bool;
  [[nodiscard]] auto lpf_cutoff() const noexcept -> float;
  [[nodiscard]] auto lpf_enabled() const noexcept -> bool;

  // Delay insert: delay in ms (1..2000, clamped), wet/dry/decay in
  // [0,1] (clamped). Disabled by default (detached).
  auto set_delay(float delay_ms, float wet, float dry,
                 float decay) noexcept -> bool;
  auto set_delay_enabled(bool enabled) noexcept -> bool;
  // Selects which bus the delay insert processes (0 Master, 1 Music,
  // 2 SFX). Takes effect on the next set_delay_enabled(true).
  auto set_delay_bus(std::uint32_t bus) noexcept -> bool;
  [[nodiscard]] auto delay_enabled() const noexcept -> bool;

 private:
  void* engine_{nullptr};  // ma_engine* (borrowed)
  void* groups_[3]{nullptr, nullptr, nullptr};  // ma_sound_group* (owned)
  void* lpf_{nullptr};    // ma_lpf_node* (owned)
  void* delay_{nullptr};  // ma_delay_node* (owned)
  // Bus group nodes route to the endpoint by default. An enabled insert
  // splices node->endpoint + bus->node (series): track which bus is
  // currently spliced so disable restores exactly that wiring.
  std::uint32_t lpf_bus_{1};    // bus the LPF filters when enabled
  std::uint32_t lpf_wired_{3};  // bus currently spliced (3 = none)
  std::uint32_t delay_bus_{1};  // bus the delay processes when enabled
  std::uint32_t delay_wired_{3};
  float bus_db_[3]{0.0F, 0.0F, 0.0F};
  float lpf_cutoff_{800.0F};
  bool lpf_enabled_{false};
  bool delay_enabled_{false};
  std::uint32_t sample_rate_{0};
  std::uint32_t channels_{0};
};

}  // namespace engine::audio
