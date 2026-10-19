# The Zone tab's Bake Navmesh (kast_editor --bake) on a copy of a project (the source is never written): kast_mapbake
# must exit 0 and write .server/nav/*.navtile for the scene's zone, its output and exit code must reach the Console, and
# the editor must keep drawing while it runs. Skipped (passes) when kast_mapbake is not built.
# Usage: cmake -DEDITOR=<kast_editor> -DPROJECT=<dir> -DSCENE=<project-relative .scene> -DMAP=<folder>
#              -DMAPBAKE=<kast_mapbake> -DOUT_DIR=<dir> -P editor_zone_bake_test.cmake
if(NOT MAPBAKE OR NOT EXISTS "${MAPBAKE}")
  message(STATUS "kast_mapbake not built (${MAPBAKE}): skipped")
  return()
endif()
set(copy "${OUT_DIR}/project")
set(shot "${OUT_DIR}/zone_bake.png")
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
file(COPY "${PROJECT}/" DESTINATION "${copy}")
file(REMOVE_RECURSE "${copy}/maps/${MAP}/.server/nav")
execute_process(COMMAND "${EDITOR}" --frames 20 --screenshot "${shot}" --projects-file "${OUT_DIR}/projects.json"
                        --settings-file "${OUT_DIR}/editor_settings.json" --project "${copy}" --scene "${copy}/${SCENE}"
                        --bake --mapbake "${MAPBAKE}" --focus-panel Zone --zone-section Bake
                WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "kast_editor --bake exited with ${result}")
endif()
file(GLOB navtiles "${copy}/maps/${MAP}/.server/nav/*.navtile")
if(NOT navtiles)
  message(FATAL_ERROR "no navtiles written")
endif()
if(NOT out MATCHES "Zone: Bake finished: exit code 0" OR NOT out MATCHES "\\[info\\] Bake: ")
  message(FATAL_ERROR "kast_mapbake's output or exit code is not in the log")
endif()
string(REGEX MATCH "([0-9]+) frames drawn while kast_mapbake ran" drawn "${out}")
if(NOT drawn OR CMAKE_MATCH_1 LESS 2)
  message(FATAL_ERROR "the editor did not keep drawing while kast_mapbake ran")
endif()
if(NOT EXISTS "${shot}")
  message(FATAL_ERROR "no screenshot ${shot}")
endif()
message(STATUS "OK: ${drawn}; ${navtiles}")
