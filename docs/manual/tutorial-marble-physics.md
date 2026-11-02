# Tutorial 2: rolling marble (physics + input)

Roll a sphere along a ramp course with WASD or the left stick, reach the
goal trigger, and read `Finished in N s` in the log. Estimated time: 30
minutes. Builds on [Tutorial 1](tutorial-hello-cube.md) (world +
renderer + camera) and the [physics guide](physics.md).

> Source note: the full Marble Run sample game (`samples/MarbleRun`,
> P2-08) had not merged when this manual was written. This tutorial uses
> the same physics surface against the `HelloCube` scene shape, so every
> step runs today; it will be re-pointed at `samples/MarbleRun` once
> P2-08 lands.

## Step 1 — Describe the marble body

A dynamic body plus one sphere collider rides on the entity next to its
`LocalTransform`. The sync system creates the Jolt body, steps it at the
fixed 50 Hz lane, and writes the pose back:

```csharp
// Snippet: marble spawn descriptor (radius + drop height).
// Self-contained: the sync system reads radius/height off your component.
(float Radius, float DropY) MarbleSpawn() => (0.5f, 5.0f);
```

Drop from y=5 onto a static box floor (top at y=0): after 2 simulated
seconds at 50 Hz the marble rests at y = 0.5 ± 0.05. Spawn exactly
resting and no begin event fires — drop it so the first impact
registers.

## Step 2 — Steer with input

Sample the action map once per frame and apply torque. `GetAxis` already
merges keys and stick; add W as a digital boost like the scripting
guide:

```csharp
// Snippet: marble drive from the shared action map.
// Requires: Engine.Runtime (Input).
float MarbleDrive()
{
    return Engine.Runtime.Input.GetAxis("Horizontal");
}
```

Falling below y = −10 respawns at start (reset the `LocalTransform`
pose and zero the velocity through the sync system); reaching the goal
trigger prints `Finished in {N:F1} s` to the console (real UI text
arrives with P5-04).

## Step 3 — Read the goal with a raycast

Detect the goal zone with a downward raycast from above the trigger
instead of eye-balling the screenshot:

```csharp
// Snippet: goal-zone probe above the trigger plate.
// Requires: Engine.Runtime (Physics, Ray).
Engine.Runtime.Ray GoalProbe(float x, float y, float z) =>
    new Engine.Runtime.Ray(x, y, z, 0.0f, -1.0f, 0.0f);
```

A hit with `Fraction * rayLength ≈ y − plateY` means the marble is over
the plate. For replayable tests, drive the marble with a recorded input
file (`--autoplay inputs.json` replays per-frame key states): the
scripted run prints `Finished in` within 1500 frames and exits 0.
