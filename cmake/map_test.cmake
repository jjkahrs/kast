# The Map tab tests that write files (cmake/map_tests.cmake): a fresh copy of tests/project's tile grid (project.kast,
# maps/ without .meta files, scenes/tilegrid.scene, an empty scenes/empty.scene, scripts/map_stream.lua, map_zones.lua) at <SCREENSHOT>.project, then smoke_test.cmake with every
# @PROJECT@ in EXTRA_ARGS, MUST_EXIST, MUST_NOT_EXIST, FILE_MATCHES and KEEP_FILES naming that copy. The source project is never written.
# KEEP_FILES: a folder whose files (.meta files aside: the editor makes them) must all still be there after the run.
# Usage: cmake -DSOURCE=<tests/project> -DEDITOR=<exe> -DSCREENSHOT=<png> [-DKEEP_FILES=<dir>] [smoke_test.cmake options] -P map_test.cmake
set(copy "${SCREENSHOT}.project")
file(REMOVE_RECURSE "${copy}")
file(MAKE_DIRECTORY "${copy}/scenes" "${copy}/scripts")
# No .meta files: the editor makes fresh ones, so a file a test removes leaves no orphan .meta for the Recycle Bin.
file(COPY "${SOURCE}/project.kast" "${SOURCE}/maps" DESTINATION "${copy}" PATTERN "*.meta" EXCLUDE)
file(COPY "${SOURCE}/scenes/tilegrid.scene" DESTINATION "${copy}/scenes")
file(WRITE "${copy}/scenes/empty.scene" "{\"version\": 5, \"entities\": []}\n")
file(COPY "${SOURCE}/scripts/map_stream.lua" "${SOURCE}/scripts/map_zones.lua" DESTINATION "${copy}/scripts")
foreach(var EXTRA_ARGS MUST_EXIST MUST_NOT_EXIST FILE_MATCHES KEEP_FILES)
  if(DEFINED ${var})
    string(REPLACE "@PROJECT@" "${copy}" ${var} "${${var}}")
  endif()
endforeach()
if(DEFINED KEEP_FILES)
  file(GLOB_RECURSE kept RELATIVE "${KEEP_FILES}" "${KEEP_FILES}/*")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake")
if(DEFINED KEEP_FILES)
  file(GLOB_RECURSE after RELATIVE "${KEEP_FILES}" "${KEEP_FILES}/*")
  list(REMOVE_ITEM kept ${after})
  if(kept)
    message(FATAL_ERROR "Files gone from ${KEEP_FILES}: ${kept}")
  endif()
endif()
