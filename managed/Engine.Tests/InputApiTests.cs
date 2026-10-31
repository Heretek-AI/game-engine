// InputApiTests.cs — P2-02 acceptance: action-map axis resolution.
//
// Feeds a fake EngineInputState (stack-local zero-copy projection) into
// Input.SetState and checks GetAxis("Horizontal"): 1 with D held, -1 with
// A held, 0 with both, gamepad axis value when no key held. Plus Key =
// SDL scancode values, mouse position/delta, AbiLayout 1668B size.
using System.Runtime.CompilerServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class InputApiTests
{
    public static unsafe int Run()
    {
        var failures = 0;

        // --- Key enum values equal SDL scancodes --------------------------
        failures += Check((int)Key.A == 4, "Key.A == SDL scancode 4");
        failures += Check((int)Key.D == 7, "Key.D == SDL scancode 7");
        failures += Check((int)Key.W == 26, "Key.W == SDL scancode 26");
        failures += Check((int)Key.Space == 44, "Key.Space == SDL scancode 44");
        failures += Check((int)Key.Left == 80, "Key.Left == SDL scancode 80");
        failures += Check((int)Key.Right == 79, "Key.Right == SDL scancode 79");
        failures += Check((int)Key.Escape == 41, "Key.Escape == SDL scancode 41");

        // --- AbiLayout: sizeof(EngineInputState) matches native -----------
        failures += Check(sizeof(NativeApi.InputState) == 1668, "InputState 1668B (matches native)");

        // --- Action-map axis: D held -> 1 --------------------------------
        var state = default(NativeApi.InputState);
        state.Keys[(int)Key.D] = 1;
        state.Pressed[(int)Key.D] = 1;
        Input.SetState(&state);
        failures += Check(Input.GetKey(Key.D), "GetKey(D) held");
        failures += Check(Input.GetKeyDown(Key.D), "GetKeyDown(D) pressed edge");
        failures += Check(!Input.GetKey(Key.A), "GetKey(A) released");
        failures += Check(Input.GetAxis("Horizontal") == 1.0f, "GetAxis(Horizontal) == 1 with D held");

        // --- A held -> -1 -------------------------------------------------
        state.Keys[(int)Key.D] = 0;
        state.Pressed[(int)Key.D] = 0;
        state.Keys[(int)Key.A] = 1;
        failures += Check(Input.GetAxis("Horizontal") == -1.0f, "GetAxis(Horizontal) == -1 with A held");
        failures += Check(Input.GetKeyUp(Key.D) == false, "GetKeyUp(D) clear (no release fed)");

        // --- Both held -> 0 ------------------------------------------------
        state.Keys[(int)Key.D] = 1;
        failures += Check(Input.GetAxis("Horizontal") == 0.0f, "GetAxis(Horizontal) == 0 with both held");

        // --- No key held -> gamepad axis -----------------------------------
        state.Keys[(int)Key.D] = 0;
        state.Keys[(int)Key.A] = 0;
        state.PadAxes[0 * 6 + 0] = 0.5f;
        failures += Check(Input.GetAxis("Horizontal") == 0.5f, "GetAxis(Horizontal) == pad LeftX 0.5 with no key");
        state.PadAxes[0 * 6 + 0] = -0.25f;
        failures += Check(Input.GetAxis("Horizontal") == -0.25f, "GetAxis(Horizontal) == pad LeftX -0.25");
        failures += Check(Input.GetAxis("NoSuchAxis") == 0.0f, "GetAxis(unknown) == 0");

        // --- Mouse position/delta ------------------------------------------
        state.MouseX = 10.0f;
        state.MouseY = 20.0f;
        state.MouseDx = 3.0f;
        state.MouseDy = 0.0f;
        var pos = Input.MousePosition;
        var delta = Input.MouseDelta;
        failures += Check(pos.X == 10.0f && pos.Y == 20.0f, "MousePosition 10,20");
        failures += Check(delta.X == 3.0f && delta.Y == 0.0f, "MouseDelta 3,0");

        // --- Released edge ---------------------------------------------------
        state.Released[(int)Key.W] = 1;
        failures += Check(Input.GetKeyUp(Key.W), "GetKeyUp(W) released edge");

        // --- Defaults restored (load-once map is process state) -------------
        Input.Reload("{\"axes\":{\"Horizontal\":{\"negative\":[\"A\",\"Left\"],\"positive\":[\"D\",\"Right\"],\"gamepadAxis\":\"LeftX\"}}}");
        Input.SetState(null);

        Console.WriteLine(failures == 0 ? "InputApiTests: all checks passed" : $"InputApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}
