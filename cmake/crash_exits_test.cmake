# With KAST_NO_CRASH_DIALOGS=1 (set for every test), a crash must end the editor with abort()'s exit code 3 instead
# of opening a "Debug Error!" dialog that waits for a click. TIMEOUT turns a dialog that did open into a failure.
# Usage: cmake -DEDITOR=<exe> -DCRASH=abort|assert [-DEXPECT_OUTPUT=<regex>] -P crash_exits_test.cmake
execute_process(COMMAND "${EDITOR}" --crash ${CRASH} TIMEOUT 30
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 3)
  message(FATAL_ERROR "Expected exit code 3 for --crash ${CRASH}, got '${result}'")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT output MATCHES "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "Output does not match '${EXPECT_OUTPUT}'")
endif()
message(STATUS "OK: --crash ${CRASH} exited with 3, no dialog")
