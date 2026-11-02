// test_audio_mixer.cpp — P5-06 acceptance: mixer buses + effects + dB.
//
// AC1: offline (noDevice) sine render through a bus at −6 dB peaks at
//      0.5 ± 0.01 (ma_volume_db_to_linear path, bit-clean with LPF +
//      delay detached).
// AC2: bus isolation — Music at −6 dB renders ~0.5, Master at −20 dB
//      renders ~0.1; SFX bus record + volume read-back.
// AC3: LPF insert — attach, 440 Hz passes (~1.0), 8 kHz attenuated;
//      detach restores; cutoff round-trip + clamping.
// AC4: delay insert — wet/dry/decay round-trip, enable/disable; dB
//      clamping at the rails.
// AC5: static pins — AudioSource still 64B/align64 with the bus field;
//      set_voice_bus refuses bad bus / attached voice / unknown entity;
//      render_sine_db refuses bad args; update on destroyed world false.
//
// Headless-safe: noDevice=true throughout.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/audio/audio_world.hpp"
#include "engine/audio/components.hpp"
#include "engine/audio/miniaudio.hpp"
#include "engine/ecs/ecs_world.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

auto Near(float a, float b, float eps) -> bool {
  float d = a - b;
  if (d < 0.0F) {
    d = -d;
  }
  return d <= eps;
}

auto Peak(const float* buf, std::uint32_t n) -> float {
  float peak = 0.0F;
  for (std::uint32_t i = 0; i < n; ++i) {
    float v = buf[i] < 0.0F ? -buf[i] : buf[i];
    if (v > peak) {
      peak = v;
    }
  }
  return peak;
}

}  // namespace

auto main() -> int {
  int failures = 0;

  // --- 0. Static layout pins (bus field must not break 64B) ---------------
  static_assert(sizeof(engine::audio::AudioSource) == 64, "source 64B");
  static_assert(alignof(engine::audio::AudioSource) == 64, "source align");
  static_assert(offsetof(engine::audio::AudioSource, bus) + 4 <= 64,
                "bus field in bounds");
  failures += check(true, "mixer: 64B static asserts with bus field");

  engine::ecs::EcsWorld ecs;
  failures += check(ecs.construct(), "mixer: ecs construct");
  engine::audio::AudioWorld audio;
  failures += check(audio.construct(ecs.raw(), /*no_device=*/true),
                    "mixer: audio construct (noDevice)");
  failures += check(audio.mixer() != nullptr, "mixer: mixer present");
  failures += check(audio.mixer()->valid(), "mixer: mixer valid");

  // --- 1. AC1: −6 dB sine peaks at 0.5 ± 0.01 -------------------------------
  constexpr std::uint32_t kFrames = 48000;
  static float out[kFrames];
  const std::uint32_t got =
      audio.render_sine_db(/*bus=*/1, 440.0F, -6.0F, out, kFrames);
  failures += check(got == kFrames, "ac1: rendered full second");
  const float peak = Peak(out, got);
  std::printf("[INFO] ac1: -6dB 440Hz peak=%.6f (expect ~0.501187)\n", peak);
  failures += check(Near(peak, 0.5F, 0.01F),
                    "ac1: -6dB sine peaks at 0.5 +- 0.01");

  // --- 2. AC2: bus isolation + volume read-back -----------------------------
  failures += check(audio.mixer()->set_bus_volume_db(
                        engine::audio::AudioBus::kMaster, -20.0F),
                    "ac2: master to -20dB");
  failures += check(
      Near(audio.mixer()->bus_volume_db(engine::audio::AudioBus::kMaster),
           -20.0F, 1e-6F),
      "ac2: master volume read-back");
  const std::uint32_t got2 =
      audio.render_sine_db(/*bus=*/0, 440.0F, -20.0F, out, kFrames);
  const float peak2 = Peak(out, got2);
  std::printf("[INFO] ac2: -20dB 440Hz peak=%.6f (expect ~0.1)\n", peak2);
  failures += check(Near(peak2, 0.1F, 0.01F),
                    "ac2: -20dB sine peaks at 0.1 +- 0.01");
  failures += check(audio.mixer()->set_bus_volume_db(
                        engine::audio::AudioBus::kMusic, -6.0F),
                    "ac2: music to -6dB stays");
  // Restore Master before the effect tests: Music/SFX parent to Master,
  // so the -20dB above would otherwise scale every later render.
  failures += check(audio.mixer()->set_bus_volume_db(
                        engine::audio::AudioBus::kMaster, 0.0F),
                    "ac2: master back to 0dB");
  // dB clamping at the rails.
  failures += check(audio.mixer()->set_bus_volume_db(
                        engine::audio::AudioBus::kSfx, -100.0F),
                    "ac2: sfx under-range clamps (accepts)");
  failures += check(
      Near(audio.mixer()->bus_volume_db(engine::audio::AudioBus::kSfx),
           -80.0F, 1e-6F),
      "ac2: sfx clamped to -80dB rail");
  failures += check(audio.mixer()->set_bus_volume_db(
                        engine::audio::AudioBus::kSfx, 0.0F),
                    "ac2: sfx back to 0dB");
  failures += check(!audio.mixer()->set_bus_volume_db(
                        static_cast<engine::audio::AudioBus>(7), -6.0F),
                    "ac2: invalid bus refuses");

  // --- 3. AC3: LPF insert (series splice on the Music bus) ------------
  failures += check(audio.mixer()->set_lpf_bus(1),
                    "ac3: lpf targets music bus");
  failures += check(audio.mixer()->set_lpf_cutoff(800.0F),
                    "ac3: lpf cutoff 800Hz");
  failures += check(Near(audio.mixer()->lpf_cutoff(), 800.0F, 1e-6F),
                    "ac3: lpf cutoff read-back");
  failures += check(!audio.mixer()->lpf_enabled(),
                    "ac3: lpf detached by default");
  failures += check(audio.mixer()->set_lpf_enabled(true),
                    "ac3: lpf attach (series)");
  failures += check(audio.mixer()->lpf_enabled(),
                    "ac3: lpf enabled read-back");
  const std::uint32_t got_lo =
      audio.render_sine_db(/*bus=*/1, 440.0F, 0.0F, out, kFrames);
  const float peak_lo = Peak(out, got_lo);
  std::printf("[INFO] ac3: 440Hz thru series LPF peak=%.6f (expect ~0.96)\n",
              peak_lo);
  failures += check(Near(peak_lo, 0.957F, 0.02F),
                    "ac3: 440Hz passes 800Hz series LPF");
  const std::uint32_t got_hi =
      audio.render_sine_db(/*bus=*/1, 8000.0F, 0.0F, out, kFrames);
  const float peak_hi = Peak(out, got_hi);
  std::printf("[INFO] ac3: 8kHz thru series LPF peak=%.6f (expect ~0.05)\n",
              peak_hi);
  failures += check(peak_hi < 0.2F,
                    "ac3: 8kHz attenuated by 800Hz series LPF");
  failures += check(audio.mixer()->set_lpf_enabled(false),
                    "ac3: lpf detach (bus restored)");
  failures += check(!audio.mixer()->lpf_enabled(),
                    "ac3: lpf disabled read-back");
  // Post-detach the bus routes straight to the endpoint again: full peak.
  const std::uint32_t got_post =
      audio.render_sine_db(/*bus=*/1, 440.0F, 0.0F, out, kFrames);
  failures += check(Near(Peak(out, got_post), 1.0F, 0.02F),
                    "ac3: detach restores direct bus route");
  failures += check(audio.mixer()->set_lpf_cutoff(1e9F),
                    "ac3: cutoff over-range clamps (accepts)");
  failures += check(Near(audio.mixer()->lpf_cutoff(), 20000.0F, 1e-3F),
                    "ac3: cutoff clamped to 20kHz rail");

  // --- 4. AC4: delay insert ---------------------------------------------------
  failures += check(
      audio.mixer()->set_delay(250.0F, 0.5F, 0.7F, 0.5F),
      "ac4: delay set 250ms/0.5/0.7/0.5");
  failures += check(!audio.mixer()->delay_enabled(),
                    "ac4: delay detached by default");
  failures += check(audio.mixer()->set_delay_enabled(true),
                    "ac4: delay attach");
  failures += check(audio.mixer()->delay_enabled(),
                    "ac4: delay enabled read-back");
  failures += check(audio.mixer()->set_delay_enabled(false),
                    "ac4: delay detach");
  failures += check(!audio.mixer()->delay_enabled(),
                    "ac4: delay disabled read-back");

  // --- 5. Edge cases ----------------------------------------------------------
  failures += check(audio.render_sine_db(7, 440.0F, -6.0F, out, 1024) == 0,
                    "mixer: render bad bus refuses");
  failures += check(audio.render_sine_db(1, 0.0F, -6.0F, out, 1024) == 0,
                    "mixer: render zero freq refuses");
  failures += check(audio.render_sine_db(1, 440.0F, -6.0F, nullptr, 1024) == 0,
                    "mixer: render null out refuses");
  failures += check(audio.render_sine_db(1, 440.0F, -6.0F, out, 0) == 0,
                    "mixer: render zero frames refuses");
  failures += check(!audio.set_voice_bus(0, 1),
                    "mixer: set_voice_bus null entity refuses");
  failures += check(!audio.set_voice_bus(12345, 7),
                    "mixer: set_voice_bus bad bus refuses");

  audio.destroy();
  failures += check(!audio.update(1.0F / 60.0F),
                    "mixer: update after destroy refuses");
  ecs.destroy();

  if (failures == 0) {
    std::printf("AUDIO_MIXER_OK peak-6db=~0.5 lpf+delay\n");
  }
  return failures == 0 ? 0 : 1;
}
