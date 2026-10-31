// navmesh.hpp — P6-03: Recast navmesh bake + Detour/DetourCrowd NavAgent update.
//
// Owns a dtNavMesh built from caller-supplied static colliders (axis-aligned
// box obstacles on a flat floor), a dtNavMeshQuery, and a dtCrowd with one
// agent slot per NavAgent. Backs the ABI 4.3 navigation tail.
//
// Frame path: no malloc/new, noexcept throughout. Bake allocates freely
// (init-time); update() only steps the crowd and writes positions.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

// Opaque Recast/Detour forward declarations (no Detour headers leak here).
class dtNavMesh;
class dtNavMeshQuery;
class dtCrowd;

namespace engine::navigation {

// One static obstacle: axis-aligned box (center + half extents).
struct NavBoxObstacle {
  float cx{0.0F};
  float cy{0.0F};
  float cz{0.0F};
  float hx{0.5F};
  float hy{0.5F};
  float hz{0.5F};
};

// Bake parameters: floor rectangle + rasterization cell size. Null floor
// means a fixed default (20x20 m centered on the origin, y=0). Obstacles
// outside the floor are ignored.
struct NavBakeConfig {
  float floor_min_x{-10.0F};
  float floor_min_y{0.0F};
  float floor_min_z{-10.0F};
  float floor_max_x{10.0F};
  float floor_max_y{2.0F};
  float floor_max_z{10.0F};
  float cell_size{0.3F};
  float cell_height{0.2F};
  float agent_height{2.0F};
  float agent_radius{0.6F};
  float agent_max_climb{0.9F};
  float agent_max_slope_deg{45.0F};
};

// Bake result: poly + triangle counts (queryable without Detour headers).
struct NavBakeStats {
  std::uint32_t polys{0};
  std::uint32_t detail_tris{0};
};

class ENGINE_CORE_API NavWorld {
 public:
  NavWorld() noexcept = default;
  NavWorld(const NavWorld&) = delete;
  auto operator=(const NavWorld&) -> NavWorld& = delete;
  ~NavWorld() noexcept;

  // Bake a navmesh from floor + box obstacles. Destroys any previous mesh
  // first (rebake-safe). Returns false on null/empty input or bake failure.
  auto bake(const NavBoxObstacle* obstacles, std::uint32_t obstacle_count,
            const NavBakeConfig& cfg) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return mesh_ != nullptr; }
  [[nodiscard]] auto stats() const noexcept -> NavBakeStats { return stats_; }

  // Single agent API (mirrors the verified P6-00 spike sequence):
  // add at `pos` (returns agent id, -1 on failure), request a move target
  // (nearest-poly lookup on both ends, crowd target on the END ref), step
  // the crowd by dt, read the agent position / arrival state.
  auto agent_add(float x, float y, float z, float max_speed) noexcept -> int;
  auto agent_goto(float x, float y, float z) noexcept -> bool;
  auto agent_update(float dt) noexcept -> bool;
  auto agent_pos(float* out_xyz) const noexcept -> bool;
  // 1 when the agent is within stop_radius of its goal AND has no pending
  // requests (crowd velocity ~ 0); 0 otherwise.
  auto agent_reached(float stop_radius) const noexcept -> bool;

 private:
  dtNavMesh* mesh_{nullptr};
  dtNavMeshQuery* query_{nullptr};
  dtCrowd* crowd_{nullptr};
  unsigned char* mesh_data_{nullptr};
  int agent_id_{-1};
  NavBakeStats stats_{};
  float goal_x_{0.0F};
  float goal_y_{0.0F};
  float goal_z_{0.0F};
  bool has_goal_{false};
};

}  // namespace engine::navigation
