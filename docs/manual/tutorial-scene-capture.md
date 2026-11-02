# Tutorial 3: headless scene capture (editor CLI)

Drive the editor without a window: list the scene tree, move an entity,
capture a screenshot PNG, and profile 60 frames — all through
`engine-cli` JSON-RPC over a Unix socket. Estimated time: 20 minutes.

## Step 1 — Start the editor server headless

Launch the editor with a renderer in offscreen mode (same hidden-window
path the samples use), then point `engine-cli` at its socket. Every
command below is JSON-RPC: request `{ method, params }`, response
`{ result | error }`.

## Step 2 — Inspect the scene tree

```csharp
// Snippet: parse a scene.tree response envelope (camelCase wire names).
// Requires: System.Text.Json.
string[] SceneNames(string json)
{
    using var doc = System.Text.Json.JsonDocument.Parse(json);
    return doc.RootElement.GetProperty("nodes").EnumerateArray()
        .Select(n => n.TryGetProperty("name", out var nm) ? (nm.GetString() ?? "?") : "?").ToArray();
}
```

`scene.tree` takes `{}` and returns `{ nodes: CliSceneNode[] }` with
ids, names, parents and component lists. `entity.get` reads one entity;
`entity.set` writes `px/py/pz/qx/...` fields and returns the stats the
CI gate needs.

## Step 3 — Capture a screenshot

`snapshot.capture --out <path> --pass final` renders the current scene
(not a stale present) through the live renderer and writes a PNG,
returning width, height and `HeadlessStub: false`:

```csharp
// Snippet: validate a snapshot.capture response.
// Requires: System.Text.Json.
(bool Ok, int W, int H) CheckCapture(string json)
{
    using var doc = System.Text.Json.JsonDocument.Parse(json);
    var root = doc.RootElement;
    bool stub = root.GetProperty("headlessStub").GetBoolean();
    int w = root.GetProperty("width").GetInt32();
    int h = root.GetProperty("height").GetInt32();
    return (!stub && w > 0 && h > 0, w, h);
}
```

Without a renderer the stub path answers `HeadlessStub: true` unchanged
— the test asserts `false` plus a PNG that exists with the window's
width and height.

## Step 4 — Profile 60 frames

`profile.capture --frames 60` runs the scene and returns per-frame
times, per-system timings and the GPU scene-pass time. The profiler
panel draws the same data live (last 300 frames). `export.pack` writes
the GUID-keyed `.pak` the player boots; `dotnet publish -r <rid>
/p:PublishAot=true` produces the self-contained player with < 1 s cold
boot. That is the M5 gate: drop a `.glb`/`.png` into `assets/` and it
is visible in under 16 ms / 1 frame.
