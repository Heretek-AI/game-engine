// AnimatorApiTests.cs — P5-03: managed animator contract gate.
//
// Verifies the C# Animator surface:
//   * Type shape (no native needed): Animator API shape (SetFloat,
//     SetBool, SetTrigger, Update, ActiveState, IDisposable) plus the
//     ABI 4.18 tail entries (animator_create/destroy/set_float/
//     set_bool/set_trigger/update/state_name) and the opaque 8B handle.
//   * Managed-stub functional + zero-alloc probe (no native needed):
//     drive the Animator wrapper through a managed ApiTable built from
//     UnmanagedCallersOnly stubs, so the zero-alloc claim covers the
//     real wrapper path. The stub models the Idle<->Run contract shape
//     (speed float gate + ActiveState readback).
//   * Live native round-trip (skipped when engine_core is unloadable):
//     create from the P5-03 Idle/Run graph, drive speed 0->1->0, assert
//     the exact active-state sequence Idle ... Run ... Idle.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class AnimatorApiTests
{
    public const string IdleRunGraph = """
        {"name":"Locomotion","initial":"Idle",
         "parameters":[{"name":"speed","type":"float","default":0.0},
                        {"name":"grounded","type":"bool","default":true},
                        {"name":"jump","type":"trigger"}],
         "states":[{"name":"Idle","clip":"idle","loop":true},
                   {"name":"Run","clip":"run","loop":true},
                   {"name":"Jump","clip":"jump","loop":false}],
         "transitions":[{"from":"Idle","to":"Run","duration":0.25,
                         "conditions":[{"param":"speed","op":">","value":0.5}]},
                        {"from":"Run","to":"Idle","duration":0.2,
                         "conditions":[{"param":"speed","op":"<=","value":0.5}]},
                        {"from":"Run","to":"Jump","duration":0.1,
                         "conditions":[{"param":"jump","op":"fired"},
                                        {"param":"grounded","op":"==","value":true}]}]}
        """;

    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(typeof(Animator).GetMethod("SetFloat", [typeof(string), typeof(float)]) != null, "Animator.SetFloat(string,float) exists");
        failures += Check(typeof(Animator).GetMethod("SetBool", [typeof(string), typeof(bool)]) != null, "Animator.SetBool(string,bool) exists");
        failures += Check(typeof(Animator).GetMethod("SetTrigger", [typeof(string)]) != null, "Animator.SetTrigger(string) exists");
        failures += Check(typeof(Animator).GetMethod("Update", [typeof(float)]) != null, "Animator.Update(float) exists");
        failures += Check(typeof(Animator).GetProperty("ActiveState") != null, "Animator.ActiveState exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Animator)), "Animator IDisposable");

        unsafe
        {
            failures += Check(sizeof(NativeApi.EngineAnimator) == 8, "NativeApi.EngineAnimator opaque 8B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineAnimatorCreate", "EngineAnimatorDestroy", "EngineAnimatorSetFloat", "EngineAnimatorSetBool", "EngineAnimatorSetTrigger", "EngineAnimatorUpdate", "EngineAnimatorStateName" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Managed-stub functional + zero-alloc probe (no native) -----
        var priorOverride = Animator.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineAnimatorCreate = &FakeAnimator.StubCreate;
                stub.EngineAnimatorDestroy = &FakeAnimator.StubDestroy;
                stub.EngineAnimatorSetFloat = &FakeAnimator.StubSetFloat;
                stub.EngineAnimatorSetBool = &FakeAnimator.StubSetBool;
                stub.EngineAnimatorSetTrigger = &FakeAnimator.StubSetTrigger;
                stub.EngineAnimatorUpdate = &FakeAnimator.StubUpdate;
                stub.EngineAnimatorStateName = &FakeAnimator.StubStateName;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Animator.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    FakeAnimator.Reset();
                    using var animator = new Animator((IntPtr)0x3000, 42, IdleRunGraph);
                    failures += Check(FakeAnimator.CreateCount == 1, "stub animator_create reached (count=1)");
                    failures += Check(FakeAnimator.LastEntity == 42, "stub animator_create receives entity id");
                    failures += Check(animator.ActiveState == "Idle", "stub initial state is Idle");

                    animator.SetFloat("speed", 1.0f);
                    failures += Check(FakeAnimator.LastFloatName == "speed" && FakeAnimator.LastFloat == 1.0f, "stub set_float reaches native entry (speed=1)");
                    animator.Update(1.0f / 60.0f);
                    failures += Check(animator.ActiveState == "Run", "stub speed=1 transitions Idle->Run");

                    animator.SetFloat("speed", 0.0f);
                    animator.Update(1.0f / 60.0f);
                    failures += Check(animator.ActiveState == "Idle", "stub speed=0 transitions Run->Idle");

                    animator.SetBool("grounded", true);
                    failures += Check(FakeAnimator.LastBoolName == "grounded" && FakeAnimator.LastBool == 1, "stub set_bool reaches native entry (grounded)");

                    animator.SetTrigger("jump");
                    failures += Check(FakeAnimator.LastTrigger == "jump", "stub set_trigger reaches native entry (jump)");

                    // Steady-state probe: Update alone must allocate nothing
                    // (UTF8 string setters + ActiveState string decode are
                    // excluded — same carve-out as the Ui string probes;
                    // the claim covers the numeric frame path the game
                    // loop drives every frame).
                    FakeAnimator.SteadyDrive(animator);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeAnimator.SteadyDrive(animator);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"Animator Update steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    Animator.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Animator.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // Acceptance through the REAL wrapper + REAL native runtime: create
        // from the Idle/Run graph, drive speed 0->1->0 with Update steps,
        // and assert the EXACT active-state sequence (never a settled-only
        // probe). Step counts and cross-fade semantics are the native
        // 65-check gate's (test_animator_state_machine.cpp, dt=0.05):
        // ActiveState() names the `from` state for the whole fade and flips
        // only at completion — Idle->Run (0.25s) reads
        // Idle,Idle,Idle,Idle,Idle,Run over 6 updates; Run->Idle (0.2s)
        // reads Run,Run,Run,Run,Idle over 5 updates; the jump trigger fires
        // only when grounded (bool AND), is latched while airborne, and is
        // consumed on fire (Run->Jump reads Run,Jump over 2 updates).
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = AnimatorLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            var savedOverride = Animator.TableOverride;
            try
            {
                if (preloaded != IntPtr.Zero)
                {
                    AnimatorLivePreload.LoadIntoApiLoader(preloaded);
                    table = (NativeApi.ApiTable*)Animator.TableOverride;
                }
                else if (liveProbe)
                {
                    table = NativeApi.ApiLoader.Load();
                    if (table == null)
                    {
                        Console.WriteLine("AnimatorApiTests: live probe requested but engine_core unloadable — FAIL");
                        return failures + 1;
                    }
                }
                else
                {
                    Console.WriteLine("AnimatorApiTests: shape + stub checks passed (live native skipped, pass --live-native-probe with staged engine_core)");
                    Console.WriteLine(failures == 0 ? "AnimatorApiTests: all checks passed" : $"AnimatorApiTests: {failures} FAILED");
                    return failures;
                }

                failures += Check(table->EngineAnimatorCreate != null, "live: animator_create non-null");
                failures += Check(table->EngineAnimatorStateName != null, "live: animator_state_name non-null");
                if (table->EngineAnimatorCreate == null || table->EngineAnimatorStateName == null)
                {
                    Console.WriteLine(failures == 0 ? "AnimatorApiTests: all checks passed" : $"AnimatorApiTests: {failures} FAILED");
                    return failures;
                }

                var world = table->EngineWorldCreate();
                failures += Check(world != null, "live: world_create");
                if (world == null)
                {
                    Console.WriteLine(failures == 0 ? "AnimatorApiTests: all checks passed" : $"AnimatorApiTests: {failures} FAILED");
                    return failures;
                }
                try
                {
                    using var animator = new Animator((IntPtr)world, 7, IdleRunGraph);
                    failures += Check(animator.ActiveState == "Idle", "live: initial state is Idle");

                    // Exact Idle->Run sequence (dt=0.05, duration 0.25): fire +
                    // 4 blend steps still read Idle, 6th update completes on Run.
                    animator.SetFloat("speed", 1.0f);
                    var idleToRun = new List<string>();
                    for (var i = 0; i < 6; i++)
                    {
                        animator.Update(0.05f);
                        idleToRun.Add(animator.ActiveState);
                    }
                    failures += Check(string.Join(",", idleToRun) == "Idle,Idle,Idle,Idle,Idle,Run",
                        $"live: exact Idle->Run sequence [{string.Join(",", idleToRun)}]");

                    // Exact Run->Idle sequence (dt=0.05, duration 0.2): fire +
                    // 3 blend steps still read Run, 5th update completes on Idle.
                    animator.SetFloat("speed", 0.0f);
                    var runToIdle = new List<string>();
                    for (var i = 0; i < 5; i++)
                    {
                        animator.Update(0.05f);
                        runToIdle.Add(animator.ActiveState);
                    }
                    failures += Check(string.Join(",", runToIdle) == "Run,Run,Run,Run,Idle",
                        $"live: exact Run->Idle sequence [{string.Join(",", runToIdle)}]");

                    // Bool AND gate: grounded=false latches the trigger (Jump
                    // must NOT fire while airborne; trigger survives).
                    animator.SetFloat("speed", 1.0f);
                    for (var i = 0; i < 6; i++)
                    {
                        animator.Update(0.05f);
                    }
                    failures += Check(animator.ActiveState == "Run", "live: back on Run before jump");
                    animator.SetBool("grounded", false);
                    animator.SetTrigger("jump");
                    animator.Update(0.05f);
                    failures += Check(animator.ActiveState == "Run", "live: jump latched while airborne (stays Run)");

                    // Grounded=true: the latched trigger fires. Exact Run->Jump
                    // sequence (dt=0.05, duration 0.1): fire read still Run,
                    // one mid-fade step still Run, 3rd update completes Jump.
                    animator.SetBool("grounded", true);
                    animator.Update(0.05f);
                    var fireRead = animator.ActiveState;
                    animator.Update(0.05f);
                    var midFade = animator.ActiveState;
                    animator.Update(0.05f);
                    var jumpDone = animator.ActiveState;
                    failures += Check(fireRead == "Run" && midFade == "Run" && jumpDone == "Jump",
                        $"live: exact Run->Jump sequence [{fireRead},{midFade},{jumpDone}]");

                    // Trigger consumed on fire: settling stays on Jump.
                    animator.Update(0.5f);
                    failures += Check(animator.ActiveState == "Jump", "live: trigger consumed, stays Jump");
                }
                finally
                {
                    table->EngineWorldDestroy(world);
                }
            }
            finally
            {
                Animator.TableOverride = savedOverride;
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
                Console.WriteLine($"AnimatorApiTests: live native skipped ({ex.GetType().Name})");
            }
        }

        Console.WriteLine(failures == 0 ? "AnimatorApiTests: all checks passed" : $"AnimatorApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static unsafe class FakeAnimator
    {
        public static int CreateCount;
        public static ulong LastEntity;
        public static string LastFloatName = "";
        public static float LastFloat;
        public static string LastBoolName = "";
        public static int LastBool;
        public static string LastTrigger = "";
        public static float Speed;
        public static string State = "Idle";
        public static void Reset()
        {
            CreateCount = 0;
            LastEntity = 0;
            LastFloatName = "";
            LastFloat = 0;
            LastBoolName = "";
            LastBool = 0;
            LastTrigger = "";
            Speed = 0;
            State = "Idle";
        }

        public static void SteadyDrive(Animator animator)
        {
            // Zero-alloc subset: Update only (numeric frame path). The
            // string-taking setters encode UTF8 per call and ActiveState
            // decodes a string (same carve-out as the Ui string probes).
            animator.Update(1.0f / 60.0f);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static NativeApi.EngineAnimator* StubCreate(NativeApi.EngineWorld* world, ulong entity, byte* json) { CreateCount++; LastEntity = entity; return (NativeApi.EngineAnimator*)0x18; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void StubDestroy(NativeApi.EngineAnimator* animator) { }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubSetFloat(NativeApi.EngineAnimator* animator, byte* name, float value)
        {
            var s = Marshal.PtrToStringUTF8((IntPtr)name) ?? "";
            LastFloatName = s;
            LastFloat = value;
            if (s == "speed")
            {
                Speed = value;
            }

            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubSetBool(NativeApi.EngineAnimator* animator, byte* name, int value)
        {
            LastBoolName = Marshal.PtrToStringUTF8((IntPtr)name) ?? "";
            LastBool = value;
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubSetTrigger(NativeApi.EngineAnimator* animator, byte* name)
        {
            LastTrigger = Marshal.PtrToStringUTF8((IntPtr)name) ?? "";
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubUpdate(NativeApi.EngineAnimator* animator, float dt)
        {
            State = Speed > 0.5f ? "Run" : "Idle";
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubStateName(NativeApi.EngineAnimator* animator, byte* @out, int cap)
        {
            var bytes = System.Text.Encoding.UTF8.GetBytes(State);
            var n = Math.Min(bytes.Length, cap - 1);
            for (var i = 0; i < n; i++)
            {
                @out[i] = bytes[i];
            }

            @out[n] = 0;
            return n;
        }
    }

    private static class AnimatorLivePreload
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

            Animator.TableOverride = (IntPtr)table;
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
