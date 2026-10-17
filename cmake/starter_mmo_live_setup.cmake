# Prepares the samples/starter-mmo live tests (ctest starter_mmo_live_*) against the services/world compose stack:
# (re)starts it with the sample's docker-compose.override.yml (its Lua and maps; services/world stays unchanged), checks
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
execute_process(COMMAND docker compose -f docker-compose.yml -f "${SAMPLE}/docker-compose.override.yml" up -d --wait
                WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result TIMEOUT 900)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "docker compose up with the sample override failed (${result}): see docker compose logs world")
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
