// DockWalk.cs — one traversal for every dockable in a layout tree.
//
// FindTool (focus restore) and EditorDockFactory.CollectTools (locator
// collection) must see the same set: each dock's VisibleDockables, each
// floating window's Layout, and the root's hidden + pinned collections.
// A floated tool lives under Windows, not VisibleDockables — walking only
// the visible tree silently drops it from both paths.
using Dock.Model.Controls;
using Dock.Model.Core;

namespace Engine.EditorApp;

internal static class DockWalk
{
    public static IEnumerable<IDockable> All(IDockable root)
    {
        var seen = new HashSet<IDockable>(ReferenceEqualityComparer.Instance);
        foreach (var node in Walk(root))
        {
            if (seen.Add(node))
            {
                yield return node;
            }
        }
    }

    private static IEnumerable<IDockable> Walk(IDockable node)
    {
        yield return node;

        if (node is IDock dock)
        {
            foreach (var child in dock.VisibleDockables ?? [])
            {
                foreach (var nested in Walk(child))
                {
                    yield return nested;
                }
            }
        }

        if (node is IRootDock root)
        {
            foreach (var window in root.Windows ?? [])
            {
                if (window?.Layout is { } layout)
                {
                    foreach (var nested in Walk(layout))
                    {
                        yield return nested;
                    }
                }
            }

            foreach (var extra in HiddenAndPinned(root))
            {
                foreach (var nested in Walk(extra))
                {
                    yield return nested;
                }
            }
        }
    }

    private static IEnumerable<IDockable> HiddenAndPinned(IRootDock root)
    {
        foreach (var list in new[] {
            root.HiddenDockables,
            root.LeftPinnedDockables,
            root.RightPinnedDockables,
            root.TopPinnedDockables,
            root.BottomPinnedDockables,
        })
        {
            if (list is null)
            {
                continue;
            }

            foreach (var item in list)
            {
                if (item is not null)
                {
                    yield return item;
                }
            }
        }

        if (root.PinnedDock is { } pinned)
        {
            yield return pinned;
        }
    }
}
