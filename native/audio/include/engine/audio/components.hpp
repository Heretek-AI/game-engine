// components.hpp — M4-04: native ECS audio components.
//
// AudioSource: per-entity voice state. AudioListener: the single ears
// entity (usually the camera). Both are 64-byte aligned (Flecs column
// rule) and blittable.
//
// The spatializer system (audio_world) reads WorldTransform columns for
// positions/directions and pushes them into miniaudio each frame:
//   * source position  <- WorldTransform translation
//   * source velocity  <- (pos - prev_pos) / dt (frame Doppler)
//   * source cone      <- direction from WorldTransform -Z axis,
//                        inner/outer angles + outer gain
//   * listener         <- first AudioListener entity's WorldTransform
//                        (position + -Z forward + velocity)
//
// Attenuation model + rolloff live on the source so designers pick
// per-sound curves. The asset path is NOT stored here — M5 owns asset
// identity (GUIDs); the AudioWorld maps entity -> loaded ma_sound by id.
#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::audio {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding by design.
#endif

// Attenuation curve applied by the spatializer each frame.
enum class AttenuationModel : std::uint8_t {
  kNone = 0,       // unattenuated (UI / music bus)
  kInverse = 1,    // miniaudio inverse-distance (default, physical)
  kLinear = 2,     // linear falloff to max_distance
  kExponential = 3  // exponential falloff to max_distance
};

// Per-entity 3D voice. Bind: entity must also carry WorldTransform.
// Slot index into AudioWorld's fixed voice pool; assigned by
// AudioWorld::attach_file, kInvalidSlot until attached. Fresh Flecs
// storage is placement-constructed via the ctor hooks in construct(),
// so the in-class initializers below DO apply to Flecs-owned instances.
struct alignas(64) AudioSource {
  static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFEu;
  // NOTE: AudioWorld::construct() registers Flecs ctor hooks that
  // placement-construct AudioSource{}/AudioListener{} into fresh storage,
  // so these in-class initializers apply to Flecs-owned instances too.
  std::uint32_t slot{kInvalidSlot};  // voice pool slot (runtime-assigned)
  float volume{1.0f};                // linear gain [0, 1+]
  float pitch{1.0f};                 // resample pitch (> 0)
  float min_distance{1.0f};          // full-volume radius (m)
  float max_distance{100.0f};        // silence radius (linear/exp models)
  float inner_cone_angle{6.2831853f};  // radians (2pi = omnidirectional)
  float outer_cone_angle{6.2831853f};  // radians
  float outer_cone_gain{0.0f};          // gain outside outer cone [0,1]
  float doppler_factor{1.0f};           // 0 = disable Doppler for this voice
  AttenuationModel attenuation{AttenuationModel::kInverse};
  std::uint32_t bus{0};  // P5-06 mixer bus: 0 Master, 1 Music, 2 SFX
  bool loop{false};
  bool playing{false};  // desired state; edge-detected by the sync system
  bool spatialized{true};
  // 20 + 32 + 4 (+pad) = 56 used; pad to 64.
  std::uint8_t reserved[8];
};

// The ears. Only the FIRST listener entity (lowest id) drives
// ma_engine_listener_* each frame; extras are ignored deterministically.
struct alignas(64) AudioListener {
  float doppler_factor{1.0f};
  std::uint8_t reserved[60];
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(AudioSource) == 64, "AudioSource must be one cache line");
static_assert(sizeof(AudioListener) == 64,
              "AudioListener must be one cache line");
static_assert(alignof(AudioSource) == 64, "AudioSource alignas(64)");
static_assert(alignof(AudioListener) == 64, "AudioListener alignas(64)");

}  // namespace engine::audio
