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

// Flecs ctor hooks: copy the C++ defaults into fresh component storage so
// the in-class initializers (slot=kInvalidSlot, volume/pitch=1, distances,
// cone angles, doppler, attenuation) apply to Flecs-owned instances.
// Flecs otherwise zeroes fresh component memory (calloc semantics) and the
// initializers never run.
//
// Two strict requirements on the copy mechanism:
//  1. Alignment: Flecs column storage is malloc-aligned (16B), NOT
//     alignas(64) — a placement-new loop vectorizes under AVX2 into
//     vmovaps (32B-aligned) stores and #GP-faults on mod32==16 rows
//     (observed). memcpy must tolerate arbitrary alignment, so the
//     compiler emits safe (movups) stores.
//  2. -fno-exceptions TU: both components are trivially-copyable
//     aggregates, so memcpy == placement construction here.
void AudioSourceCtor(void* ptr, int32_t count,
                     const ecs_type_info_t* /*type_info*/) {
  static const AudioSource kDefault{};
  // Scalar field copy: Flecs column storage is malloc-aligned (16B), NOT
  // alignas(64) — even memcpy() vectorizes here into vmovaps (32B-aligned)
  // stores that #GP-fault on mod32==16 rows (observed). Per-field scalar
  // stores cannot fault on alignment.
  auto* p = static_cast<AudioSource*>(ptr);
  for (int32_t i = 0; i < count; ++i) {
    p[i].slot = kDefault.slot;
    p[i].volume = kDefault.volume;
    p[i].pitch = kDefault.pitch;
    p[i].min_distance = kDefault.min_distance;
    p[i].max_distance = kDefault.max_distance;
    p[i].inner_cone_angle = kDefault.inner_cone_angle;
    p[i].outer_cone_angle = kDefault.outer_cone_angle;
    p[i].outer_cone_gain = kDefault.outer_cone_gain;
    p[i].doppler_factor = kDefault.doppler_factor;
    p[i].attenuation = kDefault.attenuation;
    p[i].loop = kDefault.loop;
    p[i].playing = kDefault.playing;
    p[i].spatialized = kDefault.spatialized;
  }
}

void AudioListenerCtor(void* ptr, int32_t count,
                       const ecs_type_info_t* /*type_info*/) {
  static const AudioListener kDefault{};
  auto* p = static_cast<AudioListener*>(ptr);
  for (int32_t i = 0; i < count; ++i) {
    p[i].doppler_factor = kDefault.doppler_factor;
  }
}

}  // namespace

// on_remove (type hook): fires for an AudioSource row that is going away —
// entity delete, remove_id, or world teardown. it->ctx is the AudioWorld*;
// field(0) still holds the row's slot id, so reclaim reads ONLY that.
// Safe ops here: stop/uninit the owned voice, clear the owner record,
// reset per-slot Doppler state, push the slot on the free stack. NEVER
// re-fetch the component (ecs_get_mut adds it back mid-removal) or touch
// other entities. noexcept: hooks run inside Flecs table surgery.
auto AudioWorld::OnSourceRemove(ecs_iter_t* it) noexcept -> void {
  if (it == nullptr || it->ctx == nullptr || it->count <= 0) {
    return;
  }
  auto* self = static_cast<AudioWorld*>(it->ctx);
  if (self->voices_ == nullptr || self->voice_entities_ == nullptr ||
      self->max_voices_ == 0) {
    return;  // mid-construct failure unwind / post-destroy: nothing owned
  }
  const auto* rows = static_cast<const AudioSource*>(
      ecs_field_w_size(it, sizeof(AudioSource), 0));
  if (rows == nullptr) {
    return;
  }
  for (int r = 0; r < it->count; ++r) {
    const std::uint32_t slot = rows[r].slot;
    if (slot < self->max_voices_) {
      self->ReclaimSlot(slot);
    }
  }
}

// Pop a reclaimed slot; fall back to the high-water bump. Returns
// kInvalidSlot when the pool is exhausted.
auto AudioWorld::AllocSlot() noexcept -> std::uint32_t {
  if (free_count_ > 0 && free_slots_ != nullptr) {
    --free_count_;
    return free_slots_[free_count_];
  }
  if (voice_count_ >= max_voices_) {
    return AudioSource::kInvalidSlot;
  }
  const std::uint32_t slot = voice_count_;
  ++voice_count_;
  return slot;
}

auto AudioWorld::ReclaimSlot(std::uint32_t slot) noexcept -> void {
  if (slot >= max_voices_ || voices_ == nullptr ||
      voice_entities_ == nullptr || free_slots_ == nullptr) {
    return;
  }
  // Idempotent: only the recorded owner passes; entity 0 never owns a slot
  // (calloc zero = no owner), so double-remove / teardown races are no-ops.
  if (voice_entities_[slot] == 0) {
    return;
  }
  ma_sound* pool = VoicesOf(voices_);
  ma_sound_stop(&pool[slot]);    // safe on a non-started voice
  ma_sound_uninit(&pool[slot]);  // frees stream decoder + graph node
  voice_entities_[slot] = 0;
  slot_has_prev_[slot] = 0;
  if (free_count_ < max_voices_) {
    free_slots_[free_count_] = slot;
    ++free_count_;
  }
}

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

  // Ctor hooks: placement-new the C++ defaults into Flecs-owned storage
  // (must precede any entity carrying these components — no entity can
  // exist yet, construct() runs before any spawn using this world, so the
  // hooks apply from the first add).
  {
    ecs_type_hooks_t src_hooks{};
    src_hooks.ctor = AudioSourceCtor;
    src_hooks.on_remove = AudioWorld::OnSourceRemove;
    src_hooks.ctx = this;
    ecs_set_hooks_id(flecs_world, source_id_, &src_hooks);
    ecs_type_hooks_t lst_hooks{};
    lst_hooks.ctor = AudioListenerCtor;
    ecs_set_hooks_id(flecs_world, listener_id_, &lst_hooks);
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
  auto* free_stack =
      static_cast<std::uint32_t*>(std::malloc(n * sizeof(std::uint32_t)));
  if (pool == nullptr || entities == nullptr || prev == nullptr ||
      has_prev == nullptr || free_stack == nullptr) {
    std::free(free_stack);
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
  free_slots_ = free_stack;
  free_count_ = 0;
  max_voices_ = cfg.max_voices;
  voice_count_ = 0;
  listener_has_prev_ = false;

  // Cache the frame-path queries + WorldTransform id once (update() must
  // not allocate per frame). Query init failure unwinds everything above.
  world_transform_id_ =
      static_cast<std::uint64_t>(ecs_lookup(flecs_world, "WorldTransform"));
  {
    ecs_query_desc_t ld{};
    ld.terms[0].id = static_cast<ecs_entity_t>(listener_id_);
    listener_query_ = ecs_query_init(flecs_world, &ld);
    ecs_query_desc_t sd{};
    sd.terms[0].id = static_cast<ecs_entity_t>(source_id_);
    source_query_ = ecs_query_init(flecs_world, &sd);
  }
  if (listener_query_ == nullptr || source_query_ == nullptr) {
    if (listener_query_ != nullptr) {
      ecs_query_fini(listener_query_);
      listener_query_ = nullptr;
    }
    if (source_query_ != nullptr) {
      ecs_query_fini(source_query_);
      source_query_ = nullptr;
    }
    std::free(free_stack);
    std::free(has_prev);
    std::free(prev);
    std::free(entities);
    std::free(pool);
    ma_engine_uninit(eng);
    std::free(eng);
    ma_resource_manager_uninit(manager);
    std::free(manager);
    engine_ = nullptr;
    resource_manager_ = nullptr;
    flecs_world_ = nullptr;
    voices_ = nullptr;
    voice_entities_ = nullptr;
    prev_positions_ = nullptr;
    slot_has_prev_ = nullptr;
    source_id_ = 0;
    listener_id_ = 0;
    world_transform_id_ = 0;
    max_voices_ = 0;
    return false;
  }
  return true;
}

auto AudioWorld::destroy() noexcept -> void {
  if (engine_ == nullptr) {
    return;
  }
  // Detach the reclaim hook BEFORE tearing the voices down: entity cleanup
  // during world teardown would otherwise re-enter OnSourceRemove against
  // half-freed pool storage. Keep ctor defaults (clear on_remove + ctx).
  if (flecs_world_ != nullptr && source_id_ != 0) {
    ecs_type_hooks_t no_reclaim{};
    no_reclaim.ctor = AudioSourceCtor;
    ecs_set_hooks_id(flecs_world_, source_id_, &no_reclaim);
  }
  ma_sound* pool = VoicesOf(voices_);
  // voice_count_ is the high-water mark; reclaimed slots were already
  // uninited at remove time — skip by owner record to avoid double uninit.
  for (std::uint32_t i = 0; i < voice_count_; ++i) {
    if (voice_entities_[i] != 0) {
      ma_sound_uninit(&pool[i]);
    }
  }
  if (listener_query_ != nullptr) {
    ecs_query_fini(listener_query_);
    listener_query_ = nullptr;
  }
  if (source_query_ != nullptr) {
    ecs_query_fini(source_query_);
    source_query_ = nullptr;
  }
  std::free(slot_has_prev_);
  std::free(prev_positions_);
  std::free(voice_entities_);
  std::free(free_slots_);
  std::free(pool);
  voices_ = nullptr;
  voice_entities_ = nullptr;
  prev_positions_ = nullptr;
  slot_has_prev_ = nullptr;
  free_slots_ = nullptr;
  free_count_ = 0;
  ma_engine_uninit(EngineOf(engine_));
  std::free(engine_);
  engine_ = nullptr;
  ma_resource_manager_uninit(ManagerOf(resource_manager_));
  std::free(resource_manager_);
  resource_manager_ = nullptr;
  flecs_world_ = nullptr;
  source_id_ = 0;
  listener_id_ = 0;
  world_transform_id_ = 0;
  max_voices_ = 0;
  voice_count_ = 0;
  listener_has_prev_ = false;
}

auto AudioWorld::attach_file(std::uint64_t entity,
                             const char* path) noexcept -> bool {
  if (engine_ == nullptr || entity == 0 || path == nullptr ||
      free_slots_ == nullptr) {
    return false;
  }
  auto* src = static_cast<AudioSource*>(ecs_get_mut_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr) {
    return false;  // entity lacks AudioSource
  }
  // NOTE: a never-attached source reads slot == kInvalidSlot (the Flecs
  // ctor hook placement-constructs the C++ defaults into fresh storage).
  // A slot assigned by attach_file is always < max_voices_; disambiguate
  // live slots from stale values with the entity recorded in
  // voice_entities_[slot]. voice_entities_ is calloc'd (all-zero = no
  // owner); entity ids are never 0, so equality means "this entity owns
  // the slot". Attach then claims the slot by writing BOTH the component
  // and the owner record.
  //
  // IMPORTANT: src was fetched BEFORE ma_sound_init_from_file below, and
  // that call can relocate Flecs tables (component hooks / deferred
  // commands from other threads' resource-manager callbacks). Never trust
  // src->slot across it: re-fetch after success and decide on the fresh
  // value. The only "already attached" signal we honor is the owner
  // record matching THIS entity (idempotent re-attach of the same file).
  if ((src->slot != AudioSource::kInvalidSlot) &&
      (src->slot < max_voices_) &&
      (voice_entities_[src->slot] == entity)) {
    return true;  // idempotent: this entity already owns its slot
  }
  // Reserve BEFORE init so a full pool / missing file needs no rollback:
  // AllocSlot pops a reclaimed slot or bumps the high-water mark.
  const std::uint32_t slot = AllocSlot();
  if (slot == AudioSource::kInvalidSlot) {
    return false;  // pool full
  }
  ma_sound* pool = VoicesOf(voices_);
  // STREAM: decode on the resource-manager workers, never blocking the
  // mixer or this thread on the full file. ASYNC + WAIT_INIT: init
  // returns after the header parses (format known) while PCM streams in
  // behind it — bounded call latency, lock-free playback start.
  const ma_uint32 flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM |
                          MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC;
  if (ma_sound_init_from_file(EngineOf(engine_), path, flags, nullptr,
                              nullptr, &pool[slot]) != MA_SUCCESS) {
    // Init failed: hand the reservation back. No on_remove ran (entity
    // untouched), so push directly instead of ReclaimSlot (which would
    // ma_sound_uninit a never-initialized slot — UB).
    if (free_count_ < max_voices_) {
      free_slots_[free_count_] = slot;
      ++free_count_;
    }
    return false;
  }
  ma_sound_set_spatialization_enabled(&pool[slot], MA_TRUE);
  voice_entities_[slot] = entity;
  // Claim AFTER the ma_sound init succeeds + re-fetch (table may have
  // moved): record the slot. Component defaults (volume/pitch=1,
  // distances, cone, doppler, attenuation) are already in place via the
  // Flecs ctor hooks registered in construct() — no normalization here,
  // so designer values set before attach are never overwritten.
  {
    auto* claim = static_cast<AudioSource*>(ecs_get_mut_id(
        flecs_world_, static_cast<ecs_entity_t>(entity),
        static_cast<ecs_entity_t>(source_id_)));
    if (claim == nullptr) {
      // Entity lost AudioSource mid-init (on_remove fired and reclaimed
      // this exact slot): ReclaimSlot already uninited the voice and
      // re-pushed the slot, so just return — no double uninit here.
      return false;
    }
    claim->slot = slot;
  }
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
      src->slot >= max_voices_ || voice_entities_[src->slot] != entity) {
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
  if (engine_ == nullptr || flecs_world_ == nullptr ||
      listener_query_ == nullptr || source_query_ == nullptr) {
    return false;
  }
  SyncListener(dt);
  SyncSources(dt);
  return true;
}

// Listener stage: the first AudioListener entity (lowest id) drives
// ma_engine_listener_* (position/forward/velocity from WorldTransform).
// First-frame Doppler guard: ear velocity stays 0 until the second update
// with a listener (avoids a one-frame pitch spike from differentiating
// against miniaudio's default origin). Two-level nesting max.
auto AudioWorld::SyncListener(float dt) noexcept -> void {
  ma_engine* eng = EngineOf(engine_);
  ecs_iter_t it = ecs_query_iter(flecs_world_, listener_query_);
  std::uint64_t best = 0;
  bool have_listener = false;
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
      }
    }
  }
  if (!have_listener) {
    return;
  }
  float ear_xyz[3] = {0.0F, 0.0F, 0.0F};
  float ear_fwd[3] = {0.0F, 0.0F, -1.0F};
  float ear_vel[3] = {0.0F, 0.0F, 0.0F};
  bool have_xf = false;
  const ecs_entity_t wt = static_cast<ecs_entity_t>(world_transform_id_);
  if (wt != 0) {
    const auto* w = static_cast<const float*>(
        ecs_get_id(flecs_world_, static_cast<ecs_entity_t>(best), wt));
    if (w != nullptr) {
      have_xf = true;
      ear_xyz[0] = w[kTx];
      ear_xyz[1] = w[kTy];
      ear_xyz[2] = w[kTz];
      WorldForward(w, ear_fwd);
      // Finite-difference on the engine's own last-set position (exact,
      // no extra storage). Skipped on the first frame with a listener.
      if (dt > 0.0F && listener_has_prev_) {
        const ma_vec3f prev = ma_engine_listener_get_position(eng, 0);
        ear_vel[0] = (ear_xyz[0] - prev.x) / dt;
        ear_vel[1] = (ear_xyz[1] - prev.y) / dt;
        ear_vel[2] = (ear_xyz[2] - prev.z) / dt;
      }
    }
  }
  ma_engine_listener_set_position(eng, 0, ear_xyz[0], ear_xyz[1], ear_xyz[2]);
  ma_engine_listener_set_direction(eng, 0, ear_fwd[0], ear_fwd[1], ear_fwd[2]);
  ma_engine_listener_set_velocity(eng, 0, ear_vel[0], ear_vel[1], ear_vel[2]);
  // Missing transform: origin is not a valid prior — clear so the next
  // valid frame differentiates fresh instead of spiking from (0,0,0).
  listener_has_prev_ = have_xf;
}

// Source stage: every attached AudioSource entity gets position/direction
// (world matrix translation + -Z axis), finite-difference Doppler
// velocity, cone/attenuation/gain/pitch/loop, and play-state edges.
// Per-row work lives in SyncOneSource (two-level nesting max here).
auto AudioWorld::SyncSources(float dt) noexcept -> void {
  ecs_iter_t it = ecs_query_iter(flecs_world_, source_query_);
  while (ecs_query_next(&it)) {
    const auto* srcs = static_cast<const AudioSource*>(
        ecs_field_w_size(&it, sizeof(AudioSource), 0));
    if (srcs == nullptr || it.entities == nullptr) {
      continue;
    }
    for (int r = 0; r < it.count; ++r) {
      SyncOneSource(voices_, srcs[r], it.entities[r], dt);
    }
  }
}

// One attached row: validate slot ownership, then push the full voice
// state (position/direction/velocity/cone/attenuation/gain/play edge).
// Early-continues keep nesting at two levels (Sonar S134).
auto AudioWorld::SyncOneSource(void* pool_in, const AudioSource& s,
                               std::uint64_t entity, float dt) noexcept
    -> void {
  // A live slot is always < max_voices_; kInvalidSlot rows skip. The owner
  // record disowns stale slots (entity moved tables / slot reclaimed).
  if (s.slot == AudioSource::kInvalidSlot || s.slot >= max_voices_) {
    return;
  }
  if (voice_entities_[s.slot] != entity) {
    return;  // stale slot: entity does not own this voice
  }

  // Position / direction / cone from the entity's world matrix.
  // have_xf tracks whether a transform existed: without one the origin
  // is NOT a valid prior, so velocity/doppler state must clear.
  ma_sound* pool = VoicesOf(pool_in);
  ma_sound* voice = &pool[s.slot];
  float px = 0.0F;
  float py = 0.0F;
  float pz = 0.0F;
  float fw[3] = {0.0F, 0.0F, -1.0F};
  bool have_xf = false;
  const ecs_entity_t wt = static_cast<ecs_entity_t>(world_transform_id_);
  if (wt != 0) {
    const auto* w = static_cast<const float*>(ecs_get_id(
        flecs_world_, static_cast<ecs_entity_t>(entity), wt));
    if (w != nullptr) {
      have_xf = true;
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
  if (dt > 0.0F && have_xf && slot_has_prev_[s.slot] != 0) {
    float* pp = prev_positions_ + static_cast<std::size_t>(s.slot) * 3;
    ma_sound_set_velocity(voice, (px - pp[0]) / dt, (py - pp[1]) / dt,
                          (pz - pp[2]) / dt);
  } else {
    ma_sound_set_velocity(voice, 0.0F, 0.0F, 0.0F);
  }
  if (have_xf) {
    float* pp = prev_positions_ + static_cast<std::size_t>(s.slot) * 3;
    pp[0] = px;
    pp[1] = py;
    pp[2] = pz;
    slot_has_prev_[s.slot] = 1;
  } else {
    slot_has_prev_[s.slot] = 0;
  }

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

auto AudioWorld::voice_position(std::uint64_t entity,
                                float out_xyz[3]) const noexcept -> bool {
  if (engine_ == nullptr || out_xyz == nullptr) {
    return false;
  }
  const auto* src = static_cast<const AudioSource*>(ecs_get_id(
      flecs_world_, static_cast<ecs_entity_t>(entity),
      static_cast<ecs_entity_t>(source_id_)));
  if (src == nullptr || src->slot == AudioSource::kInvalidSlot ||
      src->slot >= max_voices_ ||
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
      src->slot >= max_voices_ ||
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
