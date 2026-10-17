# After the samples/starter-mmo live stages: the test account's password (KAST_ONLINE_PASSWORD, read from the
# environment here too) and JWTs ("eyJ", the base64 of '{"') are in no text file of the sample, of the stages' project
# copies, of their editor settings files and in none of the editor logs the stages kept. "SKIP: ..." if the setup
# didn't finish.
# Usage: cmake -DSAMPLE=<dir> -DSTATE=<dir> -DSHOTS=<png;...> -P starter_mmo_live_secrets.cmake
if(NOT EXISTS "${STATE}/ready")
  message("SKIP: starter_mmo_live_setup did not get the stack ready (see its output)")
  return()
endif()
set(secrets "$ENV{KAST_ONLINE_PASSWORD}" "eyJ")
file(GLOB_RECURSE files LIST_DIRECTORIES false "${SAMPLE}/*" "${STATE}/*")
foreach(shot IN LISTS SHOTS)
  list(APPEND files "${shot}.run/editor_settings.json")
endforeach()
list(FILTER files EXCLUDE REGEX "\\.(glb|r16|png)$") # binary assets, copied unchanged
set(checked 0)
foreach(file IN LISTS files)
  if(NOT EXISTS "${file}")
    continue()
  endif()
  file(READ "${file}" text)
  foreach(secret IN LISTS secrets)
    string(FIND "${text}" "${secret}" at)
    if(NOT at EQUAL -1)
      message(FATAL_ERROR "${file} holds a secret (the password or a JWT)")
    endif()
  endforeach()
  math(EXPR checked "${checked} + 1")
endforeach()
message(STATUS "OK: no password or token in ${checked} files")
