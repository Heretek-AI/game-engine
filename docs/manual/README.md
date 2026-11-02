# P7-06 user manual

The engine user manual: getting started, a tour of the editor, and the
scripting, rendering and physics guides, plus three step-by-step
tutorials. Every C# snippet in `docs/manual/` is extracted by
`tools/manual_snippets/` and compiled in CI — keep every snippet
compilable or the `manual-snippets` job fails. Rules for writing
snippets are in `docs/manual/README.md` under "For manual authors".

Entry points:

- `docs/manual/getting-started.md` — install, first project, first run
- `docs/manual/editor-tour.md` — panels, hierarchy, inspector, play mode
- `docs/manual/scripting.md` — components, systems, hot reload
- `docs/manual/rendering.md` — camera, lights, meshes, screenshots
- `docs/manual/physics.md` — bodies, colliders, raycasts, events
- `docs/manual/tutorial-hello-cube.md` — tutorial 1: spinning cube
- `docs/manual/tutorial-marble-physics.md` — tutorial 2: rolling marble
- `docs/manual/tutorial-scene-capture.md` — tutorial 3: headless capture

## For manual authors (snippet contract)

Every ```csharp fence in `docs/manual/` is extracted and compiled in CI
(`managed` job, "Compile manual snippets"). The rules:

1. One fence = one compilable member (a full method, property, or type)
   or a block of top-level statements. The extractor wraps statements in
   a `Run()` method; members compile as-is inside a holder class.
2. No top-level statements mixed with member declarations in one fence;
   no `namespace` declarations, no `file`-scoped anything.
3. Type snippets compile as *nested* types (the holder is a plain
   class, so `public partial struct Mover : IComponent` nests fine).
4. `unsafe` and `Engine.*` references are available; `System.Text.Json`
   comes transitively via `Engine.Editor`. Anything else must be fully
   qualified (`System.IO.Path`, `System.MathF`, ...).
5. Instance-style methods are fine (holder is not static). Statements
   using `using var` are fine too — the old `using\s` regex false
   positive on `using var` statements is fixed with a lookahead.

Regenerate + verify locally before pushing:

```bash
dotnet run --project tools/manual_snippets/manual_snippets.csproj -c Release -- .
dotnet build tools/manual_snippets/manual_check.csproj -c Release --nologo
```
