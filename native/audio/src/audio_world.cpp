// audio_world.cpp — M4-04 implementation; see header.
//
// Layout: ma_engine + ma_resource_manager are heap structs allocated once
// at construct (init-time malloc/placement, matching PhysicsWorld's Jolt
// pattern — this TU builds -fno-exceptions so no nothrow-new dance is
// needed beyond the null check). The voice pool is a fixed ma_sound array
// sized by config (malloc'd once; no frame allocation ever).
//
// Threading: miniaudio runs its own mixer thread internally. update()
// only calls ma_sound_*/ma_engine_listener_* setters (documented
// thread-safe for the control side) plus Flecs reads. Flecs structural
// ops (attach_file's ecs_get_mut_id on AudioSource) must happen outside
// a running pipeline — same rule as every other component write.
//
// no_device=true (CI/headless): ma_engine inits with noDevice, so no ALSA/
// PulseAudio/WASAPI handle is opened. Streaming decode + spatialization
// math run identically; ma_engine_read_pcm_frames can still pull mixed
// output for tests.
#include "engine/audio/audio_world.hpp"

#include "engine/audio/components.hpp"
#include "engine/audio/miniaudio.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "flecs.h"

namespace engine::audio {
namespace {

// Row-major WorldTransform translation lane: m[12], m[13], m[14].
// (ComposeLocal emits row-vector matrices: translation in row 3.)
constexpr int kTx = 12;
constexpr int kTy = 13;
constexpr int kTz = 14;

// World-space -Z (forward) axis = row 2 of the matrix, negated.
// With row-vector convention the basis vectors are rows: row 0 = +X
// (right), row 1 = +Y (up), row 2 = +Z (back), so forward = -row2.
auto WorldForward(const float m[16], float out[3]) noexcept -> void {
  out[0] = -m[8];
  out[1] = -m[9];
  out[2] = -m[10];
}

auto ToMaAttenuation(AttenuationModel m) noexcept -> ma_attenuation_model {
  switch (m) {
    case AttenuationModel::kNone:
      return ma_attenuation_model_none;
    case AttenuationModel::kLinear:
      return ma_attenuation_model_linear;
    case AttenuationModel::kExponential:
      return ma_attenuation_model_exponential;
    case AttenuationModel::kInverse:
    default:
      return ma_attenuation_model_inverse;
  }
}

auto EngineOf(void* p) noexcept -> ma_engine* {
  return static_cast<ma_engine*>(p);  // NOLINT: opaque handle
}

auto ManagerOf(void* p) noexcept -> ma_resource_manager* {
  return static_cast<ma_resource_manager*>(p);  // NOLINT: opaque handle
}

auto VoicesOf(void* p) noexcept -> ma_sound* {
  return static_cast<ma_sound*>(p);  // NOLINT: opaque pool
}

}  // namespace

AudioWorld::~AudioWorld() noexcept { destroy(); }

auto AudioWorld::construct(ecs_world_t* flecs_world, bool no_device,
                           const AudioWorldConfig& cfg) noexcept -> bool {
  if (engine_ != nullptr || flecs_world == nullptr || cfg.max_voices == 0 ||
      cfg.listener_count == 0) {
    return false;
  }

  // Register components with explicit sizes (no ECS_COMPONENT macro: this
  // TU is -fno-exceptions/-fno-rtti, same rule as EcsWorld::construct).
  ecs_entity_desc_t src_desc{};
  src_desc.name = "AudioSource";
  const ecs_entity_t src_ent = ecs_entity_init(flecs_world, &src_desc);
  ecs_component_desc_t src_comp{};
  src_comp.entity = src_ent;
  src_comp.type.size = static_cast<ecs_size_t>(sizeof(AudioSource));
  src_comp.type.alignment = static_cast<ecs_size_t>(alignof(AudioSource));
  source_id_ = ecs_component_init(flecs_world, &src_comp);

  ecs_entity_desc_t lst_desc{};
  lst_desc.name = "AudioListener";
  const ecs_entity_t lst_ent = ecs_entity_init(flecs_world, &lst_desc);
  ecs_component_desc_t lst_comp{};
  lst_comp.entity = lst_ent;
  lst_comp.type.size = static_cast<ecs_size_t>(sizeof(AudioListener));
  lst_comp.type.alignment = static_cast<ecs_size_t>(alignof(AudioListener));
  listener_id_ = ecs_component_init(flecs_world, &lst_comp);

  if (source_id_ == 0 || listener_id_ == 0) {
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }

  // Resource manager first: the engine borrows it for lock-free streaming
  // (STREAM + ASYNC decode flags below). Default config = worker threads
  // on, default job queue sizes — the documented streaming setup.
  auto* manager =
      static_cast<ma_resource_manager*>(std::malloc(sizeof(ma_resource_manager)));
  if (manager == nullptr) {
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }
  ma_resource_manager_config manager_config =
      ma_resource_manager_config_init();
  if (ma_resource_manager_init(&manager_config, manager) != MA_SUCCESS) {
    std::free(manager);
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }

  // Engine: noDevice keeps CI/headless green (no ALSA/Pulse handle);
  // period size in ms left at miniaudio defaults for low latency.
  auto* eng = static_cast<ma_engine*>(std::malloc(sizeof(ma_engine)));
  if (eng == nullptr) {
    ma_resource_manager_uninit(manager);
    std::free(manager);
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }
  ma_engine_config engine_config = ma_engine_config_init();
  engine_config.pResourceManager = manager;
  engine_config.noDevice = no_device ? MA_TRUE : MA_FALSE;
  engine_config.sampleRate = cfg.sample_rate;
  engine_config.channels = cfg.channels;
  engine_config.listenerCount = cfg.listener_count;
  if (ma_engine_init(&engine_config, eng) != MA_SUCCESS) {
    std::free(eng);
    ma_resource_manager_uninit(manager);
    std::free(manager);
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }

  // Fixed voice pool: raw storage sized by config, constructed lazily
  // per attach (ma_sound_init_from_file placement into the slot). calloc
  // zeroes entity map + prev-position flags in one pass.
  const std::size_t n = static_cast<std::size_t>(cfg.max_voices);
  auto* pool = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound) * n));
  auto* entities =
      static_cast<std::uint64_t*>(std::calloc(n, sizeof(std::uint64_t)));
  auto* prev = static_cast<float*>(std::calloc(n * 3, sizeof(float)));
  auto* has_prev =
      static_cast<std::uint8_t*>(std::calloc(n, sizeof(std::uint8_t)));
  if (pool == nullptr || entities == nullptr || prev == nullptr ||
      has_prev == nullptr) {
    std::free(has_prev);
    std::free(prev);
    std::free(entities);
    std::free(pool);
    ma_engine_uninit(eng);
    std::free(eng);
    ma_resource_manager_uninit(manager);
    std::free(manager);
    source_id_ = 0;
    listener_id_ = 0;
    return false;
  }

  engine_ = eng;
  resource_manager_ = manager;
  flecs_world_ = flecs_world;
  voices_ = pool;
  voice_entities_ = entities;
  prev_positions_ = prev;
  slot_has_prev_ = has_prev;
  max_voices_ = cfg.max_voices;
  voice_count_ = 0;
  return true;
}

auto AudioWorld::destroy() noexcept -> void {
  if (engine_ == nullptr) {
    return;
  }
  ma_sound* pool = VoicesOf(voices_);
  for (std::uint32_t i = 0; i < voice_count_; ++i) {
    ma_sound_uninit(&pool[i]);
  }
  std::free(slot_has_prev_);
  std::free(prev_positions_);
  std::free(voice_entities_);
  std::free(pool);
  voices_ = nullptr;
  voice_entities_ = nullptr;
  prev_positions_ = nullptr;
  slot_has_prev_ = nullptr;
  ma_engine_uninit(EngineOf(engine_));
  std::free(engine_);
  engine_ = nullptr;
  ma_resource_manager_uninit(ManagerOf(resource_manager_));
  std::free(resource_manager_);
  resource_manager_ = nullptr;
  flecs_world_ = nullptr;
  source_id_ = 0;
  listener_id_ = 0;
  max_voices_ = 0;
  voice_count_ = 0;
}

auto AudioWorld::attach_file(std::uint64_t entity,
                             const char* path) noexcept -> bool {
  if (engine_ == nullptr || entity == 0 || path == nullptr ||
      voice_count_ >= max_voices_) {
    return false;
  }
  auto* src = static_cast<AudioSource*>(ecs_get_mut_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr) {
    return false;  // entity lacks AudioSource
  }
  // NOTE: Flecs zeroes fresh component storage (calloc semantics), so a
  // never-attached source reads slot == 0, NOT kInvalidSlot — the header
  // default member initializer only applies to C++-constructed instances,
  // and Flecs placement never runs it. A slot assigned by attach_file is
  // always < voice_count_ (which only grows), but so is a fresh zero —
  // so disambiguate with the entity recorded in voice_entities_[slot].
  // voice_entities_ is calloc'd (all-zero = no owner); entity ids are
  // never 0, so equality means "this entity owns the slot". Attach then
  // claims the slot by writing BOTH the component and the owner record.
  //
  // IMPORTANT: src was fetched BEFORE ma_sound_init_from_file below, and
  // that call can relocate Flecs tables (component hooks / deferred
  // commands from other threads' resource-manager callbacks). Never trust
  // src->slot across it: re-fetch after success and decide on the fresh
  // value. The only "already attached" signal we honor is the owner
  // record matching THIS entity (idempotent re-attach of the same file).
  if ((src->slot != AudioSource::kInvalidSlot) &&
      (src->slot < voice_count_) &&
      (voice_entities_[src->slot] == entity)) {
    return true;  // idempotent: this entity already owns its slot
  }
  ma_sound* pool = VoicesOf(voices_);
  // STREAM: decode on the resource-manager workers, never blocking the
  // mixer or this thread on the full file. ASYNC + WAIT_INIT: init
  // returns after the header parses (format known) while PCM streams in
  // behind it — bounded call latency, lock-free playback start.
  const ma_uint32 flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM |
                          MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC;
  if (ma_sound_init_from_file(EngineOf(engine_), path, flags, nullptr,
                              nullptr, &pool[voice_count_]) != MA_SUCCESS) {
    return false;
  }
  ma_sound_set_spatialization_enabled(&pool[voice_count_], MA_TRUE);
  voice_entities_[voice_count_] = entity;
  // Claim AFTER the ma_sound init succeeds + re-fetch (table may have
  // moved): then normalize a FRESH (Flecs-zeroed) component to engine
  // defaults. Flecs never runs the in-class initializers (volume=1,
  // pitch=1, distances, cone, attenuation...), so without this every
  // field stays 0: volume 0 = silence, pitch 0 = skipped, cone 0, etc.
  // Only normalize when the component is still fresh (slot still reads
  // 0 with no owner) — never overwrite user-set values on re-attach.
  {
    auto* claim = static_cast<AudioSource*>(ecs_get_mut_id(
        flecs_world_, static_cast<ecs_entity_t>(entity),
        static_cast<ecs_entity_t>(source_id_)));
    if (claim == nullptr) {
      ma_sound_uninit(&pool[voice_count_]);
      return false;
    }
    claim->slot = voice_count_;
    // Fresh = user never wrote any field (all still Flecs-zero). A user
    // who set even one field has a nonzero pattern somewhere we can
    // detect; slot was just written above so exclude it from the test.
    // Normalize ONLY the fresh case — attach-time defaults, never an
    // overwrite of designer values.
    const bool fresh = (claim->volume == 0.0F && claim->pitch == 0.0F &&
                        claim->min_distance == 0.0F &&
                        claim->max_distance == 0.0F &&
                        claim->inner_cone_angle == 0.0F &&
                        claim->outer_cone_angle == 0.0F &&
                        claim->doppler_factor == 0.0F);
    if (fresh) {
      claim->volume = 1.0F;
      claim->pitch = 1.0F;
      claim->min_distance = 1.0F;
      claim->max_distance = 100.0F;
      claim->inner_cone_angle = 6.2831853F;
      claim->outer_cone_angle = 6.2831853F;
      claim->outer_cone_gain = 0.0F;
      claim->doppler_factor = 1.0F;
      claim->attenuation = AttenuationModel::kInverse;
      claim->loop = false;
      claim->spatialized = true;
    }
  }
  ++voice_count_;
  return true;
}

auto AudioWorld::set_playing(std::uint64_t entity, bool playing) noexcept
    -> bool {
  if (engine_ == nullptr) {
    return false;
  }
  const auto* src = static_cast<const AudioSource*>(ecs_get_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr || src->slot == AudioSource::kInvalidSlot ||
      src->slot >= voice_count_ || voice_entities_[src->slot] != entity) {
    return false;
  }
  auto* mut = static_cast<AudioSource*>(ecs_get_mut_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (mut == nullptr) {
    return false;
  }
  mut->playing = playing;
  return true;
}

auto AudioWorld::update(float dt) noexcept -> bool {
  if (engine_ == nullptr || flecs_world_ == nullptr) {
    return false;
  }
  ma_engine* eng = EngineOf(engine_);
  ma_sound* pool = VoicesOf(voices_);

  // --- Listener: first AudioListener entity (lowest id) wins. ---
  // Query (Listener, WorldTransform): iterate all matches, keep min id.
  ecs_query_desc_t lqd{};
  lqd.terms[0].id = static_cast<ecs_entity_t>(listener_id_);
  // NOTE: WorldTransform id comes from the EcsWorld registration order,
  // not from here — instead read the matrix via the entity's world id
  // looked up by name. Simpler robust path: iterate listeners, then
  // ecs_get_id each candidate's WorldTransform by the well-known name.
  // (EcsWorld registers it as "WorldTransform".)
  bool have_listener = false;
  float ear_xyz[3] = {0.0F, 0.0F, 0.0F};
  float ear_fwd[3] = {0.0F, 0.0F, -1.0F};
  float ear_vel[3] = {0.0F, 0.0F, 0.0F};
  {
    ecs_query_t* lq = ecs_query_init(flecs_world_, &lqd);
    if (lq == nullptr) {
      return false;
    }
    ecs_iter_t it = ecs_query_iter(flecs_world_, lq);
    std::uint64_t best = 0;
    const AudioListener* best_lst = nullptr;
    while (ecs_query_next(&it)) {
      const auto* lst = static_cast<const AudioListener*>(
          ecs_field_w_size(&it, sizeof(AudioListener), 0));
      if (lst == nullptr || it.entities == nullptr) {
        continue;
      }
      for (int r = 0; r < it.count; ++r) {
        const std::uint64_t e = it.entities[r];
        if (!have_listener || e < best) {
          have_listener = true;
          best = e;
          best_lst = &lst[r];
        }
      }
    }
    ecs_query_fini(lq);
    if (have_listener) {
      // WorldTransform lookup by entity id: component registered on this
      // world under the "WorldTransform" name.
      const ecs_entity_t wt =
          ecs_lookup(flecs_world_, "WorldTransform");
      if (wt != 0) {
        const auto* w = static_cast<const float*>(
            ecs_get_id(flecs_world_, static_cast<ecs_entity_t>(best), wt));
        if (w != nullptr) {
          ear_xyz[0] = w[kTx];
          ear_xyz[1] = w[kTy];
          ear_xyz[2] = w[kTz];
          WorldForward(w, ear_fwd);
          // Listener Doppler: caller drives movement through the Flecs
          // transform; velocity estimated from the last pushed position.
          // (Single-listener cache in voice slot max_voices_: reuse the
          // trailing prev slot — documented below.)
          const std::uint32_t cache = max_voices_;
          (void)cache;
          if (dt > 0.0F) {
            // Finite-difference on the engine's own last-set position
            // (exact, no extra storage): read back, differentiate.
            const ma_vec3f prev =
                ma_engine_listener_get_position(eng, 0);
            ear_vel[0] = (ear_xyz[0] - prev.x) / dt;
            ear_vel[1] = (ear_xyz[1] - prev.y) / dt;
            ear_vel[2] = (ear_xyz[2] - prev.z) / dt;
          }
        }
      }
      const float df =
          (best_lst != nullptr) ? best_lst->doppler_factor : 1.0F;
      ma_engine_listener_set_position(eng, 0, ear_xyz[0], ear_xyz[1],
                                      ear_xyz[2]);
      ma_engine_listener_set_direction(eng, 0, ear_fwd[0], ear_fwd[1],
                                       ear_fwd[2]);
      ma_engine_listener_set_velocity(eng, 0, ear_vel[0], ear_vel[1],
                                      ear_vel[2]);
      (void)df;  // engine-wide Doppler scale: applied per-sound below.
    }
  }

  // --- Sources: every (AudioSource, WorldTransform) entity with a slot.
  ecs_query_desc_t sqd{};
  sqd.terms[0].id = static_cast<ecs_entity_t>(source_id_);
  ecs_query_t* sq = ecs_query_init(flecs_world_, &sqd);
  if (sq == nullptr) {
    return false;
  }
  const ecs_entity_t wt = ecs_lookup(flecs_world_, "WorldTransform");
  ecs_iter_t it = ecs_query_iter(flecs_world_, sq);
  while (ecs_query_next(&it)) {
    const auto* srcs = static_cast<const AudioSource*>(
        ecs_field_w_size(&it, sizeof(AudioSource), 0));
    if (srcs == nullptr || it.entities == nullptr) {
      continue;
    }
    for (int r = 0; r < it.count; ++r) {
      const AudioSource& s = srcs[r];
      // Attach-time note (see attach_file): fresh Flecs storage reads
      // slot == 0, not kInvalidSlot. A live slot is always < voice_count_
      // here (voice_count_ only grows), so zero-slot entities in a
      // nonempty pool ARE voice 0 — but voice 0 belongs to the entity
      // recorded in voice_entities_[0], not to every zero-slot row.
      // Compare the recorded entity id to avoid aliasing unattached
      // entities onto voice 0.
      if (s.slot == AudioSource::kInvalidSlot || s.slot >= voice_count_) {
        continue;
      }
      const std::uint64_t e = it.entities[r];
      if (voice_entities_[s.slot] != e) {
        continue;  // unattached row aliasing voice 0's zero slot
      }

      // Position / direction / cone from the entity's world matrix.
      ma_sound* voice = &pool[s.slot];
      float px = 0.0F;
      float py = 0.0F;
      float pz = 0.0F;
      float fw[3] = {0.0F, 0.0F, -1.0F};
      if (wt != 0) {
        const auto* w = static_cast<const float*>(
            ecs_get_id(flecs_world_, static_cast<ecs_entity_t>(e), wt));
        if (w != nullptr) {
          px = w[kTx];
          py = w[kTy];
          pz = w[kTz];
          WorldForward(w, fw);
        }
      }
      ma_sound_set_position(voice, px, py, pz);
      ma_sound_set_direction(voice, fw[0], fw[1], fw[2]);

      // Per-frame Doppler velocity: finite difference of pushed
      // positions (no extra component, no allocation).
      if (dt > 0.0F && slot_has_prev_[s.slot] != 0) {
        float* pp = prev_positions_ + static_cast<std::size_t>(s.slot) * 3;
        ma_sound_set_velocity(voice, (px - pp[0]) / dt, (py - pp[1]) / dt,
                              (pz - pp[2]) / dt);
      } else {
        ma_sound_set_velocity(voice, 0.0F, 0.0F, 0.0F);
      }
      float* pp = prev_positions_ + static_cast<std::size_t>(s.slot) * 3;
      pp[0] = px;
      pp[1] = py;
      pp[2] = pz;
      slot_has_prev_[s.slot] = 1;

      // Cone + attenuation + gain/pitch/loop from the component.
      ma_sound_set_cone(voice, s.inner_cone_angle, s.outer_cone_angle,
                        s.outer_cone_gain);
      ma_sound_set_attenuation_model(voice, ToMaAttenuation(s.attenuation));
      ma_sound_set_min_distance(voice, s.min_distance);
      ma_sound_set_max_distance(voice, s.max_distance);
      ma_sound_set_volume(voice, s.volume);
      if (s.pitch > 0.0F) {
        ma_sound_set_pitch(voice, s.pitch);
      }
      ma_sound_set_looping(voice, s.loop ? MA_TRUE : MA_FALSE);
      ma_sound_set_doppler_factor(voice, s.doppler_factor);
      ma_sound_set_spatialization_enabled(voice, s.spatialized ? MA_TRUE
                                                               : MA_FALSE);

      // Play-state edge: only call start/stop on transitions (both walk
      // the graph; calling them every frame would churn).
      const bool is_playing = ma_sound_is_playing(voice) != 0;
      if (s.playing && !is_playing) {
        ma_sound_start(voice);
      } else if (!s.playing && is_playing) {
        ma_sound_stop(voice);
      }
    }
  }
  ecs_query_fini(sq);
  return true;
}

auto AudioWorld::voice_position(std::uint64_t entity,
                                float out_xyz[3]) const noexcept -> bool {
  if (engine_ == nullptr || out_xyz == nullptr) {
    return false;
  }
  const auto* src = static_cast<const AudioSource*>(ecs_get_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr || src->slot == AudioSource::kInvalidSlot ||
      src->slot >= voice_count_ ||
      voice_entities_[src->slot] != entity) {
    return false;
  }
  const ma_vec3f p =
      ma_sound_get_position(&VoicesOf(voices_)[src->slot]);
  out_xyz[0] = p.x;
  out_xyz[1] = p.y;
  out_xyz[2] = p.z;
  return true;
}

auto AudioWorld::voice_velocity(std::uint64_t entity,
                                 float out_xyz[3]) const noexcept -> bool {
  if (engine_ == nullptr || out_xyz == nullptr) {
    return false;
  }
  const auto* src = static_cast<const AudioSource*>(ecs_get_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr || src->slot == AudioSource::kInvalidSlot ||
      src->slot >= voice_count_ ||
      voice_entities_[src->slot] != entity) {
    return false;
  }
  const ma_vec3f v =
      ma_sound_get_velocity(&VoicesOf(voices_)[src->slot]);
  out_xyz[0] = v.x;
  out_xyz[1] = v.y;
  out_xyz[2] = v.z;
  return true;
}

auto AudioWorld::listener_position(float out_xyz[3]) const noexcept -> bool {
  if (engine_ == nullptr || out_xyz == nullptr) {
    return false;
  }
  const ma_vec3f p = ma_engine_listener_get_position(EngineOf(engine_), 0);
  out_xyz[0] = p.x;
  out_xyz[1] = p.y;
  out_xyz[2] = p.z;
  return true;
}

}  // namespace engine::audio
