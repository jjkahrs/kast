# --launcher-open <project>: the Project Manager's open path without a window. Checks it exits 0 (it waits for the
# editor it started and returns its code), that the editor took its --screenshot (flags after --), and that <project>
# is first in the recent-projects list.
# Usage: cmake -DEDITOR=<exe> -DPROJECT=<dir> -DSCREENSHOT=<png> -P launcher_open_test.cmake
file(REMOVE "${SCREENSHOT}")
set(work_dir "${SCREENSHOT}.run")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
set(list_file "${work_dir}/projects.json")
execute_process(COMMAND "${EDITOR}" --launcher-open "${PROJECT}" --projects-file "${list_file}"
                        -- --frames 120 --screenshot "${SCREENSHOT}"
                WORKING_DIRECTORY "${work_dir}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "--launcher-open exited with ${result}")
endif()
if(NOT EXISTS "${SCREENSHOT}")
  message(FATAL_ERROR "The started editor wrote no screenshot: ${SCREENSHOT}")
endif()
file(SIZE "${SCREENSHOT}" size)
if(size EQUAL 0)
  message(FATAL_ERROR "Screenshot is empty: ${SCREENSHOT}")
endif()
file(READ "${list_file}" list)
string(JSON first GET "${list}" 0 path)
if(NOT first STREQUAL "${PROJECT}")
  message(FATAL_ERROR "First recent project is '${first}', expected '${PROJECT}':\n${list}")
endif()
message(STATUS "OK: exit 0, editor screenshot ${SCREENSHOT} is ${size} bytes, ${PROJECT} listed first")
