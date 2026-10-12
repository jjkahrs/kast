# Opening a bad or missing scene must fail cleanly with exit code 1 (not crash, not succeed).
# Usage: cmake -DEDITOR=<exe> -DSCENE=<scene> -P load_fails_test.cmake
execute_process(COMMAND "${EDITOR}" --scene "${SCENE}" RESULT_VARIABLE result)
if(NOT result EQUAL 1)
  message(FATAL_ERROR "Expected exit code 1 for --scene ${SCENE}, got ${result}")
endif()
message(STATUS "OK: --scene ${SCENE} exited with 1")
