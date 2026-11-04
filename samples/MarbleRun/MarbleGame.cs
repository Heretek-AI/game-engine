// MarbleGame.cs — course layout + scripted drive for the MarbleRun sample.
//
// The course is flat-topped (top at y = 0) and built only from static box
// colliders, plus one kinematic sphere (the marble) and one static sensor
// box (the goal trigger). No new assets or dependencies.
//
// Layout (x = run direction, z = lateral; boxes centred at y = -0.5 with
// half-height 0.5 so their tops sit exactly at y = 0):
//
//   A pad    x in [-3, 3],   z in [-3, 3]      (start)
//   B lane   x in [3, 9],    z in [-3, 3]
//   GAP      x in [9, 13] for z > -0.75       (fall through -> respawn)
//   bridge   x in [9, 13],   z in [-3.25, -0.75] (narrow plank over the gap)
//   C lane   x in [13, 19],  z in [-3, 3]
//   goal pad x in [19, 25],  z in [-3, 3]
//   goal sensor box centre (22, 1, 0), half (1.5, 1.5, 1.5)
namespace MarbleRun;

/// <summary>Axis-aligned course slab (top face at y = 0).</summary>
public readonly record struct CourseBox(float X0, float X1, float Z0, float Z1)
{
    /// <summary>Centre X of the slab.</summary>
    public readonly float Cx => (X0 + X1) * 0.5f;

    /// <summary>Centre Z of the slab.</summary>
    public readonly float Cz => (Z0 + Z1) * 0.5f;

    /// <summary>Full X extent (visual cube scale).</summary>
    public readonly float ExtentX => X1 - X0;

    /// <summary>Full Z extent (visual cube scale).</summary>
    public readonly float ExtentZ => Z1 - Z0;
}

/// <summary>Static course data shared by the host loop and the controller.</summary>
public static class MarbleGame
{
    /// <summary>Marble rest height: sphere centre above a slab top.</summary>
    public const float RestY = 0.5f;

    /// <summary>Fall below this height and the marble respawns at start.</summary>
    public const float RespawnY = -10.0f;

    /// <summary>Marble spawn (drops a short way onto the start pad).</summary>
    public static LocalTransform StartPose() => LocalTransform.IdentityAt(0.0f, 1.0f, 0.0f);

    /// <summary>Course slabs (visual + physics share these bounds).</summary>
    public static readonly CourseBox[] Boxes =
    [
        new CourseBox(-3.0f, 3.0f, -3.0f, 3.0f),
        new CourseBox(3.0f, 9.0f, -3.0f, 3.0f),
        new CourseBox(9.0f, 13.0f, -3.25f, -0.75f),
        new CourseBox(13.0f, 19.0f, -3.0f, 3.0f),
        new CourseBox(19.0f, 25.0f, -3.0f, 3.0f),
    ];

    /// <summary>Goal sensor centre.</summary>
    public const float GoalX = 22.0f;

    /// <summary>Goal sensor centre height.</summary>
    public const float GoalY = 1.0f;

    /// <summary>Goal sensor half extents (cube).</summary>
    public const float GoalHalf = 1.5f;

    /// <summary>True when (x, z) is over a slab (marble is supported).</summary>
    public static bool IsGround(float x, float z)
    {
        foreach (var b in Boxes)
        {
            if (x >= b.X0 && x <= b.X1 && z >= b.Z0 && z <= b.Z1)
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// Scripted demo drive: run +X, veer onto the bridge around the gap,
    /// then back to centre for the goal straight. Output need not be
    /// normalised; the controller clamps over-unit vectors.
    /// </summary>
    public static void DriveFor(float x, float z, out float dx, out float dz)
    {
        var targetZ = (x > 5.5f && x < 14.5f) ? -2.0f : 0.0f;
        dx = 1.0f;
        dz = (targetZ - z) * 1.5f;
        if (dz > 1.0f)
        {
            dz = 1.0f;
        }
        else if (dz < -1.0f)
        {
            dz = -1.0f;
        }
    }
}
