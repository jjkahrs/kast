# Prepares world_live against the services/world compose stack: copies the world server's dev certificate out of its
# container (the world-certs volume, /app/certs/server.crt) to CA_OUT, and adds the realm "Kast Live" (127.0.0.1:7777,
# online) to auth's database unless it exists. Test data only; nothing under services/ changes. When the stack isn't
# running it does nothing and CA_OUT stays absent, so world_live skips.
# world_live and replication_live expect the default world (services/world/lua, tests/project/maps): a world started
# with extra compose files (e.g. samples/starter-mmo's override, whose Lua rejects characters without a class) is
# recreated from docker-compose.yml alone, and its compose files are written to RESTORE_OUT for
# world_live_teardown.cmake to put back.
# Usage: cmake -DWORLD_DIR=<services/world> -DCA_OUT=<pem> -DRESTORE_OUT=<file> -P world_live_setup.cmake
file(REMOVE "${CA_OUT}")
execute_process(COMMAND docker compose ps --status running --services WORKING_DIRECTORY "${WORLD_DIR}"
                RESULT_VARIABLE result OUTPUT_VARIABLE running ERROR_QUIET)
if(NOT result EQUAL 0 OR NOT running MATCHES "world" OR NOT running MATCHES "mysql")
  message(STATUS "services/world compose stack is not running: world_live will be skipped")
  return()
endif()
execute_process(COMMAND docker compose ps -q world WORKING_DIRECTORY "${WORLD_DIR}" OUTPUT_VARIABLE container
                OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND docker inspect --format "{{index .Config.Labels \"com.docker.compose.project.config_files\"}}"
                        "${container}"
                RESULT_VARIABLE result OUTPUT_VARIABLE config_files OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cannot read the world container's compose files")
endif()
if(config_files MATCHES ",") # docker-compose.yml plus overrides
  file(WRITE "${RESTORE_OUT}" "${config_files}")
  message(STATUS "world_live: the world runs with ${config_files}; recreating it with docker-compose.yml alone")
  execute_process(COMMAND docker compose up -d --force-recreate --wait world WORKING_DIRECTORY "${WORLD_DIR}"
                  RESULT_VARIABLE result TIMEOUT 600)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "could not recreate the default world server (${result})")
  endif()
endif()
execute_process(COMMAND docker compose cp world:/app/certs/server.crt "${CA_OUT}" WORKING_DIRECTORY "${WORLD_DIR}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cannot copy the world certificate out of the world container")
endif()
execute_process(COMMAND docker compose exec -T mysql sh -c [[mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "$MYSQL_DATABASE" -e "INSERT IGNORE INTO realms (name, host, port, region, status, sort_order, created_at, updated_at) VALUES ('Kast Live', '127.0.0.1', 7777, 'local', 'online', 99, NOW(3), NOW(3))"]]
                WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cannot add the 'Kast Live' realm: ${error}")
endif()
message(STATUS "world_live: CA in ${CA_OUT}, realm 'Kast Live' present")
