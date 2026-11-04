// TimelineApiTests.cs — P5-07: managed sequencing/timing contract gate.
//
// Verifies the C# sequencing surface (ABI 4.19 over the 4.18 data model):
//   * Type shape (no native needed): Timeline type shape (AddTrack/
//     RemoveTrack/AddClip/RemoveClip/GetClip/Duration/Sort/Trim/
//     EvaluateActive, IDisposable), TimelineTrackKind values, mirror sizes.
//   * Managed-stub functional probe (no native needed): drive the Timeline
//     wrapper through a managed ApiTable built from UnmanagedCallersOnly
//     stubs, so the sequencing behavior (ordering, overlap, trim, eval)
//     is verified through the real wrapper path.
//   * Live native round-trip (skipped without engine_core): out-of-order
//     adds + sort + overlap-winner eval + half-open end + trim clamp/drop
//     through the REAL wrapper — mirrors the CTest acceptance.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class TimelineApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(typeof(Timeline).IsClass, "Timeline is a class (owns native handle)");
        failures += Check(typeof(Timeline).GetMethod("AddTrack") != null, "Timeline.AddTrack exists");
        failures += Check(typeof(Timeline).GetMethod("RemoveTrack") != null, "Timeline.RemoveTrack exists");
        failures += Check(typeof(Timeline).GetMethod("AddClip") != null, "Timeline.AddClip exists");
        failures += Check(typeof(Timeline).GetMethod("RemoveClip") != null, "Timeline.RemoveClip exists");
        failures += Check(typeof(Timeline).GetMethod("GetClip") != null, "Timeline.GetClip exists");
        failures += Check(typeof(Timeline).GetMethod("Sort") != null, "Timeline.Sort exists");
        failures += Check(typeof(Timeline).GetMethod("Trim") != null, "Timeline.Trim exists");
        failures += Check(typeof(Timeline).GetMethod("EvaluateActive") != null, "Timeline.EvaluateActive exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Timeline)), "Timeline IDisposable");
        failures += Check((uint)TimelineTrackKind.Transform == 0, "Transform == 0");
        failures += Check((uint)TimelineTrackKind.AnimClip == 1, "AnimClip == 1");
        failures += Check((uint)TimelineTrackKind.Audio == 2, "Audio == 2");
        failures += Check((uint)TimelineTrackKind.Activation == 3, "Activation == 3");

        unsafe
        {
            failures += Check(sizeof(NativeApi.EngineTimeline) == 8, "NativeApi.EngineTimeline opaque 8B");
            failures += Check(sizeof(NativeApi.TimelineClip) == 32, "NativeApi.TimelineClip 32B");
            failures += Check(sizeof(NativeApi.TimelineActiveClip) == 16, "NativeApi.TimelineActiveClip 16B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineTimelineCreate", "EngineTimelineDestroy", "EngineTimelineTrackAdd", "EngineTimelineTrackRemove", "EngineTimelineClipAdd", "EngineTimelineClipRemove", "EngineTimelineDuration", "EngineTimelineClipGet", "EngineTimelineSort", "EngineTimelineTrim", "EngineTimelineEvaluateActive" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Managed-stub functional probe (no native needed) ------------
        var priorOverride = Timeline.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineTimelineCreate = &FakeSequencer.StubCreate;
                stub.EngineTimelineDestroy = &FakeSequencer.StubDestroy;
                stub.EngineTimelineTrackAdd = &FakeSequencer.StubTrackAdd;
                stub.EngineTimelineTrackRemove = &FakeSequencer.StubTrackRemove;
                stub.EngineTimelineClipAdd = &FakeSequencer.StubClipAdd;
                stub.EngineTimelineClipRemove = &FakeSequencer.StubClipRemove;
                stub.EngineTimelineDuration = &FakeSequencer.StubDuration;
                stub.EngineTimelineClipGet = &FakeSequencer.StubClipGet;
                stub.EngineTimelineSort = &FakeSequencer.StubSort;
                stub.EngineTimelineTrim = &FakeSequencer.StubTrim;
                stub.EngineTimelineEvaluateActive = &FakeSequencer.StubEvaluateActive;
                stub.EngineTimelinePlayerCreate = &FakeSequencer.StubPlayerCreate;
                stub.EngineTimelinePlayerDestroy = &FakeSequencer.StubPlayerDestroy;
                stub.EngineTimelinePlayerPlay = &FakeSequencer.StubPlayerPlay;
                stub.EngineTimelinePlayerPause = &FakeSequencer.StubPlayerPause;
                stub.EngineTimelinePlayerStop = &FakeSequencer.StubPlayerStop;
                stub.EngineTimelinePlayerSeek = &FakeSequencer.StubPlayerSeek;
                stub.EngineTimelinePlayerAdvance = &FakeSequencer.StubPlayerAdvance;
                stub.EngineTimelinePlayerState = &FakeSequencer.StubPlayerState;
                stub.EngineTimelinePlayerTime = &FakeSequencer.StubPlayerTime;
                stub.EngineTimelinePlayerSetRate = &FakeSequencer.StubPlayerSetRate;
                stub.EngineTimelinePlayerRate = &FakeSequencer.StubPlayerRate;
                stub.EngineTimelinePlayerSetLooping = &FakeSequencer.StubPlayerSetLooping;
                stub.EngineTimelinePlayerLooping = &FakeSequencer.StubPlayerLooping;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Timeline.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    FakeSequencer.Reset();
                    using var tl = new Timeline();
                    failures += Check(FakeSequencer.CreateCount == 1, "stub create reached (count=1)");

                    var tr = tl.AddTrack(TimelineTrackKind.Transform);
                    failures += Check(tr == 1, "stub AddTrack returns 1");
                    var c0 = tl.AddClip(tr, 5.0f, 2.0f);
                    var c1 = tl.AddClip(tr, 0.0f, 3.0f);
                    failures += Check(c0 == 1 && c1 == 2, "stub AddClip ids 1,2");

                    var got = tl.GetClip(tr, c1);
                    failures += Check(got.Start == 0.0f && got.Duration == 3.0f, "stub GetClip values");

                    failures += Check(tl.Duration == 7.0f, $"stub Duration 7 (got {tl.Duration})");
                    tl.Sort();
                    failures += Check(FakeSequencer.Sorts == 1, "stub Sort reaches native entry");

                    var at1 = tl.EvaluateActive(1.0f);
                    failures += Check(at1.Length == 1 && at1[0].Local == 1.0f && at1[0].Weight == 1.0f, "stub eval t=1 local=1 w=1");
                    var at4 = tl.EvaluateActive(4.0f);
                    failures += Check(at4.Length == 0, "stub eval t=4 empty");

                    failures += Check(tl.Trim(6.0f) == 0, "stub trim 6 removes none");
                    failures += Check(tl.Trim(2.0f) == 1, "stub trim 2 removes one");

                    // --- Playback stub probe (state machine over the stub) --
                    failures += Check(typeof(TimelinePlayer).GetMethod("Play") != null, "TimelinePlayer.Play exists");
                    failures += Check(typeof(TimelinePlayer).GetMethod("Pause") != null, "TimelinePlayer.Pause exists");
                    failures += Check(typeof(TimelinePlayer).GetMethod("Stop") != null, "TimelinePlayer.Stop exists");
                    failures += Check(typeof(TimelinePlayer).GetMethod("Seek") != null, "TimelinePlayer.Seek exists");
                    failures += Check(typeof(TimelinePlayer).GetMethod("Advance") != null, "TimelinePlayer.Advance exists");
                    failures += Check((int)TimelineState.Stopped == 0 && (int)TimelineState.Playing == 1 && (int)TimelineState.Paused == 2, "TimelineState 0/1/2");
                    failures += Check(sizeof(NativeApi.EngineTimelinePlayer) == 8, "NativeApi.EngineTimelinePlayer opaque 8B");
                    foreach (var want in new[] { "EngineTimelinePlayerCreate", "EngineTimelinePlayerDestroy", "EngineTimelinePlayerPlay", "EngineTimelinePlayerPause", "EngineTimelinePlayerStop", "EngineTimelinePlayerSeek", "EngineTimelinePlayerAdvance", "EngineTimelinePlayerState", "EngineTimelinePlayerTime", "EngineTimelinePlayerSetRate", "EngineTimelinePlayerRate", "EngineTimelinePlayerSetLooping", "EngineTimelinePlayerLooping" })
                    {
                        failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
                    }

                    using var pl = tl.CreatePlayer();
                    failures += Check(FakeSequencer.PlayerCreateCount == 1, "stub player_create reached (count=1)");
                    failures += Check(pl.State == TimelineState.Stopped && pl.Time == 0.0f, "stub player initial STOPPED @ 0");
                    pl.Play();
                    failures += Check(pl.State == TimelineState.Playing, "stub player PLAYING after play");
                    pl.Advance(1.5f);
                    failures += Check(pl.Time == 1.5f, $"stub player time 1.5 (got {pl.Time})");
                    pl.Pause();
                    failures += Check(pl.State == TimelineState.Paused, "stub player PAUSED after pause");
                    pl.Advance(1.0f);
                    failures += Check(pl.Time == 1.5f, "stub paused advance frozen");
                    pl.Seek(3.0f);
                    failures += Check(pl.Time == 3.0f, "stub player seek 3");
                    pl.Rate = 2.0f;
                    failures += Check(pl.Rate == 2.0f, "stub player rate 2");
                    pl.Looping = true;
                    failures += Check(pl.Looping, "stub player looping on");
                    pl.Play();
                    pl.Advance(1.0f);
                    failures += Check(pl.Time == 2.0f && pl.State == TimelineState.Playing, $"stub loop wrap 3+2 mod 3 = 2 (got {pl.Time})");
                    pl.Looping = false;
                    pl.Seek(6.5f);
                    failures += Check(pl.Time == 3.0f, $"stub seek clamps to duration 3 (got {pl.Time})");
                    pl.Advance(1.0f);
                    failures += Check(pl.Time == 3.0f && pl.State == TimelineState.Stopped, "stub non-loop pins + auto-stops");
                    pl.Play();
                    failures += Check(pl.Time == 0.0f && pl.State == TimelineState.Playing, "stub restart at 0");
                    pl.Stop();
                    failures += Check(pl.State == TimelineState.Stopped && pl.Time == 0.0f, "stub stop rewinds");
                    try { pl.Seek(float.NaN); failures += Check(false, "stub seek NaN must throw"); }
                    catch (InvalidOperationException) { failures += Check(true, "stub seek NaN throws"); }
                    try { pl.Advance(-1.0f); failures += Check(false, "stub advance -1 must throw"); }
                    catch (InvalidOperationException) { failures += Check(true, "stub advance -1 throws"); }
                    try { pl.Rate = 0.0f; failures += Check(false, "stub rate 0 must throw"); }
                    catch (InvalidOperationException) { failures += Check(true, "stub rate 0 throws"); }

                    tl.RemoveClip(tr, c0);
                    tl.RemoveTrack(tr);
                    failures += Check(FakeSequencer.Removes == 2, "stub removes reach native entries");
                }
                finally
                {
                    Timeline.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Timeline.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = TimelineLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            var savedOverride = Timeline.TableOverride;
            try
            {
                if (preloaded != IntPtr.Zero)
                {
                    TimelineLivePreload.LoadIntoApiLoader(preloaded);
                    table = (NativeApi.ApiTable*)Timeline.TableOverride;
                }
                else if (liveProbe)
                {
                    table = NativeApi.ApiLoader.Load();
                }
                else
                {
                    Console.WriteLine("[timeline] live native probe skipped (engine_core not preloaded; pass --live-native-probe to force).");
                    return failures;
                }

                Timeline.TableOverride = (IntPtr)table;
                using var tl = new Timeline();
                var tr = tl.AddTrack(TimelineTrackKind.Transform);
                var cLate = tl.AddClip(tr, 5.0f, 2.0f, targetEntity: 1);
                var cEarly = tl.AddClip(tr, 0.0f, 3.0f, targetEntity: 1);
                failures += Check(tl.Duration == 7.0f, $"live duration 7 (got {tl.Duration})");
                var back = tl.GetClip(tr, cEarly);
                failures += Check(back.Start == 0.0f && back.Duration == 3.0f, "live clip_get round-trip");
                tl.Sort();
                var at1 = tl.EvaluateActive(1.0f);
                failures += Check(at1.Length == 1 && at1[0].Clip == cEarly && at1[0].Local == 1.0f && at1[0].Weight == 1.0f, "live eval t=1 early w=1");
                failures += Check(tl.EvaluateActive(4.0f).Length == 0, "live eval t=4 gap empty");
                var cMid = tl.AddClip(tr, 1.0f, 3.0f, targetEntity: 1);
                tl.Sort();
                var at2 = tl.EvaluateActive(2.0f);
                failures += Check(at2.Length == 1 && at2[0].Clip == cMid, "live overlap later-start wins");
                failures += Check(at2[0].Weight > 0.0f && at2[0].Weight < 1.0f, "live overlap weight shared");
                var at3 = tl.EvaluateActive(3.0f);
                failures += Check(at3.Length == 1 && at3[0].Clip == cMid, "live half-open end drops [0,3) at t=3");
                failures += Check(tl.Trim(6.0f) == 0, "live trim 6 removes none");
                failures += Check(tl.GetClip(tr, cLate).Duration == 1.0f, "live trim clamps straddler to 1");
                failures += Check(tl.Trim(2.0f) == 1, "live trim 2 removes one");
                // --- Live playback round-trip (same contract as CTest) -----
                using var pl = tl.CreatePlayer();
                failures += Check(pl.State == TimelineState.Stopped && pl.Time == 0.0f, "live player initial STOPPED @ 0");
                pl.Play();
                failures += Check(pl.State == TimelineState.Playing, "live player PLAYING after play");
                pl.Advance(1.5f);
                failures += Check(pl.Time == 1.5f, $"live player time 1.5 (got {pl.Time})");
                pl.Pause();
                failures += Check(pl.State == TimelineState.Paused, "live player PAUSED after pause");
                pl.Advance(1.0f);
                failures += Check(pl.Time == 1.5f, "live paused advance frozen");
                pl.Looping = true;
                pl.Rate = 2.0f;
                failures += Check(pl.Looping && pl.Rate == 2.0f, "live rate 2 + loop on");
                pl.Play();
                pl.Advance(0.5f);
                failures += Check(pl.Time == 0.5f && pl.State == TimelineState.Playing, $"live loop wrap 1.5+1 mod 2 = 0.5 (got {pl.Time})");
                pl.Looping = false;
                pl.Stop();
                failures += Check(pl.State == TimelineState.Stopped && pl.Time == 0.0f, "live stop rewinds");
            }
            finally
            {
                Timeline.TableOverride = savedOverride;
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"live probe threw: {ex.GetType().Name}: {ex.Message}");
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        if (!ok)
        {
            Console.WriteLine($"[FAIL] {name}");
            return 1;
        }

        Console.WriteLine($"[PASS] {name}");
        return 0;
    }

    // Minimal in-memory sequencer behind the stub table: one track with a
    // clip list; mirrors the native half-open/overlap/trim contract so the
    // wrapper path is verified without native.
    private static class FakeSequencer
    {
        public static int CreateCount;
        public static int Sorts;
        public static int Removes;
        public static int PlayerCreateCount;
        private static readonly List<(uint Id, float Start, float Duration)> Clips = new();
        private static uint _nextClip = 1;

        // Stub player cursor (mirrors the native ABI 4.20 state machine).
        private static int _pState;
        private static float _pTime;
        private static float _pRate = 1.0f;
        private static bool _pLoop;

        public static void Reset()
        {
            CreateCount = 0;
            Sorts = 0;
            Removes = 0;
            PlayerCreateCount = 0;
            Clips.Clear();
            _nextClip = 1;
            _pState = 0;
            _pTime = 0.0f;
            _pRate = 1.0f;
            _pLoop = false;
        }

        private static float StubDurationNow()
        {
            return Clips.Count == 0 ? 0 : Clips.Max(c => c.Start + c.Duration);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe NativeApi.EngineTimelinePlayer* StubPlayerCreate(NativeApi.EngineTimeline* tl) { PlayerCreateCount++; return (NativeApi.EngineTimelinePlayer*)7; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe void StubPlayerDestroy(NativeApi.EngineTimelinePlayer* pl) { }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerPlay(NativeApi.EngineTimelinePlayer* pl)
        {
            if (_pState == 1) { return 0; }
            var d = StubDurationNow();
            if (d <= 0) { _pState = 0; _pTime = 0; return 0; }
            if (_pState == 0 && _pTime >= d) { _pTime = 0; }
            _pState = 1;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerPause(NativeApi.EngineTimelinePlayer* pl)
        {
            if (_pState == 1) { _pState = 2; }
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerStop(NativeApi.EngineTimelinePlayer* pl) { _pState = 0; _pTime = 0; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerSeek(NativeApi.EngineTimelinePlayer* pl, float t)
        {
            if (!float.IsFinite(t)) { return -2; }
            var d = StubDurationNow();
            _pTime = t < 0 ? 0 : (t > d ? d : t);
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerAdvance(NativeApi.EngineTimelinePlayer* pl, float dt)
        {
            if (!float.IsFinite(dt) || dt < 0) { return -2; }
            if (_pState != 1) { return 0; }
            var d = StubDurationNow();
            if (d <= 0) { _pState = 0; _pTime = 0; return 0; }
            _pTime += dt * _pRate;
            if (_pTime < d) { return 0; }
            if (_pLoop) { while (_pTime >= d) { _pTime -= d; } return 0; }
            _pTime = d; _pState = 0;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerState(NativeApi.EngineTimelinePlayer* pl, int* outState) { *outState = _pState; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerTime(NativeApi.EngineTimelinePlayer* pl, float* outTime) { *outTime = _pTime; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerSetRate(NativeApi.EngineTimelinePlayer* pl, float rate)
        {
            if (!float.IsFinite(rate) || rate <= 0) { return -2; }
            _pRate = rate;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerRate(NativeApi.EngineTimelinePlayer* pl, float* outRate) { *outRate = _pRate; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerSetLooping(NativeApi.EngineTimelinePlayer* pl, int looping) { _pLoop = looping != 0; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubPlayerLooping(NativeApi.EngineTimelinePlayer* pl, int* outLooping) { *outLooping = _pLoop ? 1 : 0; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe NativeApi.EngineTimeline* StubCreate() { CreateCount++; return (NativeApi.EngineTimeline*)1; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe void StubDestroy(NativeApi.EngineTimeline* tl) { }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe uint StubTrackAdd(NativeApi.EngineTimeline* tl, uint kind) => 1;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubTrackRemove(NativeApi.EngineTimeline* tl, uint track) { Removes++; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe uint StubClipAdd(NativeApi.EngineTimeline* tl, uint track, NativeApi.TimelineClip* clip)
        {
            var id = _nextClip++;
            Clips.Add((id, clip->Start, clip->Duration));
            return id;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubClipRemove(NativeApi.EngineTimeline* tl, uint track, uint clip) { Removes++; return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubDuration(NativeApi.EngineTimeline* tl, float* outDuration)
        {
            *outDuration = Clips.Count == 0 ? 0 : Clips.Max(c => c.Start + c.Duration);
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubClipGet(NativeApi.EngineTimeline* tl, uint track, uint clip, NativeApi.TimelineClip* outClip)
        {
            foreach (var c in Clips)
            {
                if (c.Id == clip)
                {
                    *outClip = new NativeApi.TimelineClip { Start = c.Start, Duration = c.Duration };
                    return 0;
                }
            }

            return -4;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubSort(NativeApi.EngineTimeline* tl) { Sorts++; Clips.Sort((a, b) => a.Start.CompareTo(b.Start)); return 0; }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubTrim(NativeApi.EngineTimeline* tl, float length)
        {
            var removed = Clips.RemoveAll(c => c.Start >= length);
            return removed;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static unsafe int StubEvaluateActive(NativeApi.EngineTimeline* tl, float t, NativeApi.TimelineActiveClip* outClips, uint capacity)
        {
            var live = Clips.Where(c => t >= c.Start && t < c.Start + c.Duration).ToList();
            if (live.Count == 0 || outClips == null)
            {
                return live.Count;
            }

            var win = live.OrderByDescending(c => c.Start).First();
            if ((uint)live.Count <= capacity)
            {
                outClips[0] = new NativeApi.TimelineActiveClip { Track = 1, Clip = win.Id, Local = t - win.Start, Weight = 1.0f };
            }

            return live.Count;
        }
    }

    private static class TimelineLivePreload
    {
        public static IntPtr TryPreloadEngineCoreHandle()
        {
            try
            {
                var dir = new DirectoryInfo(AppContext.BaseDirectory);
                var seen = new HashSet<string>(StringComparer.Ordinal);
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
            catch (Exception)
            {
            }

            return IntPtr.Zero;
        }

        public static unsafe void LoadIntoApiLoader(IntPtr handle)
        {
            if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
            {
                throw new EntryPointNotFoundException("engine_get_api_table not exported.");
            }

            var getTable = (delegate* unmanaged[Cdecl]<NativeApi.ApiTable*>)addr;
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

            Timeline.TableOverride = (IntPtr)table;
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
