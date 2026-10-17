# Undoes starter_mmo_live_setup.cmake: recreates the world server with services/world/docker-compose.yml alone, so it
# serves services/world/lua and tests/project/maps again (world_live, replication_live and online_live expect them),
# and stops the stack if the setup started it. Nothing to do if the setup never touched the stack.
# Usage: cmake -DWORLD_DIR=<services/world> -DSTATE=<dir> -P starter_mmo_live_teardown.cmake
if(NOT EXISTS "${STATE}/override")
  message(STATUS "starter_mmo live: the stack was not changed, nothing to restore")
  return()
endif()
if(EXISTS "${STATE}/started_stack")
  execute_process(COMMAND docker compose down WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result TIMEOUT 300)
else()
  execute_process(COMMAND docker compose up -d --force-recreate --wait world WORKING_DIRECTORY "${WORLD_DIR}"
                  RESULT_VARIABLE result TIMEOUT 600)
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "could not restore the default compose stack (${result})")
endif()
file(REMOVE "${STATE}/override" "${STATE}/ready")
message(STATUS "starter_mmo live: default world mounts restored")
