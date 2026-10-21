# Lines logged right before the process ends must reach stdout (the log backend writes them, not the caller):
# a crash (SIGSEGV, abort) via world_self_check --log-crash, and a fatal startup error in kast_world.
foreach(how segv abort)
  execute_process(COMMAND ${CHECK} --log-crash ${how} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(rc EQUAL 0 OR NOT out MATCHES "\"msg\":\"before crash\",\"how\":\"${how}\"")
    message(FATAL_ERROR "--log-crash ${how}: exit ${rc}\nstdout:\n${out}\nstderr:\n${err}")
  endif()
  message(STATUS "crash ${how}: exit ${rc}\n${out}")
endforeach()

execute_process(COMMAND ${CMAKE_COMMAND} -E env LOG_LEVEL=debug MAP_ROOT=${OUT}/no-such-maps AUTH_JWKS_URL=http://127.0.0.1:1/jwks
  DB_HOST=127.0.0.1 DB_USER=u DB_PASSWORD=p DB_NAME=n REALM_ID=1 ${WORLD}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 1 OR NOT out MATCHES "\"msg\":\"startup failed: maps\"")
  message(FATAL_ERROR "kast_world with a missing MAP_ROOT: exit ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()
message(STATUS "fatal startup: exit ${rc}\n${out}")
