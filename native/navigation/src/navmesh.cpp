// navmesh.cpp — P6-03: Recast bake -> Detour query -> DetourCrowd stepping.
//
// Implements the verified P6-00 call sequence (§4.12): heightfield ->
// compact -> regions -> contours -> poly mesh -> detail, copy into
// dtNavMeshCreateParams (every poly FLAG=1 so the default include filter
// matches — see gotcha §4.12.1), init dtNavMeshQuery + dtCrowd.
//
// WARNING (load-bearing include rule, same as physics TUs): this TU is the
// ONLY navigation TU that textually includes <Recast.h>/<Detour...>. It
// links the engine_navigation_recast_wrapper interface (SYSTEM +
// -Wno-conversion), so -Wconversion -Werror never fires inside upstream
// headers. Never add the Recast source dir as a plain include.
#include "engine/navigation/navmesh.hpp"

#include <cmath>
#include <cstring>

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

namespace engine::navigation {

namespace {

// Nonnegative half extents (a zero/negative entry collapses that axis).
inline auto NonnegHalf(float h) noexcept -> float {
  return h < 0.0F ? 0.0F : h;
}

// Rasterize one AABB obstacle into the heightfield span set.
auto RasterizeBox(rcContext& ctx, rcHeightfield& hf, const NavBoxObstacle& box,
                  const NavBakeConfig& cfg) noexcept -> bool {
  const float half_x = NonnegHalf(box.hx);
  const float half_y = NonnegHalf(box.hy);
  const float half_z = NonnegHalf(box.hz);
  // 8 corners of the box; Recast clips spans against the floor bounds.
  const float verts[8 * 3] = {
      box.cx - half_x, box.cy - half_y, box.cz - half_z,
      box.cx + half_x, box.cy - half_y, box.cz - half_z,
      box.cx + half_x, box.cy - half_y, box.cz + half_z,
      box.cx - half_x, box.cy - half_y, box.cz + half_z,
      box.cx - half_x, box.cy + half_y, box.cz - half_z,
      box.cx + half_x, box.cy + half_y, box.cz - half_z,
      box.cx + half_x, box.cy + half_y, box.cz + half_z,
      box.cx - half_x, box.cy + half_y, box.cz + half_z,
  };
  // 12 triangles (two per face).
  constexpr int kTris[12 * 3] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                                 0, 4, 5, 0, 5, 1, 1, 5, 6, 1, 6, 2,
                                 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
  constexpr int kTriCount = 12;
  // Walkable slope: Recast marks spans walkable when the triangle slope is
  // under the threshold; box sides are vertical (non-walkable), tops flat.
  constexpr unsigned char kArea = RC_WALKABLE_AREA;
  const int* tris = kTris;
  bool ok = true;
  for (int t = 0; t < kTriCount; ++t) {
    const float* v0 = &verts[tris[t * 3 + 0] * 3];
    const float* v1 = &verts[tris[t * 3 + 1] * 3];
    const float* v2 = &verts[tris[t * 3 + 2] * 3];
    // Vertical faces are never walkable: skip them so the box interior
    // stays blocked (the top face rasterizes as walkable, then the erosion
    // + climb filter drops unreachable plateaus).
    float nx = 0.0F;
    float ny = 0.0F;
    float nz = 0.0F;
    const float e0x = v1[0] - v0[0];
    const float e0y = v1[1] - v0[1];
    const float e0z = v1[2] - v0[2];
    const float e1x = v2[0] - v0[0];
    const float e1y = v2[1] - v0[1];
    const float e1z = v2[2] - v0[2];
    nx = e0y * e1z - e0z * e1y;
    ny = e0z * e1x - e0x * e1z;
    nz = e0x * e1y - e0y * e1x;
    const float len =
        std::sqrt(nx * nx + ny * ny + nz * nz);  // NOLINT: init-time bake
    if (len <= 0.0F) {
      continue;
    }
    // Slope vs the walkable-slope threshold: flat tops get rasterized, walls
    // skip. The threshold honors cfg.agent_max_slope_deg (ABI: 45° default
    // when <= 0): a face is walkable when its up-normal >= cos(slope).
    const float slope_dot = ny / len;
    const float slope_deg =
        cfg.agent_max_slope_deg <= 0.0F ? 45.0F : cfg.agent_max_slope_deg;
    const float min_dot = std::cos(slope_deg * 3.14159265F / 180.0F);
    if (slope_dot < min_dot) {
      continue;
    }
    const float tv[3] = {v0[0], v0[1], v0[2]};
    const float tv1[3] = {v1[0], v1[1], v1[2]};
    const float tv2[3] = {v2[0], v2[1], v2[2]};
    const float tri[9] = {tv[0],  tv[1],  tv[2], tv1[0], tv1[1],
                          tv1[2], tv2[0], tv2[1], tv2[2]};
    if (!rcRasterizeTriangles(&ctx, tri, &kArea, 1, hf, 1)) {
      ok = false;
    }
  }
  return ok;
}

}  // namespace

NavWorld::~NavWorld() noexcept { destroy(); }

auto NavWorld::destroy() noexcept -> void {
  if (crowd_ != nullptr) {
    dtFreeCrowd(crowd_);
    crowd_ = nullptr;
  }
  if (query_ != nullptr) {
    dtFreeNavMeshQuery(query_);
    query_ = nullptr;
  }
  if (mesh_ != nullptr) {
    dtFreeNavMesh(mesh_);
    mesh_ = nullptr;
  }
  if (mesh_data_ != nullptr) {
    dtFree(mesh_data_);
    mesh_data_ = nullptr;
  }
  agent_id_ = -1;
  stats_ = {};
  agent_radius_ = 0.6F;
  agent_height_ = 2.0F;
  has_goal_ = false;
}

auto NavWorld::bake(const NavBoxObstacle* obstacles,
                    std::uint32_t obstacle_count,
                    const NavBakeConfig& cfg) noexcept -> bool {
  destroy();
  if (obstacles == nullptr && obstacle_count != 0) {
    return false;
  }

  rcContext ctx(false);
  const float bmin[3] = {cfg.floor_min_x, cfg.floor_min_y, cfg.floor_min_z};
  const float bmax[3] = {cfg.floor_max_x, cfg.floor_max_y, cfg.floor_max_z};
  const float width = bmax[0] - bmin[0];
  const float depth = bmax[2] - bmin[2];
  if (!(width > 0.0F) || !(depth > 0.0F)) {
    return false;
  }
  const float cs = cfg.cell_size > 0.0F ? cfg.cell_size : 0.3F;
  const float ch = cfg.cell_height > 0.0F ? cfg.cell_height : 0.2F;
  const int grid_w = static_cast<int>(width / cs + 0.5F);
  const int grid_h = static_cast<int>(depth / cs + 0.5F);
  if (grid_w <= 0 || grid_h <= 0) {
    return false;
  }

  rcHeightfield* hf = rcAllocHeightfield();
  if (hf == nullptr) {
    return false;
  }
  bool ok = rcCreateHeightfield(&ctx, *hf, grid_w, grid_h, bmin, bmax, cs, ch) != 0;
  if (ok) {
    // Floor: one walkable quad at y=bmin[1].
    const float floor_y = bmin[1];
    const float fverts[4 * 3] = {
        bmin[0], floor_y, bmin[2], bmax[0], floor_y, bmin[2],
        bmax[0], floor_y, bmax[2], bmin[0], floor_y, bmax[2],
    };
    constexpr int kFloorTris[2 * 3] = {0, 1, 2, 0, 2, 3};
    const unsigned char area = RC_WALKABLE_AREA;
    for (int t = 0; t < 2 && ok; ++t) {
      const float* v0 = &fverts[kFloorTris[t * 3 + 0] * 3];
      const float* v1 = &fverts[kFloorTris[t * 3 + 1] * 3];
      const float* v2 = &fverts[kFloorTris[t * 3 + 2] * 3];
      const float tri[9] = {v0[0], v0[1], v0[2], v1[0], v1[1],
                            v1[2], v2[0], v2[1], v2[2]};
      if (rcRasterizeTriangles(&ctx, tri, &area, 1, *hf, 1) == 0) {
        ok = false;
      }
    }
    // Obstacles: mark the box tops walkable, then erase the blocked area.
    // Simpler model: stamp each box footprint as RC_NULL_AREA so the
    // compact field drops those cells (verified equivalent in the spike:
    // agent routes around the blocked cells).
    for (std::uint32_t i = 0; i < obstacle_count && ok; ++i) {
      const NavBoxObstacle& box = obstacles[i];
      if (!RasterizeBox(ctx, *hf, box, cfg)) {
        ok = false;
      }
      // Footprint erase: clear walkability for cells whose center falls
      // inside the box XZ footprint AND whose spans overlap the box's
      // vertical extent (a raised beam must not delete the floor below it).
      const float half_x = NonnegHalf(box.hx);
      const float half_y = NonnegHalf(box.hy);
      const float half_z = NonnegHalf(box.hz);
      const float foot_min_x = box.cx - half_x;
      const float foot_max_x = box.cx + half_x;
      const float foot_min_z = box.cz - half_z;
      const float foot_max_z = box.cz + half_z;
      // Box bottom/top in heightfield voxel units (relative to hf bmin).
      const float box_bot = box.cy - half_y;
      const float box_top = box.cy + half_y;
      const int box_smin = static_cast<int>(
          std::floor((box_bot - hf->bmin[1]) / hf->ch));
      const int box_smax = static_cast<int>(
          std::ceil((box_top - hf->bmin[1]) / hf->ch));
      for (int ix = 0; ix < grid_w; ++ix) {
        for (int iz = 0; iz < grid_h; ++iz) {
          const float wx = bmin[0] + (static_cast<float>(ix) + 0.5F) * cs;
          const float wz = bmin[2] + (static_cast<float>(iz) + 0.5F) * cs;
          if (wx < foot_min_x || wx > foot_max_x || wz < foot_min_z ||
              wz > foot_max_z) {
            continue;
          }
          rcSpan* span = hf->spans[ix + iz * grid_w];
          while (span != nullptr) {
            // Skip spans fully below the box (with a +1 voxel tolerance so
            // floor-contact boxes, e.g. cy - hy == floor_y, still block the
            // floor span) or starting above the box top.
            const int smax = static_cast<int>(span->smax);
            const int smin = static_cast<int>(span->smin);
            if (smax + 1 <= box_smin || smin > box_smax) {
              span = span->next;
              continue;
            }
            span->area = RC_NULL_AREA;
            span = span->next;
          }
        }
      }
    }
  }

  rcCompactHeightfield* compact = nullptr;
  rcContourSet* contours = nullptr;
  rcPolyMesh* pmesh = nullptr;
  rcPolyMeshDetail* dmesh = nullptr;
  if (ok) {
    const int walkable_height =
        static_cast<int>(std::ceil(cfg.agent_height / ch));
    const int walkable_climb =
        static_cast<int>(std::floor(cfg.agent_max_climb / ch));
    const int walkable_radius =
        static_cast<int>(std::ceil(cfg.agent_radius / cs));
    rcFilterLowHangingWalkableObstacles(&ctx, walkable_climb, *hf);
    rcFilterLedgeSpans(&ctx, walkable_height, walkable_climb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, walkable_height, *hf);
    compact = rcAllocCompactHeightfield();
    ok = compact != nullptr &&
         rcBuildCompactHeightfield(&ctx, walkable_height, walkable_climb, *hf,
                                   *compact) != 0 &&
         rcErodeWalkableArea(&ctx, walkable_radius, *compact) != 0 &&
         rcBuildDistanceField(&ctx, *compact) != 0 &&
         rcBuildRegions(&ctx, *compact, 0, 8, 4) != 0;
  }
  if (ok) {
    contours = rcAllocContourSet();
    ok = contours != nullptr &&
         rcBuildContours(&ctx, *compact, 1.3F, 12, *contours) != 0;
  }
  if (ok) {
    pmesh = rcAllocPolyMesh();
    ok = pmesh != nullptr &&
         rcBuildPolyMesh(&ctx, *contours, 6, *pmesh) != 0;
  }
  if (ok) {
    dmesh = rcAllocPolyMeshDetail();
    ok = dmesh != nullptr &&
         rcBuildPolyMeshDetail(&ctx, *pmesh, *compact, 6.0F, 1.0F,
                               *dmesh) != 0;
  }

  unsigned char* nav_data = nullptr;
  int nav_data_size = 0;
  if (ok) {
    if (pmesh->npolys == 0) {
      ok = false;
    } else {
      // Gotcha §4.12.1: tag every poly walkable (flags=1) so the default
      // include filter (0xffff) matches in findNearestPoly.
      for (int i = 0; i < pmesh->npolys; ++i) {
        pmesh->flags[i] = 1;
      }
      dtNavMeshCreateParams params{};
      std::memset(&params, 0, sizeof(params));
      params.verts = pmesh->verts;
      params.vertCount = pmesh->nverts;
      params.polys = pmesh->polys;
      params.polyAreas = pmesh->areas;
      params.polyFlags = pmesh->flags;
      params.polyCount = pmesh->npolys;
      params.nvp = pmesh->nvp;
      params.detailMeshes = dmesh->meshes;
      params.detailVerts = dmesh->verts;
      params.detailVertsCount = dmesh->nverts;
      params.detailTris = dmesh->tris;
      params.detailTriCount = dmesh->ntris;
      params.walkableHeight = cfg.agent_height;
      params.walkableRadius = cfg.agent_radius;
      params.walkableClimb = cfg.agent_max_climb;
      params.bmin[0] = pmesh->bmin[0];
      params.bmin[1] = pmesh->bmin[1];
      params.bmin[2] = pmesh->bmin[2];
      params.bmax[0] = pmesh->bmax[0];
      params.bmax[1] = pmesh->bmax[1];
      params.bmax[2] = pmesh->bmax[2];
      params.cs = pmesh->cs;
      params.ch = pmesh->ch;
      params.buildBvTree = true;
      ok = dtCreateNavMeshData(&params, &nav_data, &nav_data_size) != 0 &&
           nav_data != nullptr && nav_data_size > 0;
      stats_.polys = static_cast<std::uint32_t>(pmesh->npolys);
      stats_.detail_tris = static_cast<std::uint32_t>(dmesh->ntris);
    }
  }

  dtNavMesh* mesh = nullptr;
  dtNavMeshQuery* query = nullptr;
  dtCrowd* crowd = nullptr;
  if (ok) {
    mesh = dtAllocNavMesh();
    ok = mesh != nullptr &&
         dtStatusSucceed(mesh->init(nav_data, nav_data_size,
                                    DT_TILE_FREE_DATA));
    if (ok) {
      nav_data = nullptr;  // mesh owns it now (DT_TILE_FREE_DATA).
    }
  }
  if (ok) {
    query = dtAllocNavMeshQuery();
    ok = query != nullptr && dtStatusSucceed(query->init(mesh, 2048));
  }
  if (ok) {
    crowd = dtAllocCrowd();
    const float eff_radius = cfg.agent_radius <= 0.0F ? 0.6F : cfg.agent_radius;
    const float eff_height = cfg.agent_height <= 0.0F ? 2.0F : cfg.agent_height;
    ok = crowd != nullptr && crowd->init(8, eff_radius, mesh);
    if (ok) {
      crowd->getEditableFilter(0)->setIncludeFlags(0xffff);
      crowd->getEditableFilter(0)->setExcludeFlags(0);
      agent_radius_ = eff_radius;
      agent_height_ = eff_height;
    }
  }

  rcFreeHeightField(hf);
  if (compact != nullptr) {
    rcFreeCompactHeightfield(compact);
  }
  if (contours != nullptr) {
    rcFreeContourSet(contours);
  }
  if (pmesh != nullptr) {
    rcFreePolyMesh(pmesh);
  }
  if (dmesh != nullptr) {
    rcFreePolyMeshDetail(dmesh);
  }
  if (!ok) {
    if (crowd != nullptr) {
      dtFreeCrowd(crowd);
    }
    if (query != nullptr) {
      dtFreeNavMeshQuery(query);
    }
    if (mesh != nullptr) {
      dtFreeNavMesh(mesh);
    }
    if (nav_data != nullptr) {
      dtFree(nav_data);
    }
    stats_ = {};
    return false;
  }

  mesh_ = mesh;
  query_ = query;
  crowd_ = crowd;
  mesh_data_ = nullptr;  // owned by the mesh via DT_TILE_FREE_DATA.
  return true;
}

auto NavWorld::agent_add(float x, float y, float z, float max_speed) noexcept
    -> int {
  if (crowd_ == nullptr) {
    return -1;
  }
  if (agent_id_ >= 0) {
    crowd_->removeAgent(agent_id_);
    agent_id_ = -1;
  }
  dtCrowdAgentParams params{};
  std::memset(&params, 0, sizeof(params));
  // Match the bake: erosion radius + crowd maxAgentRadius from bake().
  params.radius = agent_radius_;
  params.height = agent_height_;
  params.maxAcceleration = 8.0F;
  params.maxSpeed = max_speed > 0.0F ? max_speed : 3.5F;
  params.collisionQueryRange = params.radius * 12.0F;
  params.pathOptimizationRange = params.radius * 30.0F;
  params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS |
                       DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_SEPARATION;
  params.obstacleAvoidanceType = 3;
  params.separationWeight = 0.0F;  // single agent: no separation needed.
  const float pos[3] = {x, y, z};
  const int id = crowd_->addAgent(pos, &params);
  if (id < 0) {
    return -1;
  }
  agent_id_ = id;
  has_goal_ = false;
  return id;
}

auto NavWorld::agent_goto(float x, float y, float z) noexcept -> bool {
  if (crowd_ == nullptr || query_ == nullptr || agent_id_ < 0) {
    return false;
  }
  const dtCrowdAgent* agent = crowd_->getAgent(agent_id_);
  if (agent == nullptr || !agent->active) {
    return false;
  }
  // Nearest-poly on both ends, then requestMoveTarget with the END ref
  // (ref 0 silently parks the agent — gotcha §4.12.3).
  const float extents[3] = {2.0F, 4.0F, 2.0F};
  dtQueryFilter filter;
  filter.setIncludeFlags(0xffff);
  filter.setExcludeFlags(0);
  dtPolyRef start_ref = 0;
  float start_pos[3] = {0.0F, 0.0F, 0.0F};
  dtPolyRef end_ref = 0;
  float end_pos[3] = {0.0F, 0.0F, 0.0F};
  const float spos[3] = {agent->npos[0], agent->npos[1], agent->npos[2]};
  const float epos[3] = {x, y, z};
  if (dtStatusFailed(
          query_->findNearestPoly(spos, extents, &filter, &start_ref, start_pos)) ||
      start_ref == 0) {
    return false;
  }
  if (dtStatusFailed(
          query_->findNearestPoly(epos, extents, &filter, &end_ref, end_pos)) ||
      end_ref == 0) {
    return false;
  }
  if (!crowd_->requestMoveTarget(agent_id_, end_ref, end_pos)) {
    return false;
  }
  goal_x_ = end_pos[0];
  goal_y_ = end_pos[1];
  goal_z_ = end_pos[2];
  has_goal_ = true;
  return true;
}

auto NavWorld::agent_update(float dt) noexcept -> bool {
  if (crowd_ == nullptr || agent_id_ < 0) {
    return false;
  }
  if (!(dt > 0.0F)) {
    return false;
  }
  crowd_->update(dt, nullptr);
  return true;
}

auto NavWorld::agent_pos(float* out_xyz) const noexcept -> bool {
  if (crowd_ == nullptr || agent_id_ < 0 || out_xyz == nullptr) {
    return false;
  }
  const dtCrowdAgent* agent = crowd_->getAgent(agent_id_);
  if (agent == nullptr || !agent->active) {
    return false;
  }
  out_xyz[0] = agent->npos[0];
  out_xyz[1] = agent->npos[1];
  out_xyz[2] = agent->npos[2];
  return true;
}

auto NavWorld::agent_reached(float stop_radius) const noexcept -> bool {
  if (crowd_ == nullptr || agent_id_ < 0 || !has_goal_) {
    return false;
  }
  const dtCrowdAgent* agent = crowd_->getAgent(agent_id_);
  if (agent == nullptr || !agent->active) {
    return false;
  }
  const float dx = agent->npos[0] - goal_x_;
  const float dy = agent->npos[1] - goal_y_;
  const float dz = agent->npos[2] - goal_z_;
  const float dist_sq = dx * dx + dy * dy + dz * dz;
  const float stop = stop_radius > 0.0F ? stop_radius : 0.5F;
  if (dist_sq > stop * stop) {
    return false;
  }
  // No pending requests and near-zero velocity => arrived.
  const float speed_sq = agent->vel[0] * agent->vel[0] +
                         agent->vel[1] * agent->vel[1] +
                         agent->vel[2] * agent->vel[2];
  return speed_sq < 0.04F;
}

}  // namespace engine::navigation
