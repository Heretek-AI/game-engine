// Ui.cs — P5-04: managed game UI over the ABI 4.12 ui_* tail.
//
// UIDocument wraps an EngineUi (one headless RmlUi context + CPU frame):
// Load("ui/hud.rml") loads RML from memory, element lookup/text set goes
// through ui_set_text/ui_get_text/ui_has_element, and clicks injected via
// ui_inject_click queue natively — C# drains the count through
// ClickCount/ConsumeClicks (zero managed alloc, same rule as P2-05
// collision events). All entries are plain Cdecl (render may block —
// never SuppressGCTransition, same rule as run_systems).
//
// The document paints a 640x360 RGBA8 HUD (label `#lbl` + button `#btn`);
// Render() refreshes the backbuffer and CopyFrame() pulls the bytes the
// golden compares. OnClick subscribes the single managed handler the
// acceptance test uses to prove exactly-once delivery.
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Managed owner of a native game UI world: HUD document, element text,
/// click injection, and the 640x360 RGBA frame. The handle is the native
/// EngineUi pointer (borrowed — destroy this only after use).
/// </summary>
public sealed unsafe class UIDocument : IDisposable
{
    public const int FrameWidth = 640;
    public const int FrameHeight = 360;
    public const int FrameBytes = FrameWidth * FrameHeight * 4;

    private NativeApi.EngineUi* _ui;
    private bool _disposed;

    // Test/live-probe injection (same pattern as Audio/Physics2D).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Creates a UI world (fontDir holds LatoLatin-Regular.ttf).</summary>
    public UIDocument(string? fontDir = null)
    {
        var dir = fontDir ?? string.Empty;
        var dirBytes = Encoding.UTF8.GetBytes(dir + "\0");
        NativeApi.EngineUi* ui;
        fixed (byte* dirPtr = dirBytes)
        {
            ui = TablePtr->EngineUiCreate(dirPtr);
        }

        if (ui == null)
        {
            throw new InvalidOperationException("ui_create failed (allocation failure).");
        }

        _ui = ui;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_ui != null)
            {
                TablePtr->EngineUiDestroy(_ui);
                _ui = null;
            }
        }
    }

    /// <summary>Loads an RML document from memory (replaces the HUD).</summary>
    public void Load(string rml)
    {
        ThrowIfDisposed();
        var bytes = Encoding.UTF8.GetBytes(rml + "\0");
        int rc;
        fixed (byte* ptr = bytes)
        {
            rc = TablePtr->EngineUiLoad(_ui, ptr);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"ui_load failed (rc {rc}).");
        }
    }

    /// <summary>Sets an element's inner RML/text (`lbl`, `btn`, ...).</summary>
    public void SetText(string elementId, string text)
    {
        ThrowIfDisposed();
        var idBytes = Encoding.UTF8.GetBytes(elementId + "\0");
        var textBytes = Encoding.UTF8.GetBytes(text + "\0");
        int rc;
        fixed (byte* idPtr = idBytes)
        fixed (byte* textPtr = textBytes)
        {
            rc = TablePtr->EngineUiSetText(_ui, idPtr, textPtr);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"ui_set_text failed for '{elementId}' (rc {rc}).");
        }
    }

    /// <summary>Reads an element's inner RML/text.</summary>
    public string GetText(string elementId)
    {
        ThrowIfDisposed();
        var idBytes = Encoding.UTF8.GetBytes(elementId + "\0");
        Span<byte> outBytes = stackalloc byte[4096];
        int n;
        fixed (byte* idPtr = idBytes)
        fixed (byte* outPtr = outBytes)
        {
            n = TablePtr->EngineUiGetText(_ui, idPtr, outPtr, (uint)outBytes.Length);
        }

        if (n < 0)
        {
            throw new InvalidOperationException($"ui_get_text failed for '{elementId}'.");
        }

        return Encoding.UTF8.GetString(outBytes[..n]);
    }

    /// <summary>Reports whether an element id exists in the document.</summary>
    public bool HasElement(string elementId)
    {
        ThrowIfDisposed();
        var idBytes = Encoding.UTF8.GetBytes(elementId + "\0");
        int found;
        fixed (byte* idPtr = idBytes)
        {
            found = TablePtr->EngineUiHasElement(_ui, idPtr);
        }

        return found != 0;
    }

    /// <summary>Injects a mouse click at canvas coordinates.</summary>
    public void InjectClick(float x, float y)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineUiInjectClick(_ui, x, y) != 0)
        {
            throw new InvalidOperationException("ui_inject_click failed.");
        }
    }

    /// <summary>Renders the HUD into the CPU backbuffer.</summary>
    public void Render()
    {
        ThrowIfDisposed();
        if (TablePtr->EngineUiRender(_ui) != 0)
        {
            throw new InvalidOperationException("ui_render failed.");
        }
    }

    /// <summary>Copies the last rendered frame (640x360 RGBA8).</summary>
    public byte[] CopyFrame()
    {
        ThrowIfDisposed();
        var frame = new byte[FrameBytes];
        fixed (byte* outPtr = frame)
        {
            if (TablePtr->EngineUiCopyFrame(_ui, outPtr, (uint)frame.Length) != 0)
            {
                throw new InvalidOperationException("ui_copy_frame failed.");
            }
        }

        return frame;
    }

    /// <summary>Queued click count (fires on the HUD button).</summary>
    public uint ClickCount
    {
        get
        {
            ThrowIfDisposed();
            return TablePtr->EngineUiClickCount(_ui);
        }
    }

    /// <summary>Drains the click queue (returns the count, then clears).</summary>
    public uint ConsumeClicks()
    {
        ThrowIfDisposed();
        return TablePtr->EngineUiConsumeClicks(_ui);
    }
}
