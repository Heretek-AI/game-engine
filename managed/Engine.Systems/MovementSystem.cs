// MovementSystem.cs — M1-05: the zero-copy benchmark kernel.
//
// Entry point loaded by native DotNetHost via
// load_assembly_and_get_function_pointer with delegate type
// UNMANAGEDCALLERSONLY_METHOD: native calls fn(EcsChunkView*) directly,
// no delegate, no marshalling, no GC transition on the frame path.
//
// Contract (matches ManagedSystemCallback in engine_abi.h):
//   static void MoveUnrolled(EcsChunkView* view)  [UnmanagedCallersOnly, Cdecl]
//   * view->Local   = LocalTransform column (64B stride, owned by native)
//   * view->UserData = Velocity column base (16B stride, owned by native;
//     rides user_data when the registration context is null — see
//     native/bridge/src/engine_abi.cpp)
//   * view->Count   = rows in this chunk (> 0)
//   * frame dt arrives via SetDt (UCO, bound by hostfxr like Move — a plain
//     static cannot be bound through load_assembly_and_get_function_pointer;
//     set once per frame before run_systems, never allocated, never boxed).
//   SetDt stays OUT of EcsChunkView deliberately: the M1-04 ABI (MAJOR 1)
//   freezes the view layout and the callback signature, and user_data
//   already carries the Velocity column — there is no spare channel for dt
//   short of an ABI break. The publish is race-free by construction: SetDt
//   runs on the benchmark thread before run_systems, lanes only read s_dt
//   while disjoint rows integrate, and every lane joins before run_systems
//   returns — so each dispatch integrates with its own frame's frozen dt,
//   never a torn or cross-frame value.
//
// Frame-path rules (ENG0001-3 class invariants, enforced by review):
//   * zero managed allocations (no closures, no LINQ, no string ops),
//   * no throws (an exception escaping to native is fatal),
//   * thread-safe: called on native task lanes with per-chunk locals plus
//     the frame-frozen s_dt (parallel M1-05 dispatch over disjoint row
//     ranges — never the same row twice).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Systems;

/// <summary>
/// Zero-copy integrators over native Flecs columns. Stateless: all frame
/// state lives in native tables; managed code only views it.
/// </summary>
public static unsafe class MovementSystem
{
    // Frame delta, published by SetDt before each run_systems call.
    // Read-only during dispatch (all lanes join before the next publish),
    // so every lane in a dispatch sees its own frame's frozen dt.
    private static float s_dt;

    /// <summary>
    /// Publish the frame delta (seconds) for the next update. UCO so the
    /// native host can bind it through load_assembly_and_get_function_pointer.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void SetDt(float dt) => s_dt = dt;

    /// <summary>
    /// 4-wide unrolled zero-copy integrate: local.px += vel.vx * dt (etc.)
    /// over every row of the chunk. Called once per row-range shard on a
    /// native task lane. The unroll keeps four row pointers live in
    /// registers, hiding table latency the scalar loop stalls on.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void MoveUnrolled(NativeApi.EcsChunkView* view)
    {
        var local = (float*)view->Local;
        var vel = (float*)view->UserData;
        int count = view->Count;
        float dt = s_dt;
        int n4 = count & ~3;
        for (int i = 0; i < n4; i += 4)
        {
            float* r0 = local + (i * 16);
            float* v0 = vel + (i * 4);
            float* r1 = r0 + 16;
            float* v1 = v0 + 4;
            float* r2 = r1 + 16;
            float* v2 = v1 + 4;
            float* r3 = r2 + 16;
            float* v3 = v2 + 4;
            r0[0] += v0[0] * dt;
            r0[1] += v0[1] * dt;
            r0[2] += v0[2] * dt;
            r1[0] += v1[0] * dt;
            r1[1] += v1[1] * dt;
            r1[2] += v1[2] * dt;
            r2[0] += v2[0] * dt;
            r2[1] += v2[1] * dt;
            r2[2] += v2[2] * dt;
            r3[0] += v3[0] * dt;
            r3[1] += v3[1] * dt;
            r3[2] += v3[2] * dt;
        }

        // Scalar tail: i in [n4, count), row i at local + i*16 / vel + i*4.
        // (LocalTransform is 16 floats = 64B; Velocity is 4 floats = 16B.)
        for (int i = n4; i < count; i++)
        {
            float* row = local + (i * 16);
            float* vv = vel + (i * 4);
            row[0] += vv[0] * dt;
            row[1] += vv[1] * dt;
            row[2] += vv[2] * dt;
        }
    }
}
