# The Map tab tests that write files (cmake/map_tests.cmake): a fresh copy of tests/project's tile grid (project.kast,
# maps/ without .meta files, scenes/tilegrid.scene, scripts/map_stream.lua) at <SCREENSHOT>.project, then smoke_test.cmake with every
# @PROJECT@ in EXTRA_ARGS, MUST_EXIST and MUST_NOT_EXIST naming that copy. The source project is never written.
# Usage: cmake -DSOURCE=<tests/project> -DEDITOR=<exe> -DSCREENSHOT=<png> [smoke_test.cmake options] -P map_test.cmake
set(copy "${SCREENSHOT}.project")
file(REMOVE_RECURSE "${copy}")
file(MAKE_DIRECTORY "${copy}/scenes" "${copy}/scripts")
# No .meta files: the editor makes fresh ones, so a file a test removes leaves no orphan .meta for the Recycle Bin.
file(COPY "${SOURCE}/project.kast" "${SOURCE}/maps" DESTINATION "${copy}" PATTERN "*.meta" EXCLUDE)
file(COPY "${SOURCE}/scenes/tilegrid.scene" DESTINATION "${copy}/scenes")
file(COPY "${SOURCE}/scripts/map_stream.lua" DESTINATION "${copy}/scripts")
foreach(var EXTRA_ARGS MUST_EXIST MUST_NOT_EXIST)
  if(DEFINED ${var})
    string(REPLACE "@PROJECT@" "${copy}" ${var} "${${var}}")
  endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/smoke_test.cmake")
