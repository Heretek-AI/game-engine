// Audio.cs — P5-06: managed audio mixer over the ABI 4.11 audio_* tail.
//
// Wraps an EngineAudioWorld (owns a miniaudio engine + Master/Music/SFX
// buses + LPF/delay inserts). Volumes are exposed in dB (the native side
// converts with ma_volume_db_to_linear); the C# surface never deals in
// linear gain. All entries are plain Cdecl (render may block on the
// mixer — never SuppressGCTransition, same rule as run_systems).
//
// AudioSource / AudioListener are the C# component structs: blittable
// 64B mirrors of the native components (bus field included on the
// source). Layout is pinned in AbiLayoutTests.
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Mixer bus id. Mirrors engine::audio::AudioBus.</summary>
public enum AudioBus : uint
{
    Master = 0,
    Music = 1,
    Sfx = 2,
}

/// <summary>
/// Managed per-entity 3D voice. Blittable 64-byte mirror of the native
/// AudioSource (slot/volume/pitch/distances/cone/doppler/attenuation +
/// bus + loop/playing/spatialized).
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]
public struct AudioSource
{
    public uint Slot;
    public float Volume;
    public float Pitch;
    public float MinDistance;
    public float MaxDistance;
    public float InnerConeAngle;
    public float OuterConeAngle;
    public float OuterConeGain;
    public float DopplerFactor;
    public uint Attenuation;
    public uint Bus;
    public int Loop;
    public int Playing;
    public int Spatialized;
#pragma warning disable CA1051 // Blittable interop struct by design.
    private unsafe fixed byte _pad[8];
#pragma warning restore CA1051
}

/// <summary>
/// Managed ears marker. Blittable 64-byte mirror of AudioListener.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]
public struct AudioListener
{
    public float DopplerFactor;
#pragma warning disable CA1051 // Blittable interop struct by design.
    private unsafe fixed byte _pad[60];
#pragma warning restore CA1051
}

/// <summary>
/// Managed owner of a native audio mixer: bus volumes in dB, LPF/delay
/// inserts, and the offline sine-render probe. The handle is the native
/// EngineAudioWorld pointer (borrowed — destroy this only after use).
/// </summary>
public sealed unsafe class Audio : IDisposable
{
    private NativeApi.EngineAudioWorld* _world;
    private bool _disposed;

    // Test/live-probe injection (same pattern as Physics2D).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Creates a native audio mixer (noDevice=true: headless/offline).</summary>
    public Audio(bool noDevice = true, uint sampleRate = 0, uint channels = 0)
    {
        var world = TablePtr->EngineAudioCreate(noDevice ? 1 : 0, sampleRate, channels);
        if (world == null)
        {
            throw new InvalidOperationException("audio_create failed (allocation failure).");
        }

        _world = world;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_world != null)
            {
                TablePtr->EngineAudioDestroy(_world);
                _world = null;
            }
        }
    }

    /// <summary>Sets a bus volume in dB ([-80, +24], clamped natively).</summary>
    public void SetBusVolumeDb(AudioBus bus, float db)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAudioSetBusVolumeDb(_world, (uint)bus, db) != 0)
        {
            throw new InvalidOperationException($"audio_set_bus_volume_db failed (bus {(uint)bus}).");
        }
    }

    /// <summary>Reads back the last-set bus volume in dB.</summary>
    public float GetBusVolumeDb(AudioBus bus)
    {
        ThrowIfDisposed();
        float db = 0;
        if (TablePtr->EngineAudioGetBusVolumeDb(_world, (uint)bus, &db) != 0)
        {
            throw new InvalidOperationException($"audio_get_bus_volume_db failed (bus {(uint)bus}).");
        }

        return db;
    }

    /// <summary>Sets the LPF cutoff in Hz ([20, 20000], clamped).</summary>
    public void SetLpfCutoff(float cutoffHz)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAudioSetLpfCutoff(_world, cutoffHz) != 0)
        {
            throw new InvalidOperationException("audio_set_lpf_cutoff failed.");
        }
    }

    /// <summary>Attaches (true) / detaches (false) the LPF insert.</summary>
    public void SetLpfEnabled(bool enabled)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAudioSetLpfEnabled(_world, enabled ? 1 : 0) != 0)
        {
            throw new InvalidOperationException("audio_set_lpf_enabled failed.");
        }
    }

    /// <summary>Sets the delay insert (ms, wet, dry, decay — all clamped).</summary>
    public void SetDelay(float delayMs, float wet, float dry, float decay)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAudioSetDelay(_world, delayMs, wet, dry, decay) != 0)
        {
            throw new InvalidOperationException("audio_set_delay failed.");
        }
    }

    /// <summary>Attaches (true) / detaches (false) the delay insert.</summary>
    public void SetDelayEnabled(bool enabled)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAudioSetDelayEnabled(_world, enabled ? 1 : 0) != 0)
        {
            throw new InvalidOperationException("audio_set_delay_enabled failed.");
        }
    }

    /// <summary>
    /// Renders `frames` of a sine at `freqHz` through `bus` at `volumeDb`
    /// dB into `destination` (mono float32). Returns frames rendered.
    /// </summary>
    public int RenderSineDb(AudioBus bus, float freqHz, float volumeDb, Span<float> destination)
    {
        ThrowIfDisposed();
        if (destination.IsEmpty)
        {
            return 0;
        }

        fixed (float* outPtr = destination)
        {
            return (int)TablePtr->EngineAudioRenderSineDb(_world, (uint)bus, freqHz, volumeDb, outPtr, (uint)destination.Length);
        }
    }
}
