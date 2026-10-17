# Prepares a running services/world compose stack for this sample: copies the world server's self-signed dev
# certificate out of its container to certs/world_dev_ca.pem (project.kast's caFile) and adds the realm
# "Starter Realm" (127.0.0.1:7777, online) to auth's database. Safe to run again: the realm name is unique, so a second
# run adds nothing. Start the stack with the sample's override first (README).
# Usage: cmake -P samples/starter-mmo/setup_realm.cmake
set(WORLD_DIR "${CMAKE_CURRENT_LIST_DIR}/../../services/world")
set(CA_OUT "${CMAKE_CURRENT_LIST_DIR}/certs/world_dev_ca.pem")
execute_process(COMMAND docker compose ps --status running --services WORKING_DIRECTORY "${WORLD_DIR}"
                RESULT_VARIABLE result OUTPUT_VARIABLE running ERROR_QUIET)
if(NOT result EQUAL 0 OR NOT running MATCHES "world" OR NOT running MATCHES "mysql")
  message(FATAL_ERROR "The services/world compose stack is not running: start it first (samples/starter-mmo/README.md)")
endif()
execute_process(COMMAND docker compose cp world:/app/certs/server.crt "${CA_OUT}" WORKING_DIRECTORY "${WORLD_DIR}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cannot copy the world certificate out of the world container")
endif()
execute_process(COMMAND docker compose exec -T mysql sh -c [[mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "$MYSQL_DATABASE" -e "INSERT IGNORE INTO realms (name, host, port, region, status, sort_order, created_at, updated_at) VALUES ('Starter Realm', '127.0.0.1', 7777, 'local', 'online', 10, NOW(3), NOW(3))"]]
                WORKING_DIRECTORY "${WORLD_DIR}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cannot add the 'Starter Realm' realm: ${error}")
endif()
message(STATUS "Starter MMO: CA in certs/world_dev_ca.pem, realm 'Starter Realm' present")
