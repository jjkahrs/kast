# Runs a built game (a copy of GAME_DIR, in WORK_DIR) for 120 frames and checks its exit code is EXPECT (0 or 1).
# PAK=missing deletes game.pak first, PAK=corrupt overwrites it. SCREENSHOT also writes a PNG (must be non-empty).
# EXPECT_OUTPUT is a regex the game's log must match. EXTRA_ARGS are passed on to the game (e.g. --gui-click id). A hang
# fails after 60 s instead of blocking the run. WRAPPER (a list) runs the game through another program, e.g.
# kast_net_test_server <args> -- (its servers are up while the game runs).
# Usage: cmake -DGAME_DIR=<dir> -DEXE=<name.exe> -DWORK_DIR=<dir> -DEXPECT=0|1 [-DSCREENSHOT=<png>]
#              [-DPAK=missing|corrupt] [-DEXPECT_OUTPUT=<regex>] [-DEXTRA_ARGS=<args>] [-DWRAPPER=<exe;args>]
#              -P player_test.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${GAME_DIR}/" DESTINATION "${WORK_DIR}")
if(PAK STREQUAL "missing")
  file(REMOVE "${WORK_DIR}/game.pak")
elseif(PAK STREQUAL "corrupt")
  file(WRITE "${WORK_DIR}/game.pak" "this is not a Kast pak")
endif()
set(args --frames 120)
if(DEFINED EXTRA_ARGS)
  separate_arguments(EXTRA_ARGS)
  list(APPEND args ${EXTRA_ARGS})
endif()
if(DEFINED SCREENSHOT)
  file(REMOVE "${SCREENSHOT}")
  list(APPEND args --screenshot "${SCREENSHOT}")
endif()
# Only Windows on PATH: a DLL the Build forgot to copy fails the run instead of loading from vcpkg's bin folder.
set(ENV{PATH} "$ENV{SystemRoot}/System32;$ENV{SystemRoot}")
execute_process(COMMAND ${WRAPPER} "${WORK_DIR}/${EXE}" ${args} WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 60
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result STREQUAL EXPECT)
  message(FATAL_ERROR "${EXE} exited with '${result}', expected ${EXPECT}")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT output MATCHES "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "Output does not match '${EXPECT_OUTPUT}'")
endif()
if(DEFINED SCREENSHOT)
  if(NOT EXISTS "${SCREENSHOT}")
    message(FATAL_ERROR "Screenshot not written: ${SCREENSHOT}")
  endif()
  file(SIZE "${SCREENSHOT}" size)
  if(size EQUAL 0)
    message(FATAL_ERROR "Screenshot is empty: ${SCREENSHOT}")
  endif()
endif()
message(STATUS "OK: ${EXE} exited with ${EXPECT}")
