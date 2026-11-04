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
using Engine.Bindings;
using Engine.Runtime;

namespace MarbleRun;

/// <summary>Shared course visuals: one unit-cube mesh, tinted per part.</summary>
internal static unsafe class MarbleVisuals
{
    /// <summary>
    /// Adds a visual-only entity: scaled/translated unit cube with a flat
    /// base color. Used for slabs, marble (unit scale rides the physics
    /// pose) and the goal box — one implementation, no duplication.
    /// </summary>
    public static void AddVisual(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, uint cubeId,
        float px, float py, float pz, float sx, float sy, float sz, float r, float g, float b)
    {
        if (table == null || world == null || cubeId == 0)
        {
            return;
        }

        var e = Ecs.CreateEntity(table, world);
        var lt = LocalTransform.IdentityAt(px, py, pz);
        lt.Sx = sx;
        lt.Sy = sy;
        lt.Sz = sz;
        Ecs.Set(table, world, e, lt);
        var mr = new MeshRenderer { MeshId = cubeId };
        mr.BaseColor[0] = r;
        mr.BaseColor[1] = g;
        mr.BaseColor[2] = b;
        mr.BaseColor[3] = 1.0f;
        Ecs.Set(table, world, e, mr);
    }

    /// <summary>Tints an existing (physics) entity's unit-scale mesh, e.g. the marble.</summary>
    public static void Tint(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity entity, uint cubeId,
        float r, float g, float b)
    {
        if (table == null || world == null || cubeId == 0 || !entity.IsValid)
        {
            return;
        }

        var mr = new MeshRenderer { MeshId = cubeId };
        mr.BaseColor[0] = r;
        mr.BaseColor[1] = g;
        mr.BaseColor[2] = b;
        mr.BaseColor[3] = 1.0f;
        Ecs.Set(table, world, entity, mr);
    }
}

/// <summary>Shared unit-cube mesh data (24 verts, pos+normal+uv, 36 indices).</summary>
internal static class UnitCube
{
    public static (float[] Verts, uint[] Idx) Build()
    {
        var faces = new float[][][]
        {
            new[] { new[] { 1f, 0f, 0f }, new[] { 1f, -1f, -1f }, new[] { 1f, 1f, -1f }, new[] { 1f, 1f, 1f }, new[] { 1f, -1f, 1f } },
            new[] { new[] { -1f, 0f, 0f }, new[] { -1f, -1f, 1f }, new[] { -1f, 1f, 1f }, new[] { -1f, 1f, -1f }, new[] { -1f, -1f, -1f } },
            new[] { new[] { 0f, 1f, 0f }, new[] { -1f, 1f, -1f }, new[] { -1f, 1f, 1f }, new[] { 1f, 1f, 1f }, new[] { 1f, 1f, -1f } },
            new[] { new[] { 0f, -1f, 0f }, new[] { -1f, -1f, 1f }, new[] { -1f, -1f, -1f }, new[] { 1f, -1f, -1f }, new[] { 1f, -1f, 1f } },
            new[] { new[] { 0f, 0f, 1f }, new[] { -1f, -1f, 1f }, new[] { 1f, -1f, 1f }, new[] { 1f, 1f, 1f }, new[] { -1f, 1f, 1f } },
            new[] { new[] { 0f, 0f, -1f }, new[] { 1f, -1f, -1f }, new[] { -1f, -1f, -1f }, new[] { -1f, 1f, -1f }, new[] { 1f, 1f, -1f } },
        };

        var verts = new float[24 * 8];
        var idx = new uint[36];
        for (var f = 0; f < 6; f++)
        {
            for (var v = 0; v < 4; v++)
            {
                var o = (f * 4 + v) * 8;
                verts[o + 0] = faces[f][1 + v][0] * 0.5f;
                verts[o + 1] = faces[f][1 + v][1] * 0.5f;
                verts[o + 2] = faces[f][1 + v][2] * 0.5f;
                verts[o + 3] = faces[f][0][0];
                verts[o + 4] = faces[f][0][1];
                verts[o + 5] = faces[f][0][2];
                verts[o + 6] = (v == 0 || v == 3) ? 0.0f : 1.0f;
                verts[o + 7] = v < 2 ? 0.0f : 1.0f;
            }

            uint b = (uint)f * 4;
            uint[] quad = [b, b + 1, b + 2, b, b + 2, b + 3];
            for (var k = 0; k < 6; k++)
            {
                idx[f * 6 + k] = quad[k];
            }
        }

        return (verts, idx);
    }
}

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
