# Offline check of samples/starter-mmo's chat and quest log lists (scripts/chat.lua, scripts/quest_log.lua): a fresh
# copy of the sample (OUT/project) whose meadow.scene also runs starter_mmo_lists_driver.lua (TEST ONLY). The driver
# fakes 15 chat lines and 25 active quests and opens the quest log (the copy's quest_log.lua asks no server): all 25 rows
# exist (none past them), the 23rd selects; the screenshot shows the chat's 15 lines and the log on Quest 23.
# Usage: cmake -DEDITOR=<exe> -DSAMPLE=<dir> -DOUT=<dir> -DSCREENSHOT=<png> -P starter_mmo_lists.cmake
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
set(project "${OUT}/project")
file(REMOVE_RECURSE "${project}")
file(COPY "${SAMPLE}/" DESTINATION "${project}")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_lists_driver.lua" "${project}/scripts/test_lists.lua")
file(READ "${project}/scenes/meadow.scene" text)
kast_plain_asset_refs(text)
string(REPLACE "\"scripts/quest_log.lua\"" "\"scripts/quest_log.lua\", \"scripts/test_lists.lua\"" text "${text}")
file(WRITE "${project}/scenes/meadow.scene" "${text}")
# Offline there is no server to ask for the quest descriptions (the driver publishes its answer).
file(READ "${project}/scripts/quest_log.lua" text)
string(REPLACE "kast.world.send(\"quest.log\", {})" "-- (test copy: offline)" text "${text}")
file(WRITE "${project}/scripts/quest_log.lua" "${text}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DSCREENSHOT=${SCREENSHOT}" -DFRAMES=300
                        "-DEXTRA_ARGS=--scene ${project}/scenes/meadow.scene --play --no-audio-device --size 1600x1000 --screenshot-on-log 5,LISTSHOT"
                        "-DEXPECT_OUTPUT=test lists: row 24 true, row 25 false, chat lines 15.*test lists: LISTSHOT"
                        "-DFORBID_OUTPUT=[[]error|Lua error|[[]warn[]] RmlUi"
                        -P "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "chat and quest log lists check failed")
endif()
