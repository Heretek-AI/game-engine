// AudioApiTests.cs — P5-06: managed audio mixer contract gate.
//
// Verifies the C# audio surface:
//   * Type shape (no native needed): AudioSource/AudioListener blittable
//     + 64B, AudioBus ids, Audio type shape (SetBusVolumeDb/
//     GetBusVolumeDb/SetLpfCutoff/SetLpfEnabled/SetDelay/
//     SetDelayEnabled/RenderSineDb, IDisposable).
//   * Managed-stub functional probe (no native needed): drive the Audio
//     wrapper through a managed ApiTable built from UnmanagedCallersOnly
//     stubs, so the zero-alloc claim covers the real wrapper path.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     creates a real noDevice mixer and renders the -6 dB acceptance
//     sine (peak 0.5 ± 0.01) through the wrapper — mirrors the CTest.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class AudioApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<AudioSource>() == false, "AudioSource blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<AudioListener>() == false, "AudioListener blittable");
        failures += Check(typeof(Audio).GetMethod("SetBusVolumeDb") != null, "Audio.SetBusVolumeDb exists");
        failures += Check(typeof(Audio).GetMethod("GetBusVolumeDb") != null, "Audio.GetBusVolumeDb exists");
        failures += Check(typeof(Audio).GetMethod("SetLpfCutoff") != null, "Audio.SetLpfCutoff exists");
        failures += Check(typeof(Audio).GetMethod("SetLpfEnabled") != null, "Audio.SetLpfEnabled exists");
        failures += Check(typeof(Audio).GetMethod("SetDelay") != null, "Audio.SetDelay exists");
        failures += Check(typeof(Audio).GetMethod("SetDelayEnabled") != null, "Audio.SetDelayEnabled exists");
        failures += Check(typeof(Audio).GetMethod("RenderSineDb") != null, "Audio.RenderSineDb exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Audio)), "Audio IDisposable");
        failures += Check((uint)AudioBus.Master == 0, "AudioBus.Master == 0");
        failures += Check((uint)AudioBus.Music == 1, "AudioBus.Music == 1");
        failures += Check((uint)AudioBus.Sfx == 2, "AudioBus.Sfx == 2");

        unsafe
        {
            failures += Check(sizeof(AudioSource) == 64, "AudioSource 64B");
            failures += Check(sizeof(AudioListener) == 64, "AudioListener 64B");
            failures += Check(sizeof(NativeApi.EngineAudioWorld) == 8, "NativeApi.EngineAudioWorld opaque 8B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineAudioCreate", "EngineAudioDestroy", "EngineAudioSetBusVolumeDb", "EngineAudioGetBusVolumeDb", "EngineAudioSetLpfCutoff", "EngineAudioSetLpfEnabled", "EngineAudioSetDelay", "EngineAudioSetDelayEnabled", "EngineAudioRenderSineDb" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Managed-stub functional probe (no native needed) ------------
        var priorOverride = Audio.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineAudioCreate = &FakeAudio.StubCreate;
                stub.EngineAudioDestroy = &FakeAudio.StubDestroy;
                stub.EngineAudioSetBusVolumeDb = &FakeAudio.StubSetBusVolumeDb;
                stub.EngineAudioGetBusVolumeDb = &FakeAudio.StubGetBusVolumeDb;
                stub.EngineAudioSetLpfCutoff = &FakeAudio.StubSetLpfCutoff;
                stub.EngineAudioSetLpfEnabled = &FakeAudio.StubSetLpfEnabled;
                stub.EngineAudioSetDelay = &FakeAudio.StubSetDelay;
                stub.EngineAudioSetDelayEnabled = &FakeAudio.StubSetDelayEnabled;
                stub.EngineAudioRenderSineDb = &FakeAudio.StubRenderSineDb;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Audio.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    FakeAudio.Reset();
                    using var fa = new Audio(noDevice: true);
                    failures += Check(FakeAudio.CreateCount == 1, "stub create reached (count=1)");

                    fa.SetBusVolumeDb(AudioBus.Music, -6.0f);
                    failures += Check(FakeAudio.LastBus == 1 && Math.Abs(FakeAudio.LastDb - (-6.0f)) < 1e-6f, "stub SetBusVolumeDb reaches native entry (bus=1, db=-6)");
                    failures += Check(Math.Abs(fa.GetBusVolumeDb(AudioBus.Music) - (-6.0f)) < 1e-6f, "stub GetBusVolumeDb round-trips");

                    fa.SetLpfCutoff(800.0f);
                    failures += Check(Math.Abs(FakeAudio.LastCutoff - 800.0f) < 1e-3f, "stub SetLpfCutoff reaches native entry");
                    fa.SetLpfEnabled(true);
                    failures += Check(FakeAudio.LpfEnabled == 1, "stub SetLpfEnabled(true) reaches native entry");
                    fa.SetLpfEnabled(false);
                    failures += Check(FakeAudio.LpfEnabled == 0, "stub SetLpfEnabled(false) reaches native entry");

                    fa.SetDelay(250.0f, 0.5f, 0.7f, 0.5f);
                    failures += Check(FakeAudio.DelayCalls == 1, "stub SetDelay reaches native entry");
                    fa.SetDelayEnabled(true);
                    failures += Check(FakeAudio.DelayEnabled == 1, "stub SetDelayEnabled(true) reaches native entry");

                    Span<float> buf = stackalloc float[1024];
                    var n = fa.RenderSineDb(AudioBus.Music, 440.0f, -6.0f, buf);
                    failures += Check(n == 1024, "stub RenderSineDb returns span length");

                    // Warm up once (JIT + first-call caches), then measure.
                    FakeAudio.SteadyDrive(fa);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeAudio.SteadyDrive(fa);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"Audio wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    Audio.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Audio.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // -6 dB acceptance through the REAL wrapper: 1 s of 440 Hz on the
        // Music bus must peak at 0.5 ± 0.01 (same assertion as CTest).
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = AudioLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            var savedOverride = Audio.TableOverride;
            try
            {
                if (preloaded != IntPtr.Zero)
                {
                    AudioLivePreload.LoadIntoApiLoader(preloaded);
                    table = (NativeApi.ApiTable*)Audio.TableOverride;
                }
                else if (liveProbe)
                {
                    table = NativeApi.ApiLoader.Load();
                    if (table == null)
                    {
                        Console.WriteLine("AudioApiTests: live probe requested but engine_core unloadable — FAIL");
                        return failures + 1;
                    }
                }
                else
                {
                    Console.WriteLine("AudioApiTests: shape + stub checks passed (live native skipped, pass --live-native-probe with staged engine_core)");
                    Console.WriteLine(failures == 0 ? "AudioApiTests: all checks passed" : $"AudioApiTests: {failures} FAILED");
                    return failures;
                }

                failures += Check(table->EngineAudioCreate != null, "live: audio_create non-null");
                failures += Check(table->EngineAudioRenderSineDb != null, "live: audio_render_sine_db non-null");
                failures += Check(table->EngineAudioSetBusVolumeDb(null, 1, -6.0f) != 0, "live: null world set_bus_volume_db rejected");
                failures += Check(table->EngineAudioRenderSineDb(null, 1, 440.0f, -6.0f, null, 1024) == 0, "live: null world render -> 0 (no crash)");

                if (table->EngineAudioCreate == null || table->EngineAudioRenderSineDb == null)
                {
                    Console.WriteLine(failures == 0 ? "AudioApiTests: all checks passed" : $"AudioApiTests: {failures} FAILED");
                    return failures;
                }

                using var mixer = new Audio(noDevice: true);
                mixer.SetBusVolumeDb(AudioBus.Music, -6.0f);
                failures += Check(Math.Abs(mixer.GetBusVolumeDb(AudioBus.Music) - (-6.0f)) < 1e-6f, "live: music bus -6dB read-back");

                var buf = new float[48000];
                var rendered = mixer.RenderSineDb(AudioBus.Music, 440.0f, -6.0f, buf);
                failures += Check(rendered == 48000, $"live: rendered full second (got {rendered})");
                var peak = 0.0f;
                for (var i = 0; i < rendered; i++)
                {
                    var v = Math.Abs(buf[i]);
                    if (v > peak)
                    {
                        peak = v;
                    }
                }

                Console.WriteLine($"AudioApiTests: live -6dB peak={peak:F6} (expect ~0.501187)");
                failures += Check(Math.Abs(peak - 0.5f) <= 0.01f, $"live: -6dB sine peaks at 0.5 +- 0.01 (got {peak:F6})");
            }
            finally
            {
                Audio.TableOverride = savedOverride;
            }
        }
        catch (Exception ex)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live native probe threw: {ex.GetType().Name}: {ex.Message}");
            }
            else
            {
                Console.WriteLine($"AudioApiTests: live native skipped ({ex.GetType().Name})");
            }
        }

        Console.WriteLine(failures == 0 ? "AudioApiTests: all checks passed" : $"AudioApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static unsafe class FakeAudio
    {
        public static int CreateCount;
        public static uint LastBus;
        public static float LastDb;
        public static float LastCutoff;
        public static int LpfEnabled;
        public static int DelayCalls;
        public static int DelayEnabled;

        public static void Reset()
        {
            CreateCount = 0;
            LastBus = 0;
            LastDb = 0;
            LastCutoff = 0;
            LpfEnabled = 0;
            DelayCalls = 0;
            DelayEnabled = 0;
        }

        public static void SteadyDrive(Audio a)
        {
            a.SetBusVolumeDb(AudioBus.Music, -6.0f);
            _ = a.GetBusVolumeDb(AudioBus.Music);
            Span<float> buf = stackalloc float[64];
            _ = a.RenderSineDb(AudioBus.Music, 440.0f, -6.0f, buf);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe NativeApi.EngineAudioWorld* StubCreate(int noDevice, uint sampleRate, uint channels)
        {
            CreateCount++;
            return (NativeApi.EngineAudioWorld*)0x1000;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe void StubDestroy(NativeApi.EngineAudioWorld* audio)
        {
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetBusVolumeDb(NativeApi.EngineAudioWorld* audio, uint bus, float db)
        {
            LastBus = bus;
            LastDb = db;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubGetBusVolumeDb(NativeApi.EngineAudioWorld* audio, uint bus, float* outDb)
        {
            if (outDb != null)
            {
                *outDb = LastDb;
            }

            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetLpfCutoff(NativeApi.EngineAudioWorld* audio, float cutoffHz)
        {
            LastCutoff = cutoffHz;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetLpfEnabled(NativeApi.EngineAudioWorld* audio, int enabled)
        {
            LpfEnabled = enabled;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetDelay(NativeApi.EngineAudioWorld* audio, float delayMs, float wet, float dry, float decay)
        {
            DelayCalls++;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetDelayEnabled(NativeApi.EngineAudioWorld* audio, int enabled)
        {
            DelayEnabled = enabled;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe uint StubRenderSineDb(NativeApi.EngineAudioWorld* audio, uint bus, float freqHz, float volumeDb, float* @out, uint frames)
        {
            return frames;
        }
    }

    private static class AudioLivePreload
    {
        public static IntPtr TryPreloadEngineCoreHandle()
        {
            var seeds = new List<string> { AppContext.BaseDirectory };
            seeds.Add(Directory.GetCurrentDirectory());
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var seed in seeds)
            {
                DirectoryInfo? dir = null;
                try
                {
                    dir = new DirectoryInfo(seed);
                }
                catch (Exception)
                {
                    continue;
                }

                for (var i = 0; i < 12 && dir != null; i++, dir = dir.Parent)
                {
                    var key = dir.FullName;
                    if (!seen.Add(key))
                    {
                        continue;
                    }

                    if (File.Exists(Path.Combine(key, "Engine.slnx")))
                    {
                        foreach (var seedDir in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
                        {
                            foreach (var stagedName in new[] { "libengine_core.dylib", "engine_core.dll", "libengine_core.so", "libengine_core.so.0.1.0", "libengine_core.so.0" })
                            {
                                var staged = Path.Combine(seedDir, stagedName);
                                if (File.Exists(staged))
                                {
                                    var h = PreloadPair(Path.GetFullPath(staged));
                                    if (h != IntPtr.Zero)
                                    {
                                        return h;
                                    }
                                }
                            }
                        }

                        return IntPtr.Zero;
                    }
                }
            }

            return IntPtr.Zero;
        }

        public static unsafe void LoadIntoApiLoader(IntPtr handle)
        {
            if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
            {
                throw new EntryPointNotFoundException("engine_get_api_table not exported.");
            }

            var getTable = (delegate* unmanaged[Cdecl, SuppressGCTransition]<NativeApi.ApiTable*>)addr;
            var table = getTable();
            if (table == null)
            {
                throw new InvalidOperationException("engine_get_api_table returned null.");
            }

            if (table->Size < (ulong)sizeof(NativeApi.ApiTable))
            {
                throw new InvalidOperationException($"Native ABI table too small: {table->Size}.");
            }

            if (table->AbiVersionMajor != NativeApi.AbiVersionMajor || table->AbiVersionMinor != NativeApi.AbiVersionMinor || table->AbiVersionPatch != NativeApi.AbiVersionPatch)
            {
                throw new InvalidOperationException($"ABI version mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch}.");
            }

            Audio.TableOverride = (IntPtr)table;
        }

        private static IntPtr PreloadPair(string corePath)
        {
            try
            {
                var dir = Path.GetDirectoryName(corePath);
                if (dir != null)
                {
                    foreach (var flecsName in new[] { "libflecs.so", "libflecs.dylib", "flecs.dll" })
                    {
                        var flecs = Path.Combine(dir, flecsName);
                        if (File.Exists(flecs))
                        {
                            try
                            {
                                NativeLibrary.Load(flecs);
                            }
                            catch (Exception)
                            {
                            }

                            break;
                        }
                    }
                }
            }
            catch (Exception)
            {
            }

            try
            {
                return NativeLibrary.Load(corePath);
            }
            catch (Exception)
            {
                return IntPtr.Zero;
            }
        }
    }
}
