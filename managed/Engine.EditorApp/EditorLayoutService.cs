// EditorLayoutService.cs — P3-12: save/restore the dock layout as JSON.
//
// The layout is the Dock.Avalonia root model serialized with
// Dock.Serializer (Newtonsoft backend). Saved to
// `<project>/.editor/layout.json`, restored on open; ResetLayout rebuilds
// the P3-01 default. Panels are NOT in the JSON (Controls are not
// serializable): after deserialization the factory re-attaches them through
// ContextLocator + InitLayout, and the ViewLocator maps tool id → Control.
using Dock.Model.Avalonia.Json;
using Dock.Model.Controls;
using Dock.Model.Core;

namespace Engine.EditorApp;

/// <summary>Persists the editor dock layout to layout.json (P3-12).</summary>
public sealed class EditorLayoutService
{
    public const string RelativePath = ".editor/layout.json";

    private readonly EditorDockFactory _factory;
    private readonly AvaloniaDockSerializer _serializer = new();

    public EditorLayoutService(EditorDockFactory factory)
    {
        _factory = factory;
    }

    public EditorDockFactory Factory => _factory;

    public IRootDock Layout { get; private set; } = null!;

    public static string LayoutPathFor(string projectDir) =>
        Path.Combine(projectDir, ".editor", "layout.json");

    /// <summary>Build the default layout (P3-01 arrangement).</summary>
    public IRootDock CreateDefault()
    {
        Layout = _factory.CreateLayout();
        _factory.InitLayout(Layout);
        return Layout;
    }

    /// <summary>Serialize the current layout to JSON.</summary>
    /// <remarks>
    /// The dock serializer persists <c>Context</c> (an untyped object
    /// payload) with no metadata for Control values, so panel Controls must
    /// be cleared before serializing and restored after. The layout JSON
    /// therefore contains ids/arrangement only; panels re-attach by id.
    /// </remarks>
    public string Serialize()
    {
        var saved = new Dictionary<string, object?>(StringComparer.Ordinal);
        ClearContexts(Layout, saved);
        try
        {
            return _serializer.Serialize(Layout);
        }
        finally
        {
            RestoreContexts(Layout, saved);
        }
    }

    /// <summary>Restore a layout serialized with <see cref="Serialize"/>.</summary>
    public IRootDock Deserialize(string json)
    {
        var restored = _serializer.Deserialize<IRootDock>(json)
            ?? throw new InvalidOperationException("layout.json did not contain a root dock.");
        // Preserve the saved focus: base InitLayout re-asserts framework
        // defaults (Body), which would otherwise make a reserialize differ
        // from the saved JSON. Re-resolve by id (the deserialized object
        // graph is intact; only the focus pointers get reset).
        var focusedId = restored.FocusedDockable?.Id;
        _factory.InitLayout(restored);
        if (!string.IsNullOrEmpty(focusedId) && FindTool(restored, focusedId) is { } focused)
        {
            restored.FocusedDockable = focused;
        }

        Layout = restored;
        return restored;
    }

    /// <summary>Rebuild the default layout, discarding user arrangement.</summary>
    public IRootDock ResetLayout() => CreateDefault();

    /// <summary>Save the current layout to <c>&lt;project&gt;/.editor/layout.json</c>.</summary>
    public void Save(string projectDir)
    {
        var path = LayoutPathFor(projectDir);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, Serialize());
    }

    /// <summary>
    /// Open a project: restore the saved layout when present, else the default.
    /// Returns true when a saved layout was restored.
    /// </summary>
    public bool Open(string projectDir)
    {
        var path = LayoutPathFor(projectDir);
        if (File.Exists(path))
        {
            try
            {
                Deserialize(File.ReadAllText(path));
                return true;
            }
            catch (Exception ex) when (ex is IOException
                or UnauthorizedAccessException
                or NotSupportedException
                or System.Text.Json.JsonException
                or InvalidOperationException)
            {
                // Corrupt, truncated, hand-edited or merge-conflicted
                // layout.json (including a file with no root dock, which
                // Deserialize rejects): fall back to the default instead of
                // making the project unopenable. The catch is deliberately
                // narrow — unrelated failures (OOM, etc.) still propagate.
                Console.Error.WriteLine($"[editor] ignoring invalid layout '{path}': {ex.Message}");
            }
        }

        CreateDefault();
        return false;
    }

    /// <summary>Find a tool by id anywhere in the layout, including floating windows.</summary>
    private static ITool? FindTool(IDockable node, string id)
    {
        foreach (var candidate in DockWalk.All(node))
        {
            if (candidate is ITool tool && tool.Id == id)
            {
                return tool;
            }
        }

        return null;
    }

    private static void ClearContexts(IDockable node, Dictionary<string, object?> saved)
    {
        if (node is Dock.Model.Avalonia.Controls.Tool tool)
        {
            saved[tool.Id] = tool.Context;
            tool.Context = null;
        }
        else if (node is ITool plainTool)
        {
            saved[plainTool.Id] = plainTool.Context;
            plainTool.Context = null;
        }

        if (node is IDock dock)
        {
            foreach (var child in dock.VisibleDockables ?? [])
            {
                ClearContexts(child, saved);
            }
        }
    }

    private static void RestoreContexts(IDockable node, Dictionary<string, object?> saved)
    {
        if (node is ITool tool && saved.TryGetValue(tool.Id, out var context))
        {
            tool.Context = context;
        }

        if (node is IDock dock)
        {
            foreach (var child in dock.VisibleDockables ?? [])
            {
                RestoreContexts(child, saved);
            }
        }
    }
}
