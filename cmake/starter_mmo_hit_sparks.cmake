# Offline check of samples/starter-mmo's hit sparks (scripts/combat.lua): a fresh copy of the sample (OUT/project)
# whose meadow.scene also runs starter_mmo_hit_driver.lua (TEST ONLY), played at a fixed 1/60 s step. The driver fakes
# the server's hit; the log must show the burst (24 sparks alive), its entity destroyed once none is left, and a second
# burst, screenshotted mid-flight.
# Usage: cmake -DEDITOR=<exe> -DSAMPLE=<dir> -DOUT=<dir> -DSCREENSHOT=<png> -P starter_mmo_hit_sparks.cmake
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
set(project "${OUT}/project")
file(REMOVE_RECURSE "${project}")
file(COPY "${SAMPLE}/" DESTINATION "${project}")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_hit_driver.lua" "${project}/scripts/test_hit.lua")
file(READ "${project}/scenes/meadow.scene" text)
kast_plain_asset_refs(text)
string(REPLACE "\"scripts/combat.lua\"" "\"scripts/combat.lua\", \"scripts/test_hit.lua\"" text "${text}")
file(WRITE "${project}/scenes/meadow.scene" "${text}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DSCREENSHOT=${SCREENSHOT}" -DFRAMES=600
                        "-DEXTRA_ARGS=--scene ${project}/scenes/meadow.scene --play --no-audio-device --fixed-dt 0.016666667 --screenshot-on-log 4,HITSHOT"
                        "-DEXPECT_OUTPUT=test hit: hit 1: sparks 24 alive.*test hit: hit 1: sparks destroyed.*test hit: hit 2: sparks 24 alive.*HITSHOT"
                        "-DFORBID_OUTPUT=[[]error|Lua error"
                        -P "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "hit sparks check failed")
endif()
