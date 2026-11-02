# Editor tour

`engine-editor` is the Avalonia UI over the headless `Engine.Editor`
library. Every panel below is a view over library state — the same calls
work headless in tests.

## Panels

The default layout is a menu bar (`File Edit GameObject Window Help`),
`Hierarchy` left, `Scene` (viewport) center, `Inspector` right spanning
full height, and `Project` + `Console` along the bottom.

```csharp
// Snippet: the panel names the default layout creates, in layout order.
// Mirrors MainWindowFactory.PanelNames; layout tests assert this set.
string[] DefaultPanels() => new[] { "Hierarchy", "Scene", "Inspector", "Project", "Console" };
```

## Hierarchy: spawn, rename, reparent, delete

The hierarchy lists entities from an `EditorSession`. Spawn and rename
through the session, then undo with the shared `Inspector` undo stack:

```csharp
// Snippet: spawn an entity and rename it through the Inspector undo stack.
// Requires: Engine.Editor (EditorSession, Inspector, HierarchyCommands).
void SpawnAndRename(Engine.Editor.EditorSession session, Engine.Editor.Inspector inspector)
{
    var cmd = new Engine.Editor.SpawnCommand(session, "Cube");
    inspector.Execute(cmd);
    var before = session.GetName(cmd.Spawned);
    var rename = new Engine.Editor.RenameCommand(session, cmd.Spawned, before, "Player");
    inspector.Execute(rename);
    System.Diagnostics.Debug.Assert(session.GetName(cmd.Spawned) == "Player");
    inspector.Undo();
    System.Diagnostics.Debug.Assert(session.GetName(cmd.Spawned) == "Cube");
}
```

Reparent by drag-and-drop in the UI (`ReparentCommand` underneath); delete
restores the whole subtree — meshes and user components byte-for-byte —
on undo (`DeleteCommand` holds a native snapshot).

## Inspector: edit one field, one undo step

Selecting an entity lists its components in `Expander`s. Each field
commits a single `SetFieldCommand` on `LostFocus` or Enter — never per
keystroke:

```csharp
// Snippet: write one LocalTransform pose as a single undoable edit.
// Requires: Engine.Editor (EditorSession, Inspector).
void NudgeX(Engine.Editor.EditorSession session, Engine.Editor.Inspector inspector, ulong entity)
{
    var before = new float[10];
    var after = new float[10];
    session.GetLocal(entity, before);
    System.Array.Copy(before, after, before.Length);
    after[0] += 1.0f;
    inspector.Execute(new Engine.Editor.SetPoseCommand(session, entity, before, after));
    inspector.Undo();
}
```

## Scene viewport and the editor camera

The viewport renders the world into a bitmap at ~60 Hz (16 ms timer)
and shows it through an `Image`. Hold the right mouse button and press
WASD/QE to fly, wheel to dolly, F to frame the selection — active only
while the viewport has pointer focus:

```csharp
// Snippet: the editor-camera fly basis used by the viewport.
// Requires: Engine.Editor (EditorCamera), System.Numerics.
Engine.Editor.EditorCamera FrameSelection()
{
    var camera = new Engine.Editor.EditorCamera();
    camera.Frame(System.Numerics.Vector3.Zero, 2.0f);
    return camera;
}
```

## Play mode

Toolbar `Play / Pause / Step / Stop` runs over `PlayModeController`:
`EnterPlay` snapshots the whole world (component-complete since P3-03),
and `ExitPlay` restores it and discards the snapshot — edits made while
playing never leak into the scene:

```csharp
// Snippet: play-mode round-trip invariant the staged test asserts.
// Requires: Engine.Editor (EditorSession, PlayModeController).
void PlayRestores(Engine.Editor.EditorSession session, Engine.Editor.PlayModeController play)
{
    var before = new float[10];
    session.GetLocal(session.GetRoot(0), before);
    play.EnterPlay();
    var moved = new float[10];
    System.Array.Copy(before, moved, before.Length);
    moved[1] += 5.0f;
    session.SetLocal(session.GetRoot(0), moved);
    play.ExitPlay();
    var after = new float[10];
    session.GetLocal(session.GetRoot(0), after);
    System.Diagnostics.Debug.Assert(before.AsSpan().SequenceEqual(after));
}
```

## Project browser and the console

The Project panel lists `assets/` (folders, `.glb`, `.png`,
`.scene.json`, `.cs`) from the asset GUID database; dragging a `.glb`
into the viewport imports it and spawns an entity at the ground point in
one undo step. The console is a virtualized list with level filters and
collapse; the profiler draws the last 300 frame times plus per-system
timings and the GPU scene-pass time from timestamp queries.
