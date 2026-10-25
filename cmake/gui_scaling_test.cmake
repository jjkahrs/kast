# One run of kast_player or kast_editor for the GUI scaling tests (gui_scaling_tests.cmake), on a fresh copy of a
# folder, then checks of its log and of a file it left.
# Usage: cmake -DEXE=<exe name or path> -DWORK_DIR=<dir> [-DCOPY_FROM=<dir>] [-DARGS=<|-separated arguments>]
#              [-DPRE_FILE=<path> -DPRE_TEXT=<text>] [-DEXPECT=<regex>] [-DEXPECT2=<regex>] [-DFORBID=<regex>]
#              [-DCHECK_FILE=<path> [-DFILE_EXPECT=<regex>] [-DFILE_FORBID=<regex>]] [-DSECOND_DISPLAY=<kast_self_check>]
#              -P gui_scaling_test.cmake
# COPY_FROM is copied to WORK_DIR first (a built game: EXE is then the game's .exe in it). PRE_FILE is written with
# PRE_TEXT before the run (e.g. a corrupt user settings file). The run must exit 0; its log must match EXPECT and
# EXPECT2 and not FORBID. CHECK_FILE is read with all whitespace removed, so "width":800 matches however it is
# indented. SECOND_DISPLAY: the run needs a display besides the main one (asked of kast_self_check --displays); without
# one it prints "SKIPPED" and does nothing, and it always runs with KAST_WINDOW_POS=secondary.
if(DEFINED SECOND_DISPLAY)
  execute_process(COMMAND "${SECOND_DISPLAY}" --displays OUTPUT_VARIABLE displays)
  if(NOT displays MATCHES "displays: ([0-9]+)" OR CMAKE_MATCH_1 LESS 2)
    message("SKIPPED: this run switches display modes and needs a second display (${displays})")
    return()
  endif()
  set(ENV{KAST_WINDOW_POS} secondary)
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
if(DEFINED COPY_FROM)
  file(COPY "${COPY_FROM}/" DESTINATION "${WORK_DIR}")
else()
  file(MAKE_DIRECTORY "${WORK_DIR}")
endif()
if(DEFINED PRE_FILE)
  file(WRITE "${PRE_FILE}" "${PRE_TEXT}")
endif()
string(REPLACE "|" ";" ARGS "${ARGS}")
if(NOT IS_ABSOLUTE "${EXE}")
  set(EXE "${WORK_DIR}/${EXE}")
endif()
execute_process(COMMAND "${EXE}" ${ARGS} WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 90
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${EXE} exited with '${result}'")
endif()
foreach(regex IN ITEMS "${EXPECT}" "${EXPECT2}")
  if(NOT regex STREQUAL "" AND NOT output MATCHES "${regex}")
    message(FATAL_ERROR "Output does not match '${regex}'")
  endif()
endforeach()
if(DEFINED FORBID AND output MATCHES "${FORBID}")
  message(FATAL_ERROR "Output matches '${FORBID}'")
endif()
if(DEFINED CHECK_FILE)
  if(NOT EXISTS "${CHECK_FILE}")
    message(FATAL_ERROR "${CHECK_FILE} was not written")
  endif()
  file(READ "${CHECK_FILE}" text)
  string(REGEX REPLACE "[ \t\r\n]+" "" text "${text}")
  message("${CHECK_FILE}: ${text}")
  if(DEFINED FILE_EXPECT AND NOT text MATCHES "${FILE_EXPECT}")
    message(FATAL_ERROR "${CHECK_FILE} does not match '${FILE_EXPECT}'")
  endif()
  if(DEFINED FILE_FORBID AND text MATCHES "${FILE_FORBID}")
    message(FATAL_ERROR "${CHECK_FILE} matches '${FILE_FORBID}'")
  endif()
endif()
message(STATUS "OK")
