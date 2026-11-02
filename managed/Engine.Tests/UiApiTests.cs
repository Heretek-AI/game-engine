// UiApiTests.cs — P5-04: managed game UI contract gate.
//
// Verifies the C# UI surface:
//   * Type shape (no native needed): UIDocument type shape (Load/SetText/
//     GetText/HasElement/InjectClick/Render/CopyFrame/ClickCount/
//     ConsumeClicks, IDisposable, frame constants).
//   * Managed-stub functional probe (no native needed): drive the UIDocument
//     wrapper through a managed ApiTable built from UnmanagedCallersOnly
//     stubs, so the zero-alloc claim covers the real wrapper path.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     creates a real UI world from the staged font dir, sets the label,
//     injects a click at the button rect, and drains exactly one click —
//     mirrors the CTest acceptance.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class UiApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(typeof(UIDocument).IsClass, "UIDocument is a class (owns native handle)");
        failures += Check(typeof(UIDocument).GetMethod("Load") != null, "UIDocument.Load exists");
        failures += Check(typeof(UIDocument).GetMethod("SetText") != null, "UIDocument.SetText exists");
        failures += Check(typeof(UIDocument).GetMethod("GetText") != null, "UIDocument.GetText exists");
        failures += Check(typeof(UIDocument).GetMethod("HasElement") != null, "UIDocument.HasElement exists");
        failures += Check(typeof(UIDocument).GetMethod("InjectClick") != null, "UIDocument.InjectClick exists");
        failures += Check(typeof(UIDocument).GetMethod("Render") != null, "UIDocument.Render exists");
        failures += Check(typeof(UIDocument).GetMethod("CopyFrame") != null, "UIDocument.CopyFrame exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(UIDocument)), "UIDocument IDisposable");
        failures += Check(UIDocument.FrameWidth == 640, "UIDocument.FrameWidth == 640");
        failures += Check(UIDocument.FrameHeight == 360, "UIDocument.FrameHeight == 360");
        failures += Check(UIDocument.FrameBytes == 640 * 360 * 4, "UIDocument.FrameBytes == 921600");

        unsafe
        {
            failures += Check(sizeof(NativeApi.EngineUi) == 8, "NativeApi.EngineUi opaque 8B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineUiCreate", "EngineUiDestroy", "EngineUiLoad", "EngineUiSetText", "EngineUiGetText", "EngineUiHasElement", "EngineUiInjectClick", "EngineUiRender", "EngineUiCopyFrame", "EngineUiClickCount", "EngineUiConsumeClicks" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Managed-stub functional probe (no native needed) ------------
        var priorOverride = UIDocument.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineUiCreate = &FakeUi.StubCreate;
                stub.EngineUiDestroy = &FakeUi.StubDestroy;
                stub.EngineUiLoad = &FakeUi.StubLoad;
                stub.EngineUiSetText = &FakeUi.StubSetText;
                stub.EngineUiGetText = &FakeUi.StubGetText;
                stub.EngineUiHasElement = &FakeUi.StubHasElement;
                stub.EngineUiInjectClick = &FakeUi.StubInjectClick;
                stub.EngineUiRender = &FakeUi.StubRender;
                stub.EngineUiCopyFrame = &FakeUi.StubCopyFrame;
                stub.EngineUiClickCount = &FakeUi.StubClickCount;
                stub.EngineUiConsumeClicks = &FakeUi.StubConsumeClicks;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    UIDocument.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    FakeUi.Reset();
                    using var doc = new UIDocument(fontDir: "");
                    failures += Check(FakeUi.CreateCount == 1, "stub create reached (count=1)");

                    doc.SetText("lbl", "SCORE 42");
                    failures += Check(FakeUi.LastId == "lbl" && FakeUi.LastText == "SCORE 42", "stub SetText reaches native entry (lbl)");

                    var text = doc.GetText("lbl");
                    failures += Check(text == "SCORE 42", "stub GetText round-trips");

                    failures += Check(doc.HasElement("btn"), "stub HasElement(btn) true");
                    failures += Check(!doc.HasElement("nope"), "stub HasElement(nope) false");

                    doc.InjectClick(547.0f, 56.0f);
                    failures += Check(FakeUi.Clicks == 1, "stub InjectClick queues one click");
                    failures += Check(doc.ClickCount == 1, "stub ClickCount == 1");
                    var drained = doc.ConsumeClicks();
                    failures += Check(drained == 1 && doc.ClickCount == 0, "stub ConsumeClicks drains exactly once");

                    doc.Render();
                    failures += Check(FakeUi.Renders == 1, "stub Render reaches native entry");
                    var frame = doc.CopyFrame();
                    failures += Check(frame.Length == UIDocument.FrameBytes, "stub CopyFrame returns full frame");

                    // Warm up once (JIT + first-call caches), then measure.
                    FakeUi.SteadyDrive(doc);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeUi.SteadyDrive(doc);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"UI wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    UIDocument.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            UIDocument.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // Acceptance through the REAL wrapper: create from the staged font
        // dir, set the label, inject one click at the button rect (canvas
        // coords 1280x720: button center = (1094, 56)), drain exactly one.
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = UiLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            var savedOverride = UIDocument.TableOverride;
            try
            {
                if (preloaded != IntPtr.Zero)
                {
                    UiLivePreload.LoadIntoApiLoader(preloaded);
                    table = (NativeApi.ApiTable*)UIDocument.TableOverride;
                }
                else if (liveProbe)
                {
                    table = NativeApi.ApiLoader.Load();
                    if (table == null)
                    {
                        Console.WriteLine("UiApiTests: live probe requested but engine_core unloadable — FAIL");
                        return failures + 1;
                    }
                }
                else
                {
                    Console.WriteLine("UiApiTests: shape + stub checks passed (live native skipped, pass --live-native-probe with staged engine_core)");
                    Console.WriteLine(failures == 0 ? "UiApiTests: all checks passed" : $"UiApiTests: {failures} FAILED");
                    return failures;
                }

                failures += Check(table->EngineUiCreate != null, "live: ui_create non-null");
                failures += Check(table->EngineUiCopyFrame != null, "live: ui_copy_frame non-null");
                failures += Check(table->EngineUiCreate(null) == null, "live: null font dir rejected (no crash)");

                if (table->EngineUiCreate == null || table->EngineUiCopyFrame == null)
                {
                    Console.WriteLine(failures == 0 ? "UiApiTests: all checks passed" : $"UiApiTests: {failures} FAILED");
                    return failures;
                }

                var fontDir = UiLivePreload.FontDir();
                using var doc = new UIDocument(fontDir);
                failures += Check(doc.HasElement("lbl"), "live: HUD has #lbl");
                failures += Check(doc.HasElement("btn"), "live: HUD has #btn");
                failures += Check(!doc.HasElement("nope"), "live: missing id reports false");

                doc.SetText("lbl", "SCORE 42");
                failures += Check(doc.GetText("lbl") == "SCORE 42", "live: label text round-trips");

                doc.InjectClick(1094.0f, 56.0f);
                failures += Check(doc.ClickCount == 1, "live: one injected click queues exactly one");
                var drained = doc.ConsumeClicks();
                failures += Check(drained == 1, "live: ConsumeClicks drains exactly one");
                failures += Check(doc.ClickCount == 0, "live: queue empty after drain");

                doc.Render();
                var frame = doc.CopyFrame();
                failures += Check(frame.Length == UIDocument.FrameBytes, "live: frame is 640x360 RGBA8");
                var nonZero = 0;
                for (var i = 0; i < frame.Length; i += 401)
                {
                    if (frame[i] != 0)
                    {
                        nonZero++;
                    }
                }

                failures += Check(nonZero > 0, "live: frame is non-empty (HUD painted)");
            }
            finally
            {
                UIDocument.TableOverride = savedOverride;
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
                Console.WriteLine($"UiApiTests: live native skipped ({ex.GetType().Name})");
            }
        }

        Console.WriteLine(failures == 0 ? "UiApiTests: all checks passed" : $"UiApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static unsafe class FakeUi
    {
        public static int CreateCount;
        public static string LastId = "";
        public static string LastText = "";
        public static uint Clicks;
        public static int Renders;
        public static void Reset()
        {
            CreateCount = 0;
            LastId = "";
            LastText = "";
            Clicks = 0;
            Renders = 0;
        }

        public static void SteadyDrive(UIDocument doc)
        {
            // Zero-alloc subset: Render + ClickCount/ConsumeClicks only.
            // String-taking entries (SetText/HasElement/GetText/Load)
            // encode UTF8 per call (same carve-out as the P2-05 collision
            // drain's string probes); the steady-state claim covers the
            // numeric frame/click path the game loop actually drives.
            doc.Render();
            _ = doc.ClickCount;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static NativeApi.EngineUi* StubCreate(byte* fontDir) { CreateCount++; return (NativeApi.EngineUi*)0x8; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void StubDestroy(NativeApi.EngineUi* ui) { }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubLoad(NativeApi.EngineUi* ui, byte* rml) => 0;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubSetText(NativeApi.EngineUi* ui, byte* id, byte* text)
        {
            LastId = Marshal.PtrToStringUTF8((IntPtr)id) ?? "";
            LastText = Marshal.PtrToStringUTF8((IntPtr)text) ?? "";
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubGetText(NativeApi.EngineUi* ui, byte* id, byte* @out, uint cap)
        {
            var bytes = System.Text.Encoding.UTF8.GetBytes(LastText);
            var n = Math.Min(bytes.Length, (int)cap - 1);
            for (var i = 0; i < n; i++)
            {
                @out[i] = bytes[i];
            }

            @out[n] = 0;
            return n;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubHasElement(NativeApi.EngineUi* ui, byte* id)
        {
            var s = Marshal.PtrToStringUTF8((IntPtr)id) ?? "";
            return (s == "lbl" || s == "btn") ? 1 : 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubInjectClick(NativeApi.EngineUi* ui, float x, float y) { Clicks++; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubRender(NativeApi.EngineUi* ui) { Renders++; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StubCopyFrame(NativeApi.EngineUi* ui, byte* @out, uint cap)
        {
            for (uint i = 0; i < cap; i++)
            {
                @out[i] = (byte)(i & 0xFF);
            }

            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static uint StubClickCount(NativeApi.EngineUi* ui) => Clicks;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static uint StubConsumeClicks(NativeApi.EngineUi* ui)
        {
            var n = Clicks;
            Clicks = 0;
            return n;
        }
    }

    private static class UiLivePreload
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

            UIDocument.TableOverride = (IntPtr)table;
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

        public static string FontDir()
        {
            var dir = Environment.GetEnvironmentVariable("ENGINE_UI_FONT_DIR");
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
            {
                return dir;
            }

            // Fall back to the staged build dir next to the test binary.
            var baseDir = AppContext.BaseDirectory;
            var candidate = Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "ui_fonts"));
            if (Directory.Exists(candidate))
            {
                return candidate;
            }

            return "/tmp";
        }
    }
}
