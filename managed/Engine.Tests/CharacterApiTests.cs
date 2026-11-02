// CharacterApiTests.cs — P6-04: CharacterController managed contract gate.
//
// Verifies the C# character surface:
//   * Type shape (no native needed): CharacterConfig/CharacterState
//     blittable + 24B, CharacterController API shape (Move(Vector3),
//     Move(dx,dz,dt), Teleport, SetSlopeLimit, SetStepHeight,
//     GetPosition, IsGrounded, IDisposable).
//   * Managed-stub functional + zero-alloc probe (no native needed):
//     drive the CharacterController wrapper through a managed ApiTable
//     built from UnmanagedCallersOnly stubs, so the zero-alloc claim
//     covers the real wrapper path.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     preload engine_core by absolute path, create a world via
//     PhysicsApiTests' world helpers is out of scope here — drive the
//     ABI character_* entries directly (null guards + move/teleport).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class CharacterApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.CharacterConfig>() == false, "CharacterConfig blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.CharacterState>() == false, "CharacterState blittable");
        failures += Check(typeof(CharacterController).GetMethod("Move", [typeof(float), typeof(float), typeof(float)]) != null, "CharacterController.Move(dx,dz,dt) exists");
        failures += Check(typeof(CharacterController).GetMethod("Move", [typeof(System.Numerics.Vector3), typeof(float)]) != null, "CharacterController.Move(Vector3) exists");
        failures += Check(typeof(CharacterController).GetMethod("Teleport") != null, "CharacterController.Teleport exists");
        failures += Check(typeof(CharacterController).GetMethod("SetSlopeLimit") != null, "CharacterController.SetSlopeLimit exists");
        failures += Check(typeof(CharacterController).GetMethod("SetStepHeight") != null, "CharacterController.SetStepHeight exists");
        failures += Check(typeof(CharacterController).GetMethod("GetPosition") != null, "CharacterController.GetPosition exists");
        failures += Check(typeof(CharacterController).GetProperty("IsGrounded") != null, "CharacterController.IsGrounded exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(CharacterController)), "CharacterController IDisposable");

        unsafe
        {
            failures += Check(sizeof(NativeApi.CharacterConfig) == 24, "NativeApi.CharacterConfig 24B");
            failures += Check(sizeof(NativeApi.CharacterState) == 24, "NativeApi.CharacterState 24B");
            failures += Check(sizeof(NativeApi.EngineCharacter) == 8, "NativeApi.EngineCharacter opaque 8B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineCharacterCreate", "EngineCharacterDestroy", "EngineCharacterMove", "EngineCharacterTeleport", "EngineCharacterSetSlope", "EngineCharacterSetStep", "EngineCharacterGetPos" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Managed-stub functional + zero-alloc probe (no native) -----
        var priorOverride = CharacterController.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineCharacterCreate = &FakeCharacter.StubCreate;
                stub.EngineCharacterDestroy = &FakeCharacter.StubDestroy;
                stub.EngineCharacterMove = &FakeCharacter.StubMove;
                stub.EngineCharacterTeleport = &FakeCharacter.StubTeleport;
                stub.EngineCharacterSetSlope = &FakeCharacter.StubSetSlope;
                stub.EngineCharacterSetStep = &FakeCharacter.StubSetStep;
                stub.EngineCharacterGetPos = &FakeCharacter.StubGetPos;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    CharacterController.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    var config = new NativeApi.CharacterConfig
                    {
                        HalfHeight = 0.9f,
                        Radius = 0.3f,
                        MaxSlopeDeg = 45.0f,
                        MaxStepHeight = 0.4f,
                        SkinWidth = 0.02f,
                        StickToFloor = 1,
                    };
                    using var controller = new CharacterController((IntPtr)0x2000, config, 0, 1, 0);
                    failures += Check(FakeCharacter.CreateCount == 1, "stub character_create reached (count=1)");
                    failures += Check(FakeCharacter.LastHalfHeight == 0.9f, "stub character_create receives half_height");

                    var state = controller.Move(2.0f, 0.0f, 1.0f / 60.0f);
                    failures += Check(FakeCharacter.LastDx == 2.0f && FakeCharacter.LastDt > 0.0f, "stub character_move receives dx+dt");
                    failures += Check(state.Px == 1.0f && state.Grounded == 1, "stub character_move returns feet+grounded");

                    var vstate = controller.Move(new System.Numerics.Vector3(1.0f, 9.0f, 2.0f), 1.0f / 60.0f);
                    failures += Check(FakeCharacter.LastDx == 1.0f && FakeCharacter.LastDz == 2.0f, "stub Move(Vector3) maps X/Z (ignores Y)");

                    controller.Teleport(3.0f, 1.0f, 4.0f);
                    failures += Check(FakeCharacter.LastTx == 3.0f, "stub character_teleport receives x");

                    controller.SetSlopeLimit(30.0f);
                    failures += Check(FakeCharacter.LastSlope == 30.0f, "stub character_set_slope receives degrees");

                    controller.SetStepHeight(0.2f);
                    failures += Check(FakeCharacter.LastStep == 0.2f, "stub character_set_step receives meters");

                    var pos = controller.GetPosition();
                    failures += Check(pos.Px == 1.0f, "stub character_get_pos returns feet");

                    failures += Check(controller.IsGrounded, "stub IsGrounded true when stub grounded");

                    // Warm up once, then measure.
                    FakeCharacter.SteadyDrive(controller);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeCharacter.SteadyDrive(controller);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"CharacterController wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    CharacterController.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            CharacterController.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // Null-guard probes against the real table: every character_*
        // entry must fail closed on null handles (no crash). Full step/
        // slope acceptance lives in CTest (engine_character_p6_04), which
        // owns a real PhysicsWorld; managed owns no world here.
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = CharacterLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            if (preloaded != IntPtr.Zero)
            {
                CharacterLivePreload.LoadIntoApiLoader(preloaded);
                table = (NativeApi.ApiTable*)CharacterController.TableOverride;
            }
            else
            {
                table = NativeApi.ApiLoader.Load();
            }
            failures += Check(table->EngineCharacterCreate != null, "live: character_create non-null");
            failures += Check(table->EngineCharacterMove != null, "live: character_move non-null");
            failures += Check(table->EngineCharacterTeleport != null, "live: character_teleport non-null");
            failures += Check(table->EngineCharacterSetSlope != null, "live: character_set_slope non-null");
            failures += Check(table->EngineCharacterSetStep != null, "live: character_set_step non-null");
            failures += Check(table->EngineCharacterGetPos != null, "live: character_get_pos non-null");

            var cfg = default(NativeApi.CharacterConfig);
            cfg.HalfHeight = 0.9f;
            cfg.Radius = 0.3f;
            var nullChar = table->EngineCharacterCreate(null, &cfg, 0, 1, 0);
            failures += Check(nullChar == null, "live: null world character_create -> null (no crash)");
            var st = default(NativeApi.CharacterState);
            failures += Check(table->EngineCharacterMove(null, 0, 0, 1.0f / 60.0f, &st) == 0, "live: null character_move -> 0 (no crash)");
            failures += Check(table->EngineCharacterTeleport(null, 0, 0, 0) == 0, "live: null character_teleport -> 0 (no crash)");
            failures += Check(table->EngineCharacterSetSlope(null, 30) == 0, "live: null character_set_slope -> 0 (no crash)");
            failures += Check(table->EngineCharacterSetStep(null, 0.2f) == 0, "live: null character_set_step -> 0 (no crash)");
            failures += Check(table->EngineCharacterGetPos(null, &st) == 0, "live: null character_get_pos -> 0 (no crash)");
        }
        catch (Exception ex)
        {
            if ((ex is DllNotFoundException || ex is EntryPointNotFoundException) && !liveProbe)
            {
                Console.WriteLine($"[SKIP] character live (no native lib: {ex.GetType().Name})");
                Console.WriteLine(failures == 0 ? "CharacterApiTests: all checks passed" : $"CharacterApiTests: {failures} FAILED");
                return failures;
            }

            failures += Check(false, $"live probe FAILED: {ex.GetType().Name}: {ex.Message}");
            Console.WriteLine($"CharacterApiTests: {failures} FAILED");
            return failures;
        }

        Console.WriteLine(failures == 0 ? "CharacterApiTests: all checks passed" : $"CharacterApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    // Managed stub table (no native needed): UnmanagedCallersOnly entries
    // that record call args and return canned states.
    private static class FakeCharacter
    {
        public static int CreateCount;
        public static float LastHalfHeight;
        public static float LastDx;
        public static float LastDz;
        public static float LastDt;
        public static float LastTx;
        public static float LastSlope;
        public static float LastStep;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void SteadyDrive(CharacterController controller)
        {
            controller.Move(2.0f, 0.0f, 1.0f / 60.0f);
            controller.GetPosition();
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe NativeApi.EngineCharacter* StubCreate(NativeApi.EnginePhysicsWorld* world, NativeApi.CharacterConfig* config, float x, float y, float z)
        {
            CreateCount++;
            if (config != null)
            {
                LastHalfHeight = config->HalfHeight;
            }
            return (NativeApi.EngineCharacter*)0x3000;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe void StubDestroy(NativeApi.EngineCharacter* character)
        {
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubMove(NativeApi.EngineCharacter* character, float dx, float dz, float dt, NativeApi.CharacterState* state)
        {
            LastDx = dx;
            LastDz = dz;
            LastDt = dt;
            if (state != null)
            {
                state->Px = 1.0f;
                state->Py = 0.3f;
                state->Pz = 0.0f;
                state->Grounded = 1;
                state->GroundY = 0.3f;
                state->Vy = 0.0f;
            }
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubTeleport(NativeApi.EngineCharacter* character, float x, float y, float z)
        {
            LastTx = x;
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetSlope(NativeApi.EngineCharacter* character, float maxSlopeDeg)
        {
            LastSlope = maxSlopeDeg;
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSetStep(NativeApi.EngineCharacter* character, float maxStepHeight)
        {
            LastStep = maxStepHeight;
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubGetPos(NativeApi.EngineCharacter* character, NativeApi.CharacterState* state)
        {
            if (state != null)
            {
                state->Px = 1.0f;
                state->Py = 0.3f;
                state->Pz = 0.0f;
                state->Grounded = 1;
                state->GroundY = 0.3f;
                state->Vy = 0.0f;
            }
            return 1;
        }
    }

    // Staged-lib preload (same absolute-path precedent as the navigation
    // probe): pins libflecs + engine_core, then points
    // CharacterController.TableOverride at the live table.
    private static class CharacterLivePreload
    {
        public static unsafe IntPtr TryPreloadEngineCoreHandle()
        {
            var seeds = new List<string> { AppContext.BaseDirectory };
            try
            {
                var entry = System.Reflection.Assembly.GetEntryAssembly()?.Location;
                if (!string.IsNullOrEmpty(entry))
                {
                    seeds.Add(Path.GetDirectoryName(Path.GetFullPath(entry)) ?? string.Empty);
                }
            }
            catch (Exception)
            {
            }

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

            CharacterController.TableOverride = (IntPtr)table;
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
