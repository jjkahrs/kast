cmake_minimum_required(VERSION 3.25)
project(kast_world LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static vcpkg triplets on Windows use the static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-libmysql CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
# vcpkg's luajit port ships no CMake config: headers in include/luajit (Windows) or include/luajit-2.1 (Linux),
# library lua51 (Windows) or luajit-5.1 (Linux).
find_path(LUAJIT_INCLUDE_DIR luajit.h PATH_SUFFIXES luajit luajit-2.1 REQUIRED)
find_library(LUAJIT_LIBRARY NAMES lua51 luajit-5.1 REQUIRED)

# World code only; vcpkg headers are SYSTEM includes, so their warnings are suppressed.
if(MSVC)
  set(WORLD_COMPILE_OPTIONS /W4 /WX /permissive- /utf-8 /bigobj)
  set(WORLD_DEFINITIONS _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  set(WORLD_COMPILE_OPTIONS -Wall -Wextra -Werror)
  set(WORLD_DEFINITIONS)
endif()
list(APPEND WORLD_DEFINITIONS ASIO_NO_DEPRECATED CPPHTTPLIB_OPENSSL_SUPPORT JWT_DISABLE_PICOJSON)

# --- protocol: frame format + messages; no Asio/DB/ECS, reusable by a game client ---
add_library(kast_protocol STATIC protocol/codec.cpp)
target_include_directories(kast_protocol PUBLIC protocol)
target_compile_options(kast_protocol PRIVATE ${WORLD_COMPILE_OPTIONS})

# --- world_core: everything in the server except main(), so the self-check links the same code ---
add_library(world_core STATIC
  server/core/config.cpp
  server/core/log.cpp
  server/core/tick_stats.cpp
  server/core/utc.cpp
  server/db/char_repo.cpp
  server/db/db_worker.cpp
  server/db/migrate.cpp
  server/db/mysql.cpp
  server/auth/jwks_cache.cpp
  server/auth/jwt_verify.cpp
  server/auth/token_service.cpp
  server/net/session.cpp
  server/net/tls_context.cpp
  server/world/aoi_grid.cpp
  server/world/rules.cpp
  server/world/systems.cpp
  server/world/world.cpp
  server/lua/components.cpp
  server/lua/event_bus.cpp
  server/lua/runtime.cpp
  server/lua/sandbox.cpp
  server/app/game_server.cpp
)
target_include_directories(world_core PUBLIC server)
target_compile_options(world_core PRIVATE ${WORLD_COMPILE_OPTIONS})
target_compile_definitions(world_core PUBLIC ${WORLD_DEFINITIONS})
target_link_libraries(world_core PUBLIC kast_protocol asio::asio OpenSSL::SSL OpenSSL::Crypto
  unofficial::libmysql::libmysql jwt-cpp::jwt-cpp httplib::httplib EnTT::EnTT nlohmann_json::nlohmann_json
  PRIVATE sol2::sol2 "${LUAJIT_LIBRARY}" ${CMAKE_DL_LIBS})
target_include_directories(world_core SYSTEM PRIVATE "${LUAJIT_INCLUDE_DIR}")
# As the engine: sol2 on the LuaJIT (Lua 5.1) API with argument checks; C++ exceptions in bindings are caught by
# sol2 and become Lua errors carrying the binding's message (with the script's file:line).
target_compile_definitions(world_core PRIVATE SOL_LUAJIT=1 SOL_ALL_SAFETIES_ON=1 SOL_EXCEPTIONS_SAFE_PROPAGATION=0
  SOL_PRINT_ERRORS=0)

add_executable(kast_world server/main.cpp)
target_compile_options(kast_world PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_world PRIVATE world_core)

# --- bot: headless test client (auth HTTP + TLS game protocol); never links server code ---
add_library(kast_bot_lib STATIC bot/auth_client.cpp bot/world_client.cpp bot/harness.cpp bot/scenarios.cpp bot/load.cpp)
target_include_directories(kast_bot_lib PUBLIC bot)
target_compile_options(kast_bot_lib PRIVATE ${WORLD_COMPILE_OPTIONS})
target_compile_definitions(kast_bot_lib PUBLIC ${WORLD_DEFINITIONS})
target_link_libraries(kast_bot_lib PUBLIC kast_protocol asio::asio OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp
  httplib::httplib nlohmann_json::nlohmann_json)
add_executable(kast_bot bot/main.cpp)
target_compile_options(kast_bot PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_bot PRIVATE kast_bot_lib)

enable_testing()
add_executable(world_self_check tests/self_check.cpp tests/lua_checks.cpp)
target_compile_options(world_self_check PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(world_self_check PRIVATE world_core)
add_test(NAME world_self_check COMMAND world_self_check)
