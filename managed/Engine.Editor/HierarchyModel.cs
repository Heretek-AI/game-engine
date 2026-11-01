// HierarchyModel.cs — M5-02: Scene Hierarchy panel model over EcsChildOf.
//
// A headless-safe view-model (no GPU): walks roots -> children through the
// session and materializes an immutable tree of Nodes (id + prefab flag).
// The docking UI renders this tree; M5-03's engine-cli `scene tree` JSON
// dump serializes the same shape. Refresh() rebuilds from native (editor
// scenes fit in memory; per-frame refresh allocates — panels refresh on
// demand, not on the frame path).
using Engine.Runtime;

namespace Engine.Editor;

/// <summary>
/// One hierarchy row: entity id, display name, prefab-instance flag, nested children.
/// </summary>
public sealed record HierarchyNode(ulong Id, string Name, bool IsPrefab, IReadOnlyList<HierarchyNode> Children);

/// <summary>
/// Scene Hierarchy panel model: roots -> children via EcsChildOf queries.
/// </summary>
public sealed class HierarchyModel
{
    private readonly EditorSession _session;

    public HierarchyModel(EditorSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
    }

    /// <summary>The bound session (panel command routing).</summary>
    public EditorSession Session => _session;

    /// <summary>All roots with their subtrees.</summary>
    public IReadOnlyList<HierarchyNode> Roots => BuildRoots();

    /// <summary>Rebuild the tree from native (call after structural edits).</summary>
    public IReadOnlyList<HierarchyNode> Refresh() => BuildRoots();

    /// <summary>Depth-first id lookup (null when absent).</summary>
    public HierarchyNode? Find(ulong id) => FindIn(Roots, id);

    private List<HierarchyNode> BuildRoots()
    {
        var count = _session.CountRoots();
        var roots = new List<HierarchyNode>((int)Math.Min(count, int.MaxValue));
        for (ulong i = 0; i < count; i++)
        {
            var id = _session.GetRoot(i);
            roots.Add(BuildNode(id));
        }

        return roots;
    }

    private HierarchyNode BuildNode(ulong id)
    {
        var kids = new List<HierarchyNode>();
        var n = _session.CountChildren(id);
        for (ulong i = 0; i < n; i++)
        {
            kids.Add(BuildNode(_session.GetChild(id, i)));
        }

        var prefab = _session.IsPrefabInstance(id);
        var name = _session.GetName(id);
        return new HierarchyNode(id, name, prefab, kids);
    }

    private static HierarchyNode? FindIn(IReadOnlyList<HierarchyNode> nodes, ulong id)
    {
        foreach (var node in nodes)
        {
            if (node.Id == id)
            {
                return node;
            }

            var found = FindIn(node.Children, id);
            if (found != null)
            {
                return found;
            }
        }

        return null;
    }
}
