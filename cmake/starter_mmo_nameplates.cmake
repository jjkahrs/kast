# Offline checks of samples/starter-mmo's nameplates, damage numbers and notice board (scripts/nameplates.lua,
# scripts/notice_board.lua): a fresh copy of the sample (OUT/project) whose meadow.scene also runs
# starter_mmo_nameplates_driver.lua (TEST ONLY: it plays the world client's part on the bus), with CA as its CA file (the
# real one isn't committed; without one Online Services, and so replication, stay off).
# Editor (default): plays the copy's meadow.scene in the editor at a fixed 1/60 s step for FRAMES frames (an upper
# bound with --screenshot-on-log in EDITOR_ARGS, e.g. --sim-mouse too) and writes SCREENSHOT; every EXPECT_ALL regex
# ("&"-free, a CMake list) must match the log.
# PERF=ON: the driver's 100-Guard grid instead; the copy is built (startScene meadow.scene) and the built player runs
# without vsync, at its own pace, writing SCREENSHOT; the logged average frame time must be at most MAX_MS (unset: only
# logged).
# Usage: cmake -DEDITOR=<exe> -DSAMPLE=<dir> -DCA=<pem> -DOUT=<dir> -DSCREENSHOT=<png> [-DFRAMES=<n>] [-DEDITOR_ARGS=<flags>]
#              [-DEXPECT_ALL=<regex;...>] [-DPERF=ON [-DMAX_MS=<ms>]] -P starter_mmo_nameplates.cmake
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
set(project "${OUT}/project")
file(REMOVE_RECURSE "${project}")
file(COPY "${SAMPLE}/" DESTINATION "${project}")
file(COPY_FILE "${CA}" "${project}/certs/world_dev_ca.pem")
if(PERF)
  set(PERF true)
else()
  set(PERF false)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/starter_mmo_nameplates_driver.lua" "${project}/scripts/test_nameplates.lua" @ONLY)
file(READ "${project}/scenes/meadow.scene" text)
kast_plain_asset_refs(text)
string(REPLACE "\"scripts/nameplates.lua\"" "\"scripts/nameplates.lua\", \"scripts/test_nameplates.lua\"" text "${text}")
file(WRITE "${project}/scenes/meadow.scene" "${text}")
set(forbid "\\[error\\]|Lua error|\\[warn\\] RmlUi|World UI document")

if(NOT PERF STREQUAL "true")
  if(NOT DEFINED FRAMES)
    set(FRAMES 200)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DSCREENSHOT=${SCREENSHOT}" "-DFRAMES=${FRAMES}"
                          "-DEXTRA_ARGS=--scene ${project}/scenes/meadow.scene --play --no-audio-device --size 1600x1000 --fixed-dt 0.016666667 ${EDITOR_ARGS}"
                          -DFOCUS_PANEL=Game "-DOUTPUT_FILE=${OUT}/editor.log" "-DFORBID_OUTPUT=${forbid}"
                          -P "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake"
                  RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "the editor run failed (see above)")
  endif()
  file(READ "${OUT}/editor.log" output)
  foreach(expected IN LISTS EXPECT_ALL)
    if(NOT output MATCHES "${expected}")
      message(FATAL_ERROR "the log does not match '${expected}'")
    endif()
  endforeach()
  message(STATUS "OK: ${SCREENSHOT}")
  return()
endif()

file(READ "${project}/project.kast" text)
string(REGEX REPLACE "\"startScene\": [^\n]*\n" "\"startScene\": \"scenes/meadow.scene\",\n" text "${text}")
file(WRITE "${project}/project.kast" "${text}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DPROJECT=${project}" "-DOUT=${OUT}/game"
                        "-DEXE=Starter MMO.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_LIST_DIR}/build_test.cmake"
                RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "building the perf copy failed (run build_test.cmake on ${project} to see why)")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${OUT}/game" "-DEXE=Starter MMO.exe" "-DWORK_DIR=${OUT}/game.run"
                        -DEXPECT=0 "-DSCREENSHOT=${SCREENSHOT}" "-DEXTRA_ARGS=--frames 760 --size 1600x900 --no-vsync --no-audio-device"
                        "-DEXPECT_OUTPUT=test nameplates: perf: 100 Guards with nameplates" "-DFORBID_OUTPUT=${forbid}"
                        -P "${CMAKE_CURRENT_LIST_DIR}/player_test.cmake"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the player run failed (see above)")
endif()
if(NOT output MATCHES "perf: average frame time ([0-9.]+) ms over ([0-9]+) frames")
  message(FATAL_ERROR "no frame time logged")
endif()
set(ms "${CMAKE_MATCH_1}")
message(STATUS "100 nameplates: ${ms} ms average over ${CMAKE_MATCH_2} frames")
if(DEFINED MAX_MS AND ms GREATER MAX_MS)
  message(FATAL_ERROR "${ms} ms is over the ${MAX_MS} ms budget")
endif()
