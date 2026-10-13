# Runs the editor without human input for a fixed frame count and checks it exits 0 and writes a non-empty PNG.
# Each run gets a fresh working directory, so it starts from the default layout (no imgui.ini from earlier runs), and
# uses <work dir>/projects.json as its recent-projects list (--projects-file), never the user's.
# Usage: cmake -DEDITOR=<exe> -DSCREENSHOT=<png> [-DEXTRA_ARGS="<editor flags>"]
#              [-DNEW_PROJECT_NAME=<name> -DNEW_PROJECT_DIR=<dir>] [-DMUST_NOT_EXIST=<path>] [-DMUST_EXIST=<path>]
#              [-DPROJECTS_FROM=<json>] [-DLIST_MUST_CONTAIN=<text>] -P smoke_test.cmake
# NEW_PROJECT_* pass --new-project as separate arguments, so the name may contain spaces. MUST_NOT_EXIST fails the
# test if that path exists after the run (e.g. a folder the New Project modal must not have created); MUST_EXIST if
# it doesn't. PROJECTS_FROM is copied to the list before the run; LIST_MUST_CONTAIN is looked for in it after.
file(REMOVE "${SCREENSHOT}")
set(work_dir "${SCREENSHOT}.run")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
if(DEFINED PROJECTS_FROM)
  file(COPY_FILE "${PROJECTS_FROM}" "${work_dir}/projects.json")
endif()
separate_arguments(EXTRA_ARGS)
if(DEFINED NEW_PROJECT_NAME)
  list(APPEND EXTRA_ARGS --new-project "${NEW_PROJECT_NAME}" "${NEW_PROJECT_DIR}")
endif()
execute_process(COMMAND "${EDITOR}" --frames 120 --screenshot "${SCREENSHOT}" --projects-file "${work_dir}/projects.json"
                        ${EXTRA_ARGS}
                WORKING_DIRECTORY "${work_dir}" RESULT_VARIABLE result)
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
message(STATUS "OK: exit 0, ${SCREENSHOT} is ${size} bytes")
