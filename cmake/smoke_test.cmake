# Runs the editor without human input for a fixed frame count and checks it exits 0 and writes a non-empty PNG.
# Each run gets a fresh working directory, so it starts from the default layout (no imgui.ini from earlier runs).
# Usage: cmake -DEDITOR=<exe> -DSCREENSHOT=<png> [-DEXTRA_ARGS="<editor flags>"] -P smoke_test.cmake
file(REMOVE "${SCREENSHOT}")
set(work_dir "${SCREENSHOT}.run")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
separate_arguments(EXTRA_ARGS)
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
message(STATUS "OK: exit 0, ${SCREENSHOT} is ${size} bytes")
