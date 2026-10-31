// Input.cs — P2-02: C# Input API + JSON action maps.
//
// Static Input reads the native EngineInputState snapshot zero-copy
// (input_get_state borrows renderer memory; valid until the next frame
// pump). Key enum values equal SDL scancodes (SDL_scancode.h), so a C#
// Key casts directly to the native table index. Action maps load once
// from input_actions.json ({"axes":{"Horizontal":{"negative":["A","Left"],
// "positive":["D","Right"],"gamepadAxis":"LeftX"}}}) via the static
// constructor + Reload for tests. No managed gameplay state:
// Input only views the snapshot pointer + parsed map (load-once config,
// not frame state).
using System.Text.Json;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Keyboard key. Values equal SDL scancodes (SDL_scancode.h).</summary>
public enum Key
{
    Unknown = 0,
    A = 4,
    B = 5,
    C = 6,
    D = 7,
    E = 8,
    F = 9,
    G = 10,
    H = 11,
    I = 12,
    J = 13,
    K = 14,
    L = 15,
    M = 16,
    N = 17,
    O = 18,
    P = 19,
    Q = 20,
    R = 21,
    S = 22,
    T = 23,
    U = 24,
    V = 25,
    W = 26,
    X = 27,
    Y = 28,
    Z = 29,
    D1 = 30,
    D2 = 31,
    D3 = 32,
    D4 = 33,
    D5 = 34,
    D6 = 35,
    D7 = 36,
    D8 = 37,
    D9 = 38,
    D0 = 39,
    Return = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,
    Minus = 45,
    Equals = 46,
    Leftbracket = 47,
    Rightbracket = 48,
    Backslash = 49,
    Nonushash = 50,
    Semicolon = 51,
    Apostrophe = 52,
    Grave = 53,
    Comma = 54,
    Period = 55,
    Slash = 56,
    Capslock = 57,
    F1 = 58,
    F2 = 59,
    F3 = 60,
    F4 = 61,
    F5 = 62,
    F6 = 63,
    F7 = 64,
    F8 = 65,
    F9 = 66,
    F10 = 67,
    F11 = 68,
    F12 = 69,
    Printscreen = 70,
    Scrolllock = 71,
    Pause = 72,
    Insert = 73,
    Home = 74,
    Pageup = 75,
    Delete = 76,
    End = 77,
    Pagedown = 78,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    Numlockclear = 83,
    KpDivide = 84,
    KpMultiply = 85,
    KpMinus = 86,
    KpPlus = 87,
    KpEnter = 88,
    Kp1 = 89,
    Kp2 = 90,
    Kp3 = 91,
    Kp4 = 92,
    Kp5 = 93,
    Kp6 = 94,
    Kp7 = 95,
    Kp8 = 96,
    Kp9 = 97,
    Kp0 = 98,
    KpPeriod = 99,
    Nonusbackslash = 100,
    Application = 101,
    Power = 102,
    KpEquals = 103,
    F13 = 104,
    F14 = 105,
    F15 = 106,
    F16 = 107,
    F17 = 108,
    F18 = 109,
    F19 = 110,
    F20 = 111,
    F21 = 112,
    F22 = 113,
    F23 = 114,
    F24 = 115,
    Execute = 116,
    Help = 117,
    Menu = 118,
    Select = 119,
    Stop = 120,
    Again = 121,
    Undo = 122,
    Cut = 123,
    Copy = 124,
    Paste = 125,
    Find = 126,
    Mute = 127,
    Volumeup = 128,
    Volumedown = 129,
    KpComma = 133,
    KpEqualsas400 = 134,
    International1 = 135,
    International2 = 136,
    International3 = 137,
    International4 = 138,
    International5 = 139,
    International6 = 140,
    International7 = 141,
    International8 = 142,
    International9 = 143,
    Lang1 = 144,
    Lang2 = 145,
    Lang3 = 146,
    Lang4 = 147,
    Lang5 = 148,
    Lang6 = 149,
    Lang7 = 150,
    Lang8 = 151,
    Lang9 = 152,
    Alterase = 153,
    Sysreq = 154,
    Cancel = 155,
    Clear = 156,
    Prior = 157,
    Return2 = 158,
    Separator = 159,
    Out = 160,
    Oper = 161,
    Clearagain = 162,
    Crsel = 163,
    Exsel = 164,
    Kp00 = 176,
    Kp000 = 177,
    Thousandsseparator = 178,
    Decimalseparator = 179,
    Currencyunit = 180,
    Currencysubunit = 181,
    KpLeftparen = 182,
    KpRightparen = 183,
    KpLeftbrace = 184,
    KpRightbrace = 185,
    KpTab = 186,
    KpBackspace = 187,
    KpA = 188,
    KpB = 189,
    KpC = 190,
    KpD = 191,
    KpE = 192,
    KpF = 193,
    KpXor = 194,
    KpPower = 195,
    KpPercent = 196,
    KpLess = 197,
    KpGreater = 198,
    KpAmpersand = 199,
    KpDblampersand = 200,
    KpVerticalbar = 201,
    KpDblverticalbar = 202,
    KpColon = 203,
    KpHash = 204,
    KpSpace = 205,
    KpAt = 206,
    KpExclam = 207,
    KpMemstore = 208,
    KpMemrecall = 209,
    KpMemclear = 210,
    KpMemadd = 211,
    KpMemsubtract = 212,
    KpMemmultiply = 213,
    KpMemdivide = 214,
    KpPlusminus = 215,
    KpClear = 216,
    KpClearentry = 217,
    KpBinary = 218,
    KpOctal = 219,
    KpDecimal = 220,
    KpHexadecimal = 221,
    Lctrl = 224,
    Lshift = 225,
    Lalt = 226,
    Lgui = 227,
    Rctrl = 228,
    Rshift = 229,
    Ralt = 230,
    Rgui = 231,
    Mode = 257,
    Sleep = 258,
    Wake = 259,
    ChannelIncrement = 260,
    ChannelDecrement = 261,
    MediaPlay = 262,
    MediaPause = 263,
    MediaRecord = 264,
    MediaFastForward = 265,
    MediaRewind = 266,
    MediaNextTrack = 267,
    MediaPreviousTrack = 268,
    MediaStop = 269,
    MediaEject = 270,
    MediaPlayPause = 271,
    MediaSelect = 272,
    AcNew = 273,
    AcOpen = 274,
    AcClose = 275,
    AcExit = 276,
    AcSave = 277,
    AcPrint = 278,
    AcProperties = 279,
    AcSearch = 280,
    AcHome = 281,
    AcBack = 282,
    AcForward = 283,
    AcStop = 284,
    AcRefresh = 285,
    AcBookmarks = 286,
    Softleft = 287,
    Softright = 288,
    Call = 289,
    Endcall = 290,
    Reserved = 400,
    Count = 512,}

/// <summary>
/// Zero-copy input API over the native EngineInputState snapshot
/// (input_get_state borrows renderer memory, valid until the next pump).
/// Feed a snapshot via SetTestState for tests (live path: InputState* from
/// the ABI table entry); GetAxis resolves the load-once action map
/// (Horizontal default; D/Right - A/Left, else gamepad LeftX).
/// </summary>
public static unsafe class Input
{
    private static NativeApi.InputState* _state;
    private static Dictionary<string, AxisMap> _axes = new(StringComparer.Ordinal);

    static Input()
    {
        Reload(LoadDefaultActions());
    }

    /// <summary>Held (scancode index; out-of-range reads released).</summary>
    public static bool GetKey(Key key) => _state != null && (int)key >= 0 && (int)key < 512 && _state->Keys[(int)key] != 0;

    /// <summary>Went down this frame.</summary>
    public static bool GetKeyDown(Key key) => _state != null && (int)key >= 0 && (int)key < 512 && _state->Pressed[(int)key] != 0;

    /// <summary>Went up this frame.</summary>
    public static bool GetKeyUp(Key key) => _state != null && (int)key >= 0 && (int)key < 512 && _state->Released[(int)key] != 0;

    /// <summary>Last motion position (pixels).</summary>
    public static (float X, float Y) MousePosition => _state != null ? (_state->MouseX, _state->MouseY) : (0.0f, 0.0f);

    /// <summary>Accumulated motion this frame (pixels; clears on begin_frame).</summary>
    public static (float X, float Y) MouseDelta => _state != null ? (_state->MouseDx, _state->MouseDy) : (0.0f, 0.0f);

    /// <summary>SDL mouse button held (1=left, 2=middle, 3=right, 4=X1, 5=X2).</summary>
    public static bool GetMouseButton(int button) =>
        _state != null && button >= 1 && button <= 32 && (_state->MouseButtons & (1u << (button - 1))) != 0;

    /// <summary>Gamepad axis normalised to [-1, 1] (unknown pad reads 0).</summary>
    public static float GetGamepadAxis(int pad, int axis) =>
        _state != null && (uint)pad < 4 && (uint)axis < 6 ? _state->PadAxes[pad * 6 + axis] : 0.0f;

    /// <summary>Gamepad button held (SDL button ordinal; unknown pad released).</summary>
    public static bool GetGamepadButton(int pad, int button) =>
        _state != null && (uint)pad < 4 && (uint)button < 32 && (_state->PadButtons[pad] & (1u << button)) != 0;

    /// <summary>
    /// Action-map axis in [-1, 1]: +1 when any positive key held, -1 when any
    /// negative key held, 0 when both sides held, else the mapped gamepad
    /// axis (LeftX = pad 0 axis 0). Unknown axis name reads 0.
    /// </summary>
    public static float GetAxis(string name)
    {
        if (!_axes.TryGetValue(name, out var map))
        {
            return 0.0f;
        }

        var pos = AnyDown(map.Positive);
        var neg = AnyDown(map.Negative);
        if (pos && neg)
        {
            return 0.0f;
        }

        if (pos)
        {
            return 1.0f;
        }

        if (neg)
        {
            return -1.0f;
        }

        return GamepadAxisByName(map.GamepadAxis);
    }

    /// <summary>Bind the live native snapshot (P1-09 renderer_frame path).</summary>
    public static void SetState(NativeApi.InputState* state) => _state = state;

    /// <summary>Replace the action map (tests + input_actions.json load).</summary>
    public static void Reload(string json) => _axes = ParseAxes(json);

    /// <summary>Load action maps from input_actions.json next to the game assembly (once at startup; missing file keeps defaults).</summary>
    public static void LoadActionMaps(string gameAssemblyPath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(gameAssemblyPath));
        if (dir == null)
        {
            return;
        }

        var path = Path.Combine(dir, "input_actions.json");
        if (!File.Exists(path))
        {
            return;
        }

        Reload(File.ReadAllText(path));
    }

    private static bool AnyDown(Key[] keys)
    {
        foreach (var k in keys)
        {
            if (GetKey(k))
            {
                return true;
            }
        }

        return false;
    }

    private static float GamepadAxisByName(string name) => name switch
    {
        "LeftX" => GetGamepadAxis(0, 0),
        "LeftY" => GetGamepadAxis(0, 1),
        "RightX" => GetGamepadAxis(0, 2),
        "RightY" => GetGamepadAxis(0, 3),
        "LeftTrigger" => GetGamepadAxis(0, 4),
        "RightTrigger" => GetGamepadAxis(0, 5),
        _ => 0.0f,
    };

    private static string LoadDefaultActions() =>
        "{\"axes\":{\"Horizontal\":{\"negative\":[\"A\",\"Left\"],\"positive\":[\"D\",\"Right\"],\"gamepadAxis\":\"LeftX\"}}}";

    private sealed record AxisMap(Key[] Positive, Key[] Negative, string GamepadAxis);

    private static Dictionary<string, AxisMap> ParseAxes(string json)
    {
        var axes = new Dictionary<string, AxisMap>(StringComparer.Ordinal);
        using var doc = JsonDocument.Parse(json);
        if (!doc.RootElement.TryGetProperty("axes"u8, out var root))
        {
            return axes;
        }

        foreach (var prop in root.EnumerateObject())
        {
            var positive = ParseKeys(prop.Value, "positive");
            var negative = ParseKeys(prop.Value, "negative");
            var pad = prop.Value.TryGetProperty("gamepadAxis"u8, out var g) && g.ValueKind == JsonValueKind.String
                ? g.GetString() ?? string.Empty
                : string.Empty;
            axes[prop.Name] = new AxisMap(positive, negative, pad);
        }

        return axes;
    }

    private static Key[] ParseKeys(JsonElement axis, string prop)
    {
        if (!axis.TryGetProperty(prop, out var arr) || arr.ValueKind != JsonValueKind.Array)
        {
            return [];
        }

        var keys = new List<Key>();
        foreach (var el in arr.EnumerateArray())
        {
            if (el.ValueKind == JsonValueKind.String && Enum.TryParse<Key>(el.GetString(), out var key))
            {
                keys.Add(key);
            }
        }

        return [.. keys];
    }
}
