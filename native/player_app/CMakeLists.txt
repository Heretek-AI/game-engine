# native/player_app — P2-07 engine-player executable.
#
# Builds the `engine-player` tool: reads <project_dir>/project.json,
# boots the game assembly through the existing dotnet_host (optional
# Game.Bootstrap.Main UCO), loads the start scene through the scene ABI
# tail, and runs the P2-03 frame loop until close or N frames.
#
# Gotcha 11: volk lives in libengine_core; this TU includes no volk
# headers and compiles no volk.c — Vulkan resolves inside the library.
add_executable(engine-player main.cpp)

target_link_libraries(engine-player PRIVATE engine_core)

# Autoplay replays input through synthetic SDL key events (SDL_PushEvent),
# so the player needs the same static SDL3 the input module uses. Without
# it the TU compiles to a stub (ENGINE_PLAYER_HAS_SDL3 undefined) whose
# autoplay injection is a no-op.
if(TARGET SDL3::SDL3-static)
  target_link_libraries(engine-player PRIVATE SDL3::SDL3-static)
  target_compile_definitions(engine-player PRIVATE ENGINE_PLAYER_HAS_SDL3=1)
endif()

engine_apply_base_flags(engine-player)

# Windows: engine_core.dll symbols reach the exe through engine_core.lib
# (ENGINE_CORE_API import lib), same as the native tests.
if(WIN32)
  target_link_libraries(engine-player PRIVATE flecs)
endif()
