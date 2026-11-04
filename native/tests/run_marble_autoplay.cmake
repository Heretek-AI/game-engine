# run_marble_autoplay.cmake — exit-status + output gate for the MarbleRun
# acceptance test (P2-08, CodeRabbit round 2).
#
# PASS_REGULAR_EXPRESSION alone cannot fail a test whose binary exits
# nonzero (a crash AFTER printing "Finished in" would still pass), so this
# wrapper asserts BOTH: the player exits 0 AND its output contains
# "Finished in". Invoked as:
#   cmake -DP layer=<path> -DPROJECT_DIR=<dir> -DAUTOPLAY=<file> -DFRAMES=N \
#     -P run_marble_autoplay.cmake
# (All-caps -D names survive every generator; paths are passed in to keep
# this file free of build-tree assumptions. SDL offscreen comes from the
# test's ENVIRONMENT properties, unchanged.)
if(NOT DEFINED PLAYER OR NOT DEFINED PROJECT_DIR OR NOT DEFINED AUTOPLAY)
  message(FATAL_ERROR "run_marble_autoplay.cmake needs -DPLAYER, -DPROJECT_DIR, -DAUTOPLAY")
endif()
if(NOT DEFINED FRAMES)
  set(FRAMES 1500)
endif()
execute_process(
  COMMAND "${PLAYER}" "${PROJECT_DIR}" --frames ${FRAMES} --autoplay "${AUTOPLAY}" --hidden
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE rc
  TIMEOUT 280)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "engine-player exited ${rc} (expected 0).\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
if(NOT "${out}" MATCHES "Finished in")
  message(FATAL_ERROR "engine-player exited 0 but never printed 'Finished in'.\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
message(STATUS "marble autoplay OK (exit 0 + 'Finished in' present)")
