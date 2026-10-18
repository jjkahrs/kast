# The legacy-character live stage (ctest starter_mmo_live_legacy): a character from before classes enters as a Warrior
# and keeps it. Plays the driver's "legacy" stage once (it creates CHARACTER as a Mage the first time), then makes it
# look like a character from before migration 004 in MySQL (options NULL, no Class component), plays the stage again,
# which must show Class warrior with a Warrior's 140 max Health and powers, and finally checks that MySQL holds Class
# warrior for it, still without options: the Warrior was saved, not just worked out again on the next login.
# Usage: as starter_mmo_live_play.cmake, without STAGE and EXPECT_ALL.
if(NOT EXISTS "${STATE}/ready")
  message("SKIP: starter_mmo_live_setup did not get the stack ready (see its output)")
  return()
endif()
set(base "${CHARACTER}")
include("${CMAKE_CURRENT_LIST_DIR}/starter_mmo_character.cmake")
set(name "${CHARACTER}")

function(play screenshot expected) # expected: regexes separated by "&"
  string(REPLACE "&" ";" expected "${expected}")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DEDITOR=${EDITOR}" "-DWORLD_DIR=${WORLD_DIR}" "-DSAMPLE=${SAMPLE}"
                          "-DSTATE=${STATE}" -DSTAGE=legacy "-DCHARACTER=${base}" "-DSCREENSHOT=${screenshot}"
                          "-DFRAMES=${FRAMES}" "-DEXPECT_ALL=${expected}" "-DEDITOR_ARGS=--screenshot-on-log 30,SHOT"
                          -P "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_live_play.cmake"
                  RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "the legacy stage failed (see above)")
  endif()
endfunction()

# The root password stays inside the mysql container ($MYSQL_ROOT_PASSWORD is its own environment).
function(sql statement out)
  execute_process(COMMAND docker compose exec -T mysql sh -c
                          "mysql -uroot -p\"\$MYSQL_ROOT_PASSWORD\" -N -B kast_world -e \"${statement}\""
                  WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "mysql failed: ${errors}")
  endif()
  set(${out} "${output}" PARENT_SCOPE)
endfunction()

play("${STATE}/legacy_first.png" "test driver: entered map")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3) # the logout's save lands first
sql("UPDATE characters SET options = NULL, components = JSON_REMOVE(components, '\$.Class') WHERE name = '${name}'" ignored)
sql("SELECT COALESCE(JSON_EXTRACT(components, '\$.Class'), 'none'), options IS NULL FROM characters WHERE name = '${name}'" before)
if(NOT before MATCHES "^none\t1$")
  message(FATAL_ERROR "could not strip ${name}'s class: '${before}'")
endif()
play("${SCREENSHOT}" "test driver: characters: .*${name} .no class.&test driver: Class warrior on&test driver: Health [0-9]+ / 140 on&test driver: powers of warrior: 1 Strike, 2 Heroic Slam, 3 Shield Wall")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
sql("SELECT JSON_UNQUOTE(JSON_EXTRACT(components, '\$.Class.id')), options IS NULL FROM characters WHERE name = '${name}'" after)
if(NOT after MATCHES "^warrior\t1$")
  message(FATAL_ERROR "${name} was not saved as a Warrior: '${after}'")
endif()
message(STATUS "OK: legacy ${name} entered as a Warrior and was saved as one")
