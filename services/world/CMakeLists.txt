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
find_package(quill CONFIG REQUIRED) # async logging backend, private to server/core/log.cpp
find_package(recastnavigation CONFIG REQUIRED) # navmesh: Recast in tools/mapbake, Detour in server/world/nav.cpp
find_package(Stb REQUIRED) # header-only: stb_image decodes 16-bit PNG heightmaps (map/heights.cpp)
# vcpkg's luajit port ships no CMake config: headers in include/luajit (Windows) or include/luajit-2.1 (Linux),
# library lua51 (Windows) or luajit-5.1 (Linux).
find_path(LUAJIT_INCLUDE_DIR luajit.h PATH_SUFFIXES luajit luajit-2.1 REQUIRED)
find_library(LUAJIT_LIBRARY NAMES lua51 luajit-5.1 REQUIRED)

# Shared with services/chat: kast_protocol and kast_service_core, and the warning flags (WORLD_* below).
# vcpkg headers are SYSTEM includes, so their warnings are suppressed.
include(service_core.cmake)
set(WORLD_COMPILE_OPTIONS ${KAST_SERVICE_COMPILE_OPTIONS})
set(WORLD_DEFINITIONS ${KAST_SERVICE_DEFINITIONS})

# --- map: map.json manifests, tile math and heightmaps; std + nlohmann_json + stb only, no file IO, shared with the engine ---
add_library(kast_map STATIC map/map.cpp map/heights.cpp map/objects.cpp)
target_include_directories(kast_map PUBLIC map)
target_include_directories(kast_map SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_compile_options(kast_map PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_map PUBLIC nlohmann_json::nlohmann_json)

# --- world_core: everything in the world server except main() and kast_service_core, so the self-check links the same code ---
add_library(world_core STATIC
  server/core/config.cpp
  server/core/tick_stats.cpp
  server/db/char_repo.cpp
  server/db/db_worker.cpp
  server/world/aoi_grid.cpp
  server/world/maps.cpp
  server/world/nav.cpp
  server/world/rules.cpp
  server/world/systems.cpp
  server/world/world.cpp
  server/lua/components.cpp
  server/lua/event_bus.cpp
  server/lua/lua_state_machines.cpp
  server/lua/items.cpp
  server/lua/npcs.cpp
  server/lua/runtime.cpp
  server/lua/sandbox.cpp
  server/lua/state_machines.cpp
  server/app/admin_port.cpp
  server/app/chat_link.cpp
  server/app/game_server.cpp
)
target_include_directories(world_core PUBLIC server)
target_compile_options(world_core PRIVATE ${WORLD_COMPILE_OPTIONS})
target_compile_definitions(world_core PUBLIC ${WORLD_DEFINITIONS})
target_link_libraries(world_core PUBLIC kast_service_core kast_map EnTT::EnTT
  PRIVATE RecastNavigation::Detour sol2::sol2 "${LUAJIT_LIBRARY}" ${CMAKE_DL_LIBS})
target_include_directories(world_core SYSTEM PRIVATE "${LUAJIT_INCLUDE_DIR}")
# As the engine: sol2 on the LuaJIT (Lua 5.1) API with argument checks; C++ exceptions in bindings are caught by
# sol2 and become Lua errors carrying the binding's message (with the script's file:line).
target_compile_definitions(world_core PRIVATE SOL_LUAJIT=1 SOL_ALL_SAFETIES_ON=1 SOL_EXCEPTIONS_SAFE_PROPAGATION=0
  SOL_PRINT_ERRORS=0)

add_executable(kast_world server/main.cpp)
target_compile_options(kast_world PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_world PRIVATE world_core)

# --- bot: headless test client (auth HTTP + TLS game protocol); never links server code (kast_map is shared) ---
add_library(kast_bot_lib STATIC bot/auth_client.cpp bot/world_client.cpp bot/harness.cpp bot/scenarios.cpp bot/load.cpp)
target_include_directories(kast_bot_lib PUBLIC bot)
target_compile_options(kast_bot_lib PRIVATE ${WORLD_COMPILE_OPTIONS})
target_compile_definitions(kast_bot_lib PUBLIC ${WORLD_DEFINITIONS})
target_link_libraries(kast_bot_lib PUBLIC kast_protocol kast_map asio::asio OpenSSL::SSL OpenSSL::Crypto jwt-cpp::jwt-cpp
  httplib::httplib nlohmann_json::nlohmann_json)
add_executable(kast_bot bot/main.cpp)
target_compile_options(kast_bot PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_bot PRIVATE kast_bot_lib)

# --- mapbake: offline navmesh bake + map lint (docs/services/world/maps.html#navmesh); the lib is pure, so the self-check bakes fixtures ---
add_library(kast_mapbake_lib STATIC tools/mapbake/bake.cpp)
target_include_directories(kast_mapbake_lib PUBLIC tools/mapbake)
target_compile_options(kast_mapbake_lib PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_mapbake_lib PUBLIC kast_map PRIVATE RecastNavigation::Recast RecastNavigation::Detour)
add_executable(kast_mapbake tools/mapbake/main.cpp)
target_compile_options(kast_mapbake PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(kast_mapbake PRIVATE kast_mapbake_lib)

enable_testing()
add_executable(world_self_check tests/self_check.cpp tests/lua_checks.cpp)
target_compile_options(world_self_check PRIVATE ${WORLD_COMPILE_OPTIONS})
target_link_libraries(world_self_check PRIVATE world_core kast_mapbake_lib)
# The sample lua/ folder, so the checks can run it (the NPC sample).
target_compile_definitions(world_self_check PRIVATE KAST_WORLD_LUA_SAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/lua")
# The sample maps (the area checks), when building inside the repo. The Docker build context is this folder only, so
# there they are left out; integration scenario 17 runs them against the live stack instead.
set(KAST_SAMPLE_MAPS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/project/maps")
if(EXISTS "${KAST_SAMPLE_MAPS}")
  target_compile_definitions(world_self_check PRIVATE KAST_SAMPLE_MAPS="${KAST_SAMPLE_MAPS}")
endif()
# The starter-mmo sample's Lua and maps (its combat check), likewise only inside the repo.
set(KAST_STARTER_MMO "${CMAKE_CURRENT_SOURCE_DIR}/../../samples/starter-mmo")
if(EXISTS "${KAST_STARTER_MMO}")
  target_compile_definitions(world_self_check PRIVATE KAST_STARTER_MMO="${KAST_STARTER_MMO}"
                             KAST_STARTER_MMO_TEST_LUA="${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/starter_mmo_test_server.lua")
endif()
add_test(NAME world_self_check COMMAND world_self_check)
# Caller-side logging cost vs a synchronous stdout write, and no lines lost under queue pressure.
add_test(NAME log_bench COMMAND world_self_check --log-bench)
# ns per aoi_system call: 500 / 2000 players, idle, 10% and 100% moving (printed, not checked).
add_test(NAME aoi_bench COMMAND world_self_check --aoi-bench)
# Lines logged right before a crash or a fatal startup error still reach stdout.
add_test(NAME log_flush COMMAND ${CMAKE_COMMAND} -DCHECK=$<TARGET_FILE:world_self_check> -DWORLD=$<TARGET_FILE:kast_world>
  -DOUT=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/log_flush.cmake)
# kast_mapbake's lint exits (tests/mapbake fixtures); in the repo also that the sample hills' stored navtiles are current.
set(MAPBAKE_HILLS "")
if(EXISTS "${KAST_SAMPLE_MAPS}")
  set(MAPBAKE_HILLS "${KAST_SAMPLE_MAPS}/hills")
endif()
add_test(NAME mapbake_cli COMMAND ${CMAKE_COMMAND} -DBAKE=$<TARGET_FILE:kast_mapbake>
  -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/mapbake -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mapbake_cli
  -DHILLS=${MAPBAKE_HILLS} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/mapbake_cli.cmake)
