# Getting started

Build your first project: install the toolchain, create a project, run
the spinning-cube sample headless, and take a screenshot.

## 1. Install the toolchain

Install the native build tools (§3.1 of the parity prompt: Ninja, Clang,
CMake, the Vulkan headers and drivers, the X11/Wayland dev packages) and
the .NET 10 SDK:

```csharp
// Snippet: check the .NET SDK major version from a script or CI step.
// Runs anywhere dotnet is on PATH; no engine assemblies needed.
var start = new System.Diagnostics.ProcessStartInfo("dotnet", "--version");
start.RedirectStandardOutput = true;
using var proc = System.Diagnostics.Process.Start(start);
proc!.WaitForExit();
var version = proc.StandardOutput.ReadToEnd().Trim();
System.Console.WriteLine($"dotnet SDK: {version}");
if (!version.StartsWith("10."))
{
    throw new System.InvalidOperationException($"Need .NET 10 SDK, found {version}.");
}
```

## 2. Build the engine

From the repository root (the folder containing `Engine.slnx`):

```csharp
// Snippet: build every managed project in Release.
// Runs in the repo root; fails with the build log on error.
var psi = new System.Diagnostics.ProcessStartInfo("dotnet", "build Engine.slnx -c Release --nologo");
psi.RedirectStandardOutput = true;
psi.RedirectStandardError = true;
using var build = System.Diagnostics.Process.Start(psi);
build!.WaitForExit();
if (build.ExitCode != 0)
{
    throw new System.InvalidOperationException("dotnet build Engine.slnx failed; see log above.");
}
```

## 3. Create a project

A project is a folder with a `project.json` manifest plus `assets/` and
`scenes/` directories. The editor creates one for you (see the
[editor tour](editor-tour.md)); the layout it writes is:

```csharp
// Snippet: scaffold the same layout the editor's New Project writes.
// Pass an empty temp dir; creates project.json, assets/, scenes/.
// Requires: System.Text.Json.Nodes (JsonObject).
void ScaffoldProject(string dir)
{
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(dir, "assets"));
    System.IO.Directory.CreateDirectory(System.IO.Path.Combine(dir, "scenes"));
    var manifest = new System.Text.Json.Nodes.JsonObject
    {
        ["name"] = "MyGame",
        ["startScene"] = "scenes/main.scene.json",
        ["gameAssembly"] = "bin/Game.dll",
    };
    System.IO.File.WriteAllText(
        System.IO.Path.Combine(dir, "project.json"),
        manifest.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
}
```

The manifest fields are `name` (shown in the editor title bar),
`startScene` (path relative to the project dir, loaded at startup) and
`gameAssembly` (your compiled scripts, loaded through the collectible
hot-reload context).

## 4. Run the HelloCube sample headless

`HelloCube` is the smallest rendering sample: one camera, one light,
one spinning cube. Run it offscreen (no window) for 10 frames:

```csharp
// Snippet: resolve sample-relative output paths for a headless run.
// Pure path math so the tutorial never depends on the working dir.
string SampleOut(string sampleDir, string file)
{
    var outDir = System.IO.Path.Combine(sampleDir, "out");
    System.IO.Directory.CreateDirectory(outDir);
    return System.IO.Path.Combine(outDir, file);
}
```

With the native library built, run the sample against it and print the
result marker:

```csharp
// Snippet: report a sample frame budget (fixed 60 Hz steps).
// Pure math: frames * (1/60 s); used to sanity-check autoplay logs.
double FrameBudgetSeconds(int frames) => frames / 60.0;
```

Expected output ends with `HELLO-CUBE-OK`. If you pass
`--screenshot out.png`, the last frame is written as a PNG; compare it
with the golden in `native/tests/golden/` (channel tolerance ±2, at most
0.1% of pixels differ).

## 5. Where to go next

- [Editor tour](editor-tour.md) — open the project in `engine-editor`.
- [Scripting](scripting.md) — write your first component and system.
- [Tutorial 1](tutorial-hello-cube.md) — build the spinning cube step by step.
