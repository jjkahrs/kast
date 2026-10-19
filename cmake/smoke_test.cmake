# Runs the editor without human input for a fixed frame count and checks it exits 0 and writes a non-empty PNG.
# Each run gets a fresh working directory, so it starts from the default layout (no imgui.ini from earlier runs), and
# uses <work dir>/projects.json as its recent-projects list (--projects-file), never the user's.
# Usage: cmake -DEDITOR=<exe> -DSCREENSHOT=<png> [-DEXTRA_ARGS="<editor flags>"]
#              [-DNEW_PROJECT_NAME=<name> -DNEW_PROJECT_DIR=<dir>] [-DMUST_NOT_EXIST=<path>] [-DMUST_EXIST=<path>]
#              [-DPROJECTS_FROM=<json>] [-DLIST_MUST_CONTAIN=<text>] [-DFOCUS_PANEL=<window>]
#              [-DEXPECT_OUTPUT=<regex> [-DEXPECT_COUNT=<n>]] [-DFORBID_OUTPUT=<regex>] [-DFRAMES=<n>]
#              [-DWRAPPER=<exe;args>] [-DSETTINGS_FROM=<json>] [-DSETTINGS_MUST_NOT_CONTAIN=<text;...>]
#              [-DOUTPUT_FILE=<log>] [-DLAYOUT_FROM=<imgui.ini>] [-DLAYOUT_SAME_DOCK=<window;window...>] -P smoke_test.cmake
# NEW_PROJECT_* pass --new-project as separate arguments, so the name may contain spaces; FOCUS_PANEL passes
# --focus-panel the same way (e.g. "State Machines"). MUST_NOT_EXIST fails the test if that path exists after the run
# (e.g. a folder the New Project modal must not have created); MUST_EXIST if it doesn't. PROJECTS_FROM is copied to
# the list before the run; LIST_MUST_CONTAIN is looked for in it after. EXPECT_OUTPUT must match the editor's log
# (exactly EXPECT_COUNT times if given; for that count the log's ";" read as ","); FORBID_OUTPUT must not match it;
# OUTPUT_FILE gets a copy of it (e.g. for a later secrets check). FRAMES is how many frames run
# before the screenshot (default 120). WRAPPER (a list) runs the editor through another program, e.g.
# kast_net_test_server <args> -- (its servers are up while the editor runs).
# The editor settings are <work dir>/editor_settings.json (--settings-file), never the user's: SETTINGS_FROM is copied
# there before the run; SETTINGS_MUST_NOT_CONTAIN (a list) fails the test if the file holds any of those texts after.
# LAYOUT_FROM is copied to <work dir>/imgui.ini, the editor's saved panel layout, before the run. LAYOUT_SAME_DOCK (a
# list of window titles) fails the test unless the imgui.ini saved at exit docks them all in one node (tabs together).
if(NOT DEFINED FRAMES)
  set(FRAMES 120)
endif()
file(REMOVE "${SCREENSHOT}")
set(work_dir "${SCREENSHOT}.run")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
if(DEFINED PROJECTS_FROM)
  file(COPY_FILE "${PROJECTS_FROM}" "${work_dir}/projects.json")
endif()
if(DEFINED SETTINGS_FROM)
  file(COPY_FILE "${SETTINGS_FROM}" "${work_dir}/editor_settings.json")
endif()
if(DEFINED LAYOUT_FROM)
  file(COPY_FILE "${LAYOUT_FROM}" "${work_dir}/imgui.ini")
endif()
separate_arguments(EXTRA_ARGS)
if(DEFINED NEW_PROJECT_NAME)
  list(APPEND EXTRA_ARGS --new-project "${NEW_PROJECT_NAME}" "${NEW_PROJECT_DIR}")
endif()
if(DEFINED FOCUS_PANEL)
  list(APPEND EXTRA_ARGS --focus-panel "${FOCUS_PANEL}")
endif()
execute_process(COMMAND ${WRAPPER} "${EDITOR}" --frames ${FRAMES} --screenshot "${SCREENSHOT}" --projects-file "${work_dir}/projects.json" --settings-file "${work_dir}/editor_settings.json"
                        ${EXTRA_ARGS}
                WORKING_DIRECTORY "${work_dir}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(DEFINED OUTPUT_FILE)
  file(WRITE "${OUTPUT_FILE}" "${output}")
endif()
if(DEFINED FORBID_OUTPUT AND output MATCHES "${FORBID_OUTPUT}")
  message(FATAL_ERROR "Output matches the forbidden '${FORBID_OUTPUT}': '${CMAKE_MATCH_0}'")
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "kast_editor exited with ${result}")
endif()
if(NOT EXISTS "${SCREENSHOT}")
  message(FATAL_ERROR "Screenshot not written: ${SCREENSHOT}")
endif()
file(SIZE "${SCREENSHOT}" size)
if(size EQUAL 0)
  message(FATAL_ERROR "Screenshot is empty: ${SCREENSHOT}")
endif()
if(DEFINED EXPECT_OUTPUT AND NOT output MATCHES "${EXPECT_OUTPUT}")
  message(FATAL_ERROR "Output does not match '${EXPECT_OUTPUT}'")
endif()
if(DEFINED EXPECT_COUNT)
  string(REPLACE ";" "," listable "${output}") # a ; in a match would split it into two list items
  string(REGEX MATCHALL "${EXPECT_OUTPUT}" matches "${listable}")
  list(LENGTH matches count)
  if(NOT count EQUAL EXPECT_COUNT)
    message(FATAL_ERROR "'${EXPECT_OUTPUT}' matched ${count} times, expected ${EXPECT_COUNT}")
  endif()
endif()
if(DEFINED MUST_NOT_EXIST AND EXISTS "${MUST_NOT_EXIST}")
  message(FATAL_ERROR "Must not exist after the run: ${MUST_NOT_EXIST}")
endif()
if(DEFINED MUST_EXIST AND NOT EXISTS "${MUST_EXIST}")
  message(FATAL_ERROR "Must exist after the run: ${MUST_EXIST}")
endif()
if(DEFINED LIST_MUST_CONTAIN)
  file(READ "${work_dir}/projects.json" list)
  string(FIND "${list}" "${LIST_MUST_CONTAIN}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "projects.json does not contain '${LIST_MUST_CONTAIN}':\n${list}")
  endif()
endif()
if(DEFINED LAYOUT_SAME_DOCK)
  file(READ "${work_dir}/imgui.ini" ini)
  set(docks "")
  foreach(window IN LISTS LAYOUT_SAME_DOCK)
    if(NOT ini MATCHES "\\[Window\\]\\[${window}\\][^[]*DockId=(0x[0-9A-Fa-f]+)")
      message(FATAL_ERROR "imgui.ini: '${window}' is not docked:\n${ini}")
    endif()
    list(APPEND docks "${window} ${CMAKE_MATCH_1}")
    if(NOT DEFINED dock)
      set(dock "${CMAKE_MATCH_1}")
    elseif(NOT dock STREQUAL CMAKE_MATCH_1)
      message(FATAL_ERROR "imgui.ini: not docked together: ${docks}")
    endif()
  endforeach()
endif()
message(STATUS "OK: exit 0, ${SCREENSHOT} is ${size} bytes")
if(DEFINED SETTINGS_MUST_NOT_CONTAIN AND EXISTS "${work_dir}/editor_settings.json")
  file(READ "${work_dir}/editor_settings.json" settings)
  foreach(secret IN LISTS SETTINGS_MUST_NOT_CONTAIN)
    string(FIND "${settings}" "${secret}" at)
    if(NOT at EQUAL -1)
      message(FATAL_ERROR "editor_settings.json holds '${secret}'")
    endif()
  endforeach()
endif()
