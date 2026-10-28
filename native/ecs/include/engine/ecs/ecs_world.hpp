// ecs_world.hpp — M1-03 owner. Minimal placeholder so #include paths and
// the engine::ecs namespace exist from M1-01; the Flecs world wrapper,
// LocalTransform/WorldTransform, and hierarchy pipeline land in M1-03.
#pragma once

namespace engine::ecs {

// Forward declaration only: keeps downstream TUs compiling while the
// implementation is stubbed. M1-03 replaces this with the real wrapper.
class EcsWorld;

}  // namespace engine::ecs
