# Prepares the samples/starter-mmo live tests (ctest starter_mmo_live_*) against the services/world compose stack:
# (re)starts it with the sample's docker-compose.override.yml (its Lua and maps; services/world stays unchanged) and a
# test-only override that adds starter_mmo_test_server.lua to a copy of the sample's server Lua, checks
# the world server loaded them, runs the sample's setup_realm.cmake (CA file + realm "Starter Realm") and makes sure the
# test account in KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD can log in (starter_mmo_account.ps1: registers and verifies
# it through Mailpit if needed). The credentials come only from the environment ctest runs in: never from a file, a
# CMake variable or a command line. Writes STATE/override once the stack runs with the override (the teardown restores
# the default mounts), STATE/started_stack if the stack wasn't running before (the teardown stops it again) and
# STATE/ready when everything worked. Without docker, a running Docker engine or the credentials it prints "SKIP: ..."
# and writes nothing, so every live test skips.
# Usage: cmake -DWORLD_DIR=<services/world> -DSAMPLE=<samples/starter-mmo> -DSTATE=<dir> -P starter_mmo_live_setup.cmake
file(REMOVE_RECURSE "${STATE}")
file(MAKE_DIRECTORY "${STATE}")
if(NOT DEFINED ENV{KAST_ONLINE_EMAIL} OR NOT DEFINED ENV{KAST_ONLINE_PASSWORD})
  message("SKIP: set KAST_ONLINE_EMAIL and KAST_ONLINE_PASSWORD (a test account) in the environment to run them")
  return()
endif()
execute_process(COMMAND docker info RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(NOT result EQUAL 0)
  message("SKIP: docker is not on PATH or its engine is not running")
  return()
endif()
execute_process(COMMAND docker compose ps --status running --services WORKING_DIRECTORY "${WORLD_DIR}"
                OUTPUT_VARIABLE running ERROR_QUIET)
if(NOT running MATCHES "world")
  file(WRITE "${STATE}/started_stack" "")
endif()
file(WRITE "${STATE}/override" "")
# The world server's Lua: a copy of the sample's server folders plus the test-only starter_mmo_test_server.lua (test.seed,
# test.bag), mounted over the sample's own by a third compose file, so the sample itself never holds test code. The
# same file lets the test account hold 20 characters (every stage has its own; the default is 10).
set(server_lua "${STATE}/server_lua")
foreach(dir components fsms items npcs systems)
  file(COPY "${SAMPLE}/${dir}" DESTINATION "${server_lua}")
endforeach()
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_test_server.lua" "${server_lua}/systems/zz_test.lua")
# Long volume syntax: a Windows path's drive colon would split the short "src:dst:ro" form.
file(WRITE "${STATE}/test.override.yml" "services:
  world:
    environment:
      MAX_CHARS_PER_ACCOUNT: \"20\"
    volumes:
      - type: bind
        source: \"${server_lua}\"
        target: /app/lua
        read_only: true
")
execute_process(COMMAND docker compose -f docker-compose.yml -f "${SAMPLE}/docker-compose.override.yml"
                        -f "${STATE}/test.override.yml" up -d --wait
                WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result TIMEOUT 900)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "docker compose up with the sample override failed (${result}): see docker compose logs world")
endif()
# A fresh world server every run: it loads the Lua copy above (an unchanged config would keep a running one, with
# whatever it loaded before) and starts with every NPC alive.
execute_process(COMMAND docker compose -f docker-compose.yml -f "${SAMPLE}/docker-compose.override.yml"
                        -f "${STATE}/test.override.yml" up -d --wait --force-recreate --no-deps world
                WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result TIMEOUT 600)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "recreating the world server failed (${result}): see docker compose logs world")
endif()
execute_process(COMMAND docker compose exec -T world ls /app/lua/systems WORKING_DIRECTORY "${WORLD_DIR}" OUTPUT_VARIABLE systems)
if(NOT systems MATCHES "zz_test.lua")
  message(FATAL_ERROR "the world server does not see the test-only system: ${systems}")
endif()
execute_process(COMMAND docker compose logs --no-color world WORKING_DIRECTORY "${WORLD_DIR}" OUTPUT_VARIABLE logs)
if(NOT logs MATCHES "maps loaded" OR logs MATCHES "Lua error|startup failed")
  message(FATAL_ERROR "the world server did not load the sample cleanly:\n${logs}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -P "${SAMPLE}/setup_realm.cmake" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "setup_realm.cmake failed")
endif()
execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_LIST_DIR}/starter_mmo_account.ps1"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the test account is not usable")
endif()
file(WRITE "${STATE}/ready" "")
message(STATUS "starter_mmo live: stack up with the sample override, realm and account ready")
