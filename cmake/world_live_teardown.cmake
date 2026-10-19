# Undoes world_live_setup.cmake's switch to the default world: recreates the world server with the compose files it ran
# with before (RESTORE_OUT, comma-separated), e.g. samples/starter-mmo's override. Nothing to do if the setup didn't
# switch it.
# Usage: cmake -DWORLD_DIR=<services/world> -DRESTORE_OUT=<file> -P world_live_teardown.cmake
if(NOT EXISTS "${RESTORE_OUT}")
  return()
endif()
file(READ "${RESTORE_OUT}" config_files)
string(STRIP "${config_files}" config_files)
string(REPLACE "," ";" config_files "${config_files}")
set(args)
foreach(file IN LISTS config_files)
  list(APPEND args -f "${file}")
endforeach()
execute_process(COMMAND docker compose ${args} up -d --force-recreate --wait world WORKING_DIRECTORY "${WORLD_DIR}"
                RESULT_VARIABLE result TIMEOUT 600)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "could not restore the world server's compose files (${result}): ${config_files}")
endif()
file(REMOVE "${RESTORE_OUT}")
message(STATUS "world_live: world server restored with ${config_files}")
