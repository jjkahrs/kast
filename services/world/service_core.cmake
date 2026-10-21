# The code every Kast C++ service shares, included by services/world/CMakeLists.txt and services/chat/CMakeLists.txt:
#   kast_protocol      frame format + messages (protocol/); no Asio/DB, reusable by a game client
#   kast_service_core  config/log helpers, chat text rules and mutes, TLS sessions, JWT/JWKS verification, MySQL and
#                      the migration runner (server/core, server/net, server/auth, server/db/mysql + migrate)
# The including project finds the packages first (asio, OpenSSL, unofficial-libmysql, jwt-cpp, httplib, nlohmann_json,
# quill); KAST_SERVICE_COMPILE_OPTIONS / _DEFINITIONS are set here for its own targets too.
set(KAST_SERVICE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(MSVC)
  set(KAST_SERVICE_COMPILE_OPTIONS /W4 /WX /permissive- /utf-8 /bigobj)
  set(KAST_SERVICE_DEFINITIONS _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  set(KAST_SERVICE_COMPILE_OPTIONS -Wall -Wextra -Werror)
  set(KAST_SERVICE_DEFINITIONS)
endif()
list(APPEND KAST_SERVICE_DEFINITIONS ASIO_NO_DEPRECATED CPPHTTPLIB_OPENSSL_SUPPORT JWT_DISABLE_PICOJSON)

add_library(kast_protocol STATIC "${KAST_SERVICE_DIR}/protocol/codec.cpp")
target_include_directories(kast_protocol PUBLIC "${KAST_SERVICE_DIR}/protocol")
target_compile_options(kast_protocol PRIVATE ${KAST_SERVICE_COMPILE_OPTIONS})

add_library(kast_service_core STATIC
  "${KAST_SERVICE_DIR}/server/core/chat_rules.cpp"
  "${KAST_SERVICE_DIR}/server/core/env.cpp"
  "${KAST_SERVICE_DIR}/server/core/log.cpp"
  "${KAST_SERVICE_DIR}/server/core/utc.cpp"
  "${KAST_SERVICE_DIR}/server/db/migrate.cpp"
  "${KAST_SERVICE_DIR}/server/db/mysql.cpp"
  "${KAST_SERVICE_DIR}/server/auth/jwks_cache.cpp"
  "${KAST_SERVICE_DIR}/server/auth/jwt_verify.cpp"
  "${KAST_SERVICE_DIR}/server/auth/token_service.cpp"
  "${KAST_SERVICE_DIR}/server/net/session.cpp"
  "${KAST_SERVICE_DIR}/server/net/tls_context.cpp"
)
# Included as <core/log.h>, <net/session.h>, ...
target_include_directories(kast_service_core PUBLIC "${KAST_SERVICE_DIR}/server")
target_compile_options(kast_service_core PRIVATE ${KAST_SERVICE_COMPILE_OPTIONS})
target_compile_definitions(kast_service_core PUBLIC ${KAST_SERVICE_DEFINITIONS})
target_link_libraries(kast_service_core PUBLIC kast_protocol asio::asio OpenSSL::SSL OpenSSL::Crypto
  unofficial::libmysql::libmysql jwt-cpp::jwt-cpp httplib::httplib nlohmann_json::nlohmann_json PRIVATE quill::quill)
