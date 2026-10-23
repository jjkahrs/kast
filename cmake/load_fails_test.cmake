# Opening a bad or missing scene (or project, with -DFLAG=--project) must fail cleanly with exit code 1, and with
# -DEXPECT_OUTPUT=<regex> log an error matching it.
# Usage: cmake -DEDITOR=<exe> -DSCENE=<path> [-DFLAG=--project] [-DEXPECT_OUTPUT=<regex>] -P load_fails_test.cmake
if(NOT DEFINED FLAG)
  set(FLAG --scene)
endif()
execute_process(COMMAND "${EDITOR}" ${FLAG} "${SCENE}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 1)
  message(FATAL_ERROR "Expected exit code 1 for ${FLAG} ${SCENE}, got ${result}")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT output MATCHES "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "Output does not match '${EXPECT_OUTPUT}'")
endif()
message(STATUS "OK: ${FLAG} ${SCENE} exited with 1")
