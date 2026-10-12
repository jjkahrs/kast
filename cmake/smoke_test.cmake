# Runs the editor without human input for a fixed frame count and checks it exits 0 and writes a non-empty PNG.
# Each run gets a fresh working directory, so it starts from the default layout (no imgui.ini from earlier runs).
# Usage: cmake -DEDITOR=<exe> -DSCREENSHOT=<png> [-DEXTRA_ARGS="<editor flags>"]
#              [-DNEW_PROJECT_NAME=<name> -DNEW_PROJECT_DIR=<dir>] [-DMUST_NOT_EXIST=<path>] -P smoke_test.cmake
# NEW_PROJECT_* pass --new-project as separate arguments, so the name may contain spaces. MUST_NOT_EXIST fails the
# test if that path exists after the run (e.g. a folder the New Project modal must not have created).
file(REMOVE "${SCREENSHOT}")
set(work_dir "${SCREENSHOT}.run")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
separate_arguments(EXTRA_ARGS)
if(DEFINED NEW_PROJECT_NAME)
  list(APPEND EXTRA_ARGS --new-project "${NEW_PROJECT_NAME}" "${NEW_PROJECT_DIR}")
endif()
execute_process(COMMAND "${EDITOR}" --frames 120 --screenshot "${SCREENSHOT}" ${EXTRA_ARGS}
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
message(STATUS "OK: exit 0, ${SCREENSHOT} is ${size} bytes")
