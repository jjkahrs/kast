# Opening a bad or missing scene (or project, with -DFLAG=--project) must fail cleanly with exit code 1.
# Usage: cmake -DEDITOR=<exe> -DSCENE=<path> [-DFLAG=--project] -P load_fails_test.cmake
if(NOT DEFINED FLAG)
  set(FLAG --scene)
endif()
execute_process(COMMAND "${EDITOR}" ${FLAG} "${SCENE}" RESULT_VARIABLE result)
if(NOT result EQUAL 1)
  message(FATAL_ERROR "Expected exit code 1 for ${FLAG} ${SCENE}, got ${result}")
endif()
message(STATUS "OK: ${FLAG} ${SCENE} exited with 1")
