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
# (ENGINE_CORE_API import lib), same as the native tests. Stage the
# runtime DLLs next to the exe (POST_BUILD copy, same pattern as the
# native tests): without engine_core.dll/flecs.dll beside it the exe
# dies at startup with 0xc0000135 (STATUS_DLL_NOT_FOUND).
if(WIN32)
  target_link_libraries(engine-player PRIVATE flecs)
  # Explicit DLL list (NOT the $<TARGET_RUNTIME_DLLS> genex: that
  # expands to the exe's OWN direct shared deps only, which misses
  # engine_core.dll when it arrives transitively). engine_core + flecs
  # are the only shared libs in this link closure (SDL3/Jolt/Box2D/
  # miniaudio are all static).
  add_custom_command(TARGET engine-player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:engine_core>
      $<TARGET_FILE:flecs>
      $<TARGET_FILE_DIR:engine-player>
    COMMAND_EXPAND_LISTS)
endif()
