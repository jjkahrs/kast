# One samples/starter-mmo live stage: plays a fresh copy of the sample (STATE/<STAGE>_project, with the real dev CA
# setup_realm.cmake copied) in the editor, logged in through --online-login-env (KAST_ONLINE_EMAIL /
# KAST_ONLINE_PASSWORD from the environment), and takes a screenshot. The copy's realms, characters and zone scenes
# also run starter_mmo_test_driver.lua (TEST ONLY, configured with STAGE and CHARACTER), which drives the sample's own
# screens by publishing the events their buttons publish; its game machine "test_route" (a copy's fsms/ and
# project.kast only) counts the portals it took across the zone scenes' switches. BOT=ON first starts an idle kast_bot
# (compose service "bot") standing at the Meadow spawn and stops it afterwards. Every EXPECT_ALL regex must match the editor's log, which is kept in
# STATE/<STAGE>.log; an [error] line, an RmlUi warning or a GUI image warning fails. Prints "SKIP: ..." when the setup didn't finish (no docker or credentials).
# Usage: cmake -DEDITOR=<exe> -DWORLD_DIR=<dir> -DSAMPLE=<dir> -DSTATE=<dir> -DSTAGE=<name> -DCHARACTER=<name>
#              -DSCREENSHOT=<png> -DFRAMES=<n> -DEXPECT_ALL=<regex;...> [-DBOT=ON] [-DEDITOR_ARGS=<flags>]
#              [-DSTOP_CHAT=ON] -P starter_mmo_live_play.cmake
# STOP_CHAT=ON stops the stack's chat service for the run and starts it again afterwards.
# EDITOR_ARGS go to the editor too (e.g. --sim-mouse for the camera stages).
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
if(NOT EXISTS "${STATE}/ready")
  message("SKIP: starter_mmo_live_setup did not get the stack ready (see its output)")
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/starter_mmo_character.cmake") # CHARACTER + the account's 6 letters
set(project "${STATE}/${STAGE}_project")
file(REMOVE_RECURSE "${project}")
file(COPY "${SAMPLE}/" DESTINATION "${project}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/starter_mmo_test_driver.lua" "${project}/scripts/test_driver.lua" @ONLY)
foreach(scene_script "realms|realms" "characters|world_flow" "meadow|world_flow" "cavern|world_flow")
  string(REPLACE "|" ";" scene_script "${scene_script}")
  list(GET scene_script 0 scene)
  list(GET scene_script 1 script)
  file(READ "${project}/scenes/${scene}.scene" text)
  kast_plain_asset_refs(text)
  string(REPLACE "\"scripts/${script}.lua\"" "\"scripts/${script}.lua\", \"scripts/test_driver.lua\"" text "${text}")
  file(WRITE "${project}/scenes/${scene}.scene" "${text}")
endforeach()
file(WRITE "${project}/fsms/test_route.lua" "-- TEST ONLY: portals the test driver took (kept across kast.loadScene)
return { name = \"test_route\", initial = \"c0\", states = { c0 = {}, c1 = {}, c2 = {}, c3 = {}, c4 = {} } }
")
file(READ "${project}/project.kast" text)
string(REPLACE "\"stateMachines\": [\"session\"" "\"stateMachines\": [\"session\", \"test_route\"" text "${text}")
file(WRITE "${project}/project.kast" "${text}")

# The same compose files as the setup, and --no-deps: a "run" with other files would recreate the world server
# without the test-only Lua.
set(override -f docker-compose.yml -f "${SAMPLE}/docker-compose.override.yml" -f "${STATE}/test.override.yml")
set(bot "")
if(BOT)
  string(RANDOM LENGTH 6 ALPHABET abcdefghijklmnopqrstuvwxyz run_id)
  execute_process(COMMAND docker compose ${override} run -d --rm --no-deps bot --mode load --count 1 --behaviour idle
                          --char-option class=warrior
                          --duration 120 --run-id "starter${run_id}"
                  WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE bot
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "could not start kast_bot")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 8) # registers through Mailpit, then enters at the spawn
endif()

if(STOP_CHAT)
  execute_process(COMMAND docker compose ${override} stop chat WORKING_DIRECTORY "${WORLD_DIR}" OUTPUT_QUIET ERROR_QUIET)
endif()
set(log "${STATE}/${STAGE}.log")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DSCREENSHOT=${SCREENSHOT}" "-DFRAMES=${FRAMES}"
                        "-DEXTRA_ARGS=--project ${project} --online-login-env --play --no-audio-device --size 1600x1000 ${EDITOR_ARGS}"
                        "-DOUTPUT_FILE=${log}" "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\] RmlUi|GUI image"
                        -P "${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake"
                RESULT_VARIABLE result)
if(STOP_CHAT)
  execute_process(COMMAND docker compose ${override} up -d --wait --no-deps chat WORKING_DIRECTORY "${WORLD_DIR}"
                  OUTPUT_QUIET ERROR_QUIET)
endif()
if(bot)
  execute_process(COMMAND docker logs "${bot}" OUTPUT_FILE "${STATE}/${STAGE}_bot.log" ERROR_FILE "${STATE}/${STAGE}_bot.log")
  execute_process(COMMAND docker stop "${bot}" OUTPUT_QUIET ERROR_QUIET)
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the editor run failed (see above)")
endif()
file(READ "${log}" output)
foreach(expected IN LISTS EXPECT_ALL)
  if(NOT output MATCHES "${expected}")
    message(FATAL_ERROR "the log does not match '${expected}'")
  endif()
endforeach()
message(STATUS "OK: ${STAGE} stage, ${SCREENSHOT}")
