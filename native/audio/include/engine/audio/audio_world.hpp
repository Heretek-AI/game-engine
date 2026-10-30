// audio_world.hpp — M4-04: AudioWorld facade (miniaudio engine init,
// voice pool, Flecs spatializer system).
//
// Owns: ma_resource_manager (lock-free streaming backend for .wav/.flac/
// .mp3 — miniaudio's built-in decoders; Vorbis/.ogg needs external
// stb_vorbis, which is not vendored) +
// ma_engine (mix graph + 3D spatialization), and a fixed-capacity voice
// pool of ma_sound objects loaded from file paths.
//
// Flecs binding: construct() registers AudioSource + AudioListener
// components on the caller's EcsWorld (component ids stable for the
// AudioWorld lifetime). update(dt) runs the spatializer: for every
// (AudioSource, WorldTransform) entity it pushes position/direction
// (from the World matrix translation + -Z axis), per-frame velocity
// ((pos - prev_pos)/dt for Doppler), cone angles, attenuation distances,
// gain/pitch/loop/play-state edges into the voice's ma_sound; the first
// AudioListener entity drives ma_engine_listener_* (position/forward/
// velocity; world +X right convention, so up is derived forward x right).
//
// Frame-path rules: update() performs no allocation (fixed pool, ids
// and queries cached at construct; voice slot storage is malloc'd once at
// construct (init-time, not frame). noexcept throughout.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/audio/components.hpp"
#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;
struct ecs_query_t;
struct ecs_iter_t;

namespace engine::audio {

struct AudioWorldConfig {
  std::uint32_t max_voices{256};  // fixed voice pool capacity
  std::uint32_t listener_count{1};
  std::uint32_t sample_rate{48000};  // 0 = engine default (required: leaving
                                     // both at 0 is MA_INVALID_ARGS)
  std::uint32_t channels{2};  // 0 = engine default; set explicitly with sr
};

class ENGINE_CORE_API AudioWorld {
 public:
  AudioWorld() noexcept = default;
  AudioWorld(const AudioWorld&) = delete;
  auto operator=(const AudioWorld&) -> AudioWorld& = delete;
  ~AudioWorld() noexcept;

  // Init the resource manager + engine (noDevice=true: mix graph runs
  // without opening a playback device — headless CI, asset pipelines,
  // and tests still stream/decode/spatialize; pass false for audible
  // output on a real machine). Registers AudioSource/AudioListener on
  // the Flecs world (must be a valid EcsWorld raw pointer). Returns
  // false on engine init failure or double-construct.
  auto construct(ecs_world_t* flecs_world, bool no_device,
                 const AudioWorldConfig& cfg = {}) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return engine_ != nullptr; }

  // Component ids (valid after construct()).
  [[nodiscard]] auto source_id() const noexcept -> std::uint64_t {
    return source_id_;
  }
  [[nodiscard]] auto listener_id() const noexcept -> std::uint64_t {
    return listener_id_;
  }

  // Opaque engine handle for tests (read-back of listener/source state).
  [[nodiscard]] auto native_engine() const noexcept -> void* {
    return engine_;
  }

  // Load `path` (.wav/.flac/.mp3, streamed lock-free via the resource
  // manager) into a voice slot and bind it to `entity`'s AudioSource
  // (sets slot + playing=false; call set_playing to start). The entity
  // must already carry AudioSource. Returns false when the pool is full,
  // the entity lacks AudioSource, or the file fails to load.
  auto attach_file(std::uint64_t entity, const char* path) noexcept -> bool;

  // Desired play state for an attached entity (edge-detected in update:
  // transitions call ma_sound_start/stop). Returns false for unknown
  // entities / unattached slots.
  auto set_playing(std::uint64_t entity, bool playing) noexcept -> bool;

  // Per-frame 3D spatialization sync (call after EcsWorld::propagate so
  // World matrices are current). dt <= 0 disables the velocity term
  // (positions/cones/attenuation still update). Returns false when the
  // world is not constructed.
  auto update(float dt) noexcept -> bool;

  // Test/benchmark probes (valid only for attached entities).
  [[nodiscard]] auto voice_count() const noexcept -> std::uint32_t {
    return voice_count_;
  }
  auto voice_position(std::uint64_t entity, float out_xyz[3]) const noexcept
      -> bool;
  auto voice_velocity(std::uint64_t entity, float out_xyz[3]) const noexcept
      -> bool;
  auto listener_position(float out_xyz[3]) const noexcept -> bool;

 private:
  void* engine_{nullptr};              // ma_engine* (owned, malloc'd init)
  void* resource_manager_{nullptr};    // ma_resource_manager* (owned)
  ecs_world_t* flecs_world_{nullptr};  // borrowed, must outlive this
  void* voices_{nullptr};              // fixed ma_sound pool (owned array)
  std::uint64_t* voice_entities_{nullptr};  // entity per slot (owned array)
  float* prev_positions_{nullptr};          // xyz per slot (owned array)
  std::uint8_t* slot_has_prev_{nullptr};    // init flag per slot (owned)
  ecs_query_t* listener_query_{nullptr};    // cached query (owned, fini'd)
  ecs_query_t* source_query_{nullptr};      // cached query (owned, fini'd)
  std::uint32_t* free_slots_{nullptr};  // reclaimed slot stack (owned array)
  std::uint32_t free_count_{0};         // depth of the free stack
  std::uint64_t source_id_{0};
  std::uint64_t listener_id_{0};
  std::uint64_t world_transform_id_{0};  // EcsWorld "WorldTransform" id
  std::uint32_t max_voices_{0};
  std::uint32_t voice_count_{0};  // high-water mark (slots < this were init'd)
  bool listener_has_prev_{false};  // first-frame listener Doppler guard

  // Reclaim path: on_remove fires with the component still readable (slot
  // id) while the entity is halfway out — the ONLY safe ops there are the
  // slot-state unwire + voice stop/uninit (never re-fetch the component or
  // touch other entities). Freed slots go on a LIFO stack; attach pops it
  // first and only bumps the high-water mark when the stack is empty.
  // NOTE: dtor hook deliberately NOT used — it also fires on table moves
  // (add/remove of OTHER components relocates the row), which would stop
  // a live voice for an entity that still owns its slot.
  static auto OnSourceRemove(ecs_iter_t* it) noexcept -> void;
  auto AllocSlot() noexcept -> std::uint32_t;
  auto ReclaimSlot(std::uint32_t slot) noexcept -> void;

  // update() stages (keeps per-stage nesting shallow; no allocation).
  // Pool is void* here (ma_sound is an impl detail; the .cpp casts).
  auto SyncListener(float dt) noexcept -> void;
  auto SyncSources(float dt) noexcept -> void;
  auto SyncOneSource(void* pool, const AudioSource& s,
                     std::uint64_t entity, float dt) noexcept -> void;
};

}  // namespace engine::audio
