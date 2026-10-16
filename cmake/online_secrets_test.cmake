# The Online panel's test login leaves no secret on disk: logs in (--online-login-env, through WRAPPER's environment and
# fake auth), plays, then builds the project, and fails if any of SECRETS (a list: the password, token texts) appears in
# the editor settings file, any file of the project or the built game.pak. The settings must remember the email.
# Usage: cmake -DEDITOR=<exe> -DPROJECT=<dir> -DWORK_DIR=<dir> -DWRAPPER=<exe;args> -DSECRETS=<text;...>
#              -P online_secrets_test.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(settings "${WORK_DIR}/editor_settings.json")
execute_process(COMMAND ${WRAPPER} "${EDITOR}" --frames 150 --projects-file "${WORK_DIR}/projects.json"
                        --settings-file "${settings}" --scene "${PROJECT}/online_session.scene" --online-login-env --play
                        --no-audio-device
                WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output
                TIMEOUT 60)
message("${output}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "kast_editor (login + Play) exited with ${result}")
endif()
if(NOT output MATCHES "auth.session: admin@example.com")
  message(FATAL_ERROR "Play did not start logged in")
endif()
execute_process(COMMAND "${EDITOR}" --project "${PROJECT}" --build "${WORK_DIR}/game" --settings-file "${settings}"
                WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output
                TIMEOUT 60)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Build failed (${result}): ${output}")
endif()
file(READ "${settings}" text)
if(NOT text MATCHES "\"onlineEmail\": \"admin@example.com\"")
  message(FATAL_ERROR "the settings don't remember the email: ${text}")
endif()
file(GLOB_RECURSE files LIST_DIRECTORIES false "${PROJECT}/*")
list(APPEND files "${settings}" "${WORK_DIR}/game/game.pak")
foreach(file IN LISTS files)
  file(READ "${file}" bytes HEX)
  foreach(secret IN LISTS SECRETS)
    string(HEX "${secret}" hex)
    string(FIND "${bytes}" "${hex}" at)
    if(NOT at EQUAL -1)
      message(FATAL_ERROR "'${secret}' found in ${file}")
    endif()
  endforeach()
endforeach()
list(LENGTH files count)
message(STATUS "OK: no secret in ${count} files (settings, project, game.pak)")
