// EngineCoreLocator.cs — P0-06: one shared engine_core locator for all live probes.
//
// Out-of-tree builds (cmake -B /tmp/oot) never contain Engine.slnx above
// them, so the old upward-walk copies returned IntPtr.Zero and the fallback
// NativeLibrary.TryLoad("engine_core") failed even with LD_LIBRARY_PATH.
// CTest now sets ENGINE_CORE_PATH=$<TARGET_FILE:engine_core> on every
// live-probing managed test; this helper loads that path first (pinning the
// libflecs sibling in the same directory), then falls back to the staged
// CWD/appdir siblings and the legacy in-tree build-tree walk.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Tests;

internal static unsafe class EngineCoreLocator
{
    public static IntPtr TryLoad()
    {
        var fromEnv = Environment.GetEnvironmentVariable("ENGINE_CORE_PATH");
        if (!string.IsNullOrEmpty(fromEnv) && File.Exists(fromEnv))
        {
            var h = PreloadPair(Path.GetFullPath(fromEnv));
            if (h != IntPtr.Zero)
            {
                return h;
            }
        }

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

                    foreach (var preset in new[] { "ci-linux", "ci-windows", "ci-macos" })
                    {
                        var nativeDir = Path.Combine(dir.FullName, "build", preset, "native");
                        foreach (var lib in new[] { "libengine_core.so.0.1.0", "libengine_core.so", "libengine_core.dylib", "engine_core.dll" })
                        {
                            var cand = Path.Combine(nativeDir, lib);
                            if (File.Exists(cand))
                            {
                                var h = PreloadPair(Path.GetFullPath(cand));
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

    public static NativeApi.ApiTable* LoadTable(IntPtr handle)
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

        if (table->Size < (ulong)sizeof(NativeApi.ApiTable)
            || table->AbiVersionMajor != NativeApi.AbiVersionMajor
            || table->AbiVersionMinor != NativeApi.AbiVersionMinor
            || table->AbiVersionPatch != NativeApi.AbiVersionPatch)
        {
            throw new InvalidOperationException(
                $"ABI version mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch}.");
        }

        if (table->ColumnStride != NativeApi.ColumnStride
            || sizeof(NativeApi.LocalTransform) != 64
            || sizeof(NativeApi.WorldTransform) != 64)
        {
            throw new InvalidOperationException("ABI column stride mismatch.");
        }

        return table;
    }

    public static IntPtr PreloadPair(string corePath)
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
