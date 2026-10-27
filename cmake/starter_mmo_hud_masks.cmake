# Offline check of samples/starter-mmo's HUD masks (ui/hud.rml, ui/chat.rml, ui/starter.rcss): a fresh copy of the
# sample (OUT/project) whose meadow.scene also runs starter_mmo_hud_masks_driver.lua (TEST ONLY), played at a fixed
# 1/60 s step. The driver selects the Guard, fills the chat and starts Firebolt's cooldown; the screenshot shows the
# round target portrait, the chat fading toward the top and the powers' cooldown wipes. The copy's powers.lua ticks
# offline (the real one only in the world). RmlUi and the GUI renderer must not warn, and the round portrait redirects
# the base layer.
# Usage: cmake -DEDITOR=<exe> -DSAMPLE=<dir> -DOUT=<dir> -DSCREENSHOT=<png> -DFORBID=<regex> -P starter_mmo_hud_masks.cmake
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
set(project "${OUT}/project")
file(REMOVE_RECURSE "${project}")
file(COPY "${SAMPLE}/" DESTINATION "${project}")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_hud_masks_driver.lua" "${project}/scripts/test_hud_masks.lua")
file(READ "${project}/scenes/meadow.scene" text)
kast_plain_asset_refs(text)
string(REPLACE "\"scripts/powers.lua\"" "\"scripts/powers.lua\", \"scripts/test_hud_masks.lua\"" text "${text}")
file(WRITE "${project}/scenes/meadow.scene" "${text}")
file(READ "${project}/scripts/powers.lua" text)
string(REPLACE "if not (kast.world and kast.world.state() == \"inWorld\") then return end" "-- (test copy: offline)"
       text "${text}")
file(WRITE "${project}/scripts/powers.lua" "${text}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DSCREENSHOT=${SCREENSHOT}" -DFRAMES=400
                        "-DEXTRA_ARGS=--scene ${project}/scenes/meadow.scene --play --no-audio-device --size 1600x1000 --fixed-dt 0.016666667 --screenshot-on-log 5,HUDSHOT"
                        "-DEXPECT_OUTPUT=GUI base layer redirected for clip masks.*test hud masks: HUDSHOT"
                        "-DFORBID_OUTPUT=[[]error|Lua error|${FORBID}"
                        -P "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "HUD masks check failed")
endif()
