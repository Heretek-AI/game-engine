// EditorDockFactory.cs — P3-12: Dock.Avalonia factory for the editor layout.
//
// Builds the default dock model matching the P3-01 shell: Hierarchy on the
// left; Scene + Game tabbed in the center; Inspector on the right;
// Project + Console + Profiler tabbed along the bottom. Panels are Controls
// resolved at render time by EditorViewLocator (tool id → panel instance);
// the dock MODEL carries no Control payload so layout.json stays pure JSON.
// Panels are closable (CanClose): closing removes the tool from the layout;
// ResetLayout rebuilds the default.
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Dock.Model.Avalonia;
using Dock.Model.Avalonia.Controls;
using Dock.Model.Controls;
using Dock.Model.Core;

namespace Engine.EditorApp;

/// <summary>Resolves tool ids to their real panel Controls (P3-12).</summary>
public sealed class EditorViewLocator : IDataTemplate
{
    private readonly IReadOnlyDictionary<string, Control> _panels;

    public EditorViewLocator(IReadOnlyDictionary<string, Control> panels)
    {
        _panels = panels;
    }

    public Control? Build(object? data)
    {
        if (data is IDockable dockable && _panels.TryGetValue(dockable.Id, out var panel))
        {
            return panel;
        }

        return null;
    }

    public bool Match(object? data) =>
        data is IDockable dockable && _panels.ContainsKey(dockable.Id);
}

/// <summary>Dock factory creating the editor's tool layout (P3-12).</summary>
public sealed class EditorDockFactory : Factory
{
    public const string HierarchyId = "Hierarchy";
    public const string InspectorId = "Inspector";
    public const string SceneId = "Scene";
    public const string GameId = "Game";
    public const string ProjectId = "Project";
    public const string ConsoleId = "Console";
    public const string ProfilerId = "Profiler";

    private readonly Dictionary<string, Control> _panels = new(StringComparer.Ordinal);

    public EditorDockFactory(
        Control hierarchy,
        Control inspector,
        Control scene,
        Control game,
        Control project,
        Control console,
        Control profiler)
    {
        _panels[HierarchyId] = hierarchy;
        _panels[InspectorId] = inspector;
        _panels[SceneId] = scene;
        _panels[GameId] = game;
        _panels[ProjectId] = project;
        _panels[ConsoleId] = console;
        _panels[ProfilerId] = profiler;
    }

    public IReadOnlyDictionary<string, Control> Panels => _panels;

    public EditorViewLocator CreateViewLocator() => new(_panels);

    public override IRootDock CreateLayout()
    {
        var hierarchy = ToolFor(HierarchyId, HierarchyId);
        var inspector = ToolFor(InspectorId, InspectorId);
        var scene = ToolFor(SceneId, SceneId);
        var game = ToolFor(GameId, GameId);
        var project = ToolFor(ProjectId, ProjectId);
        var console = ToolFor(ConsoleId, ConsoleId);
        var profiler = ToolFor(ProfilerId, ProfilerId);

        var left = CreateToolDock();
        left.Id = "Left";
        left.Alignment = Alignment.Left;
        left.Proportion = 0.17;
        left.VisibleDockables = CreateList<IDockable>(hierarchy);
        left.ActiveDockable = hierarchy;

        var right = CreateToolDock();
        right.Id = "Right";
        right.Alignment = Alignment.Right;
        right.Proportion = 0.20;
        right.VisibleDockables = CreateList<IDockable>(inspector);
        right.ActiveDockable = inspector;

        var center = CreateToolDock();
        center.Id = "Center";
        center.Alignment = Alignment.Unset;
        center.VisibleDockables = CreateList<IDockable>(scene, game);
        center.ActiveDockable = scene;

        var middle = CreateProportionalDock();
        middle.Id = "Middle";
        middle.Orientation = Orientation.Horizontal;
        middle.VisibleDockables = CreateList<IDockable>(
            left, new ProportionalDockSplitter(), center, new ProportionalDockSplitter(), right);

        var bottom = CreateToolDock();
        bottom.Id = "Bottom";
        bottom.Alignment = Alignment.Bottom;
        bottom.Proportion = 0.25;
        bottom.VisibleDockables = CreateList<IDockable>(project, console, profiler);
        bottom.ActiveDockable = project;

        var body = CreateProportionalDock();
        body.Id = "Body";
        body.Orientation = Orientation.Vertical;
        body.VisibleDockables = CreateList<IDockable>(
            middle, new ProportionalDockSplitter(), bottom);

        var layout = CreateRootDock();
        layout.Id = "Root";
        layout.VisibleDockables = CreateList<IDockable>(body);
        layout.DefaultDockable = body;
        layout.ActiveDockable = body;
        return layout;
    }

    public override void InitLayout(IDockable layout)
    {
        // ContextLocator restores the per-tool panel Views after
        // deserialization: InitDockable fills any null Context from here.
        ContextLocator = new Dictionary<string, Func<object?>>();
        foreach (var (id, panel) in _panels)
        {
            var captured = panel;
            ContextLocator[id] = () => captured;
        }

        DockableLocator = new Dictionary<string, Func<IDockable?>>();
        var tools = new Dictionary<string, IDockable>(StringComparer.Ordinal);
        CollectTools(layout, tools);
        foreach (var (id, tool) in tools)
        {
            var captured = tool;
            DockableLocator[id] = () => captured;
        }

        base.InitLayout(layout);
    }

    private static void CollectTools(IDockable dockable, Dictionary<string, IDockable> tools)
    {
        // Same visible + floating + hidden/pinned walk as focus restore
        // (DockWalk.All): floated tools live under Windows, not
        // VisibleDockables, and must stay in the locator collection.
        foreach (var node in DockWalk.All(dockable))
        {
            if (node is ITool tool)
            {
                tools[tool.Id] = tool;
            }
        }
    }

    private Tool ToolFor(string id, string title)
    {
        // The model carries NO Control payload: Content/Context holding a
        // Border breaks SystemTextJson ("metadata was not generated").
        // Rendering resolves the panel via EditorViewLocator (id → Control).
        var tool = (Tool)CreateTool();
        tool.Id = id;
        tool.Title = title;
        tool.CanClose = true;
        return tool;
    }
}
