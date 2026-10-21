cmake_minimum_required(VERSION 3.25)
project(kast_chat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static vcpkg triplets on Windows use the static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-libmysql CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(quill CONFIG REQUIRED)

# The world server's protocol (kast_protocol) and the code the services share (kast_service_core): compiled from
# services/world's sources, never copied.
set(KAST_WORLD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../world" CACHE PATH "services/world (protocol/ and server/)")
include("${KAST_WORLD_DIR}/service_core.cmake")

# --- chat_core: everything but main(), so the self-check links the same code ---
add_library(chat_core STATIC server/chat_router.cpp server/chat_server.cpp server/config.cpp server/mute_store.cpp)
target_include_directories(chat_core PUBLIC server)
target_compile_options(chat_core PRIVATE ${KAST_SERVICE_COMPILE_OPTIONS})
target_link_libraries(chat_core PUBLIC kast_service_core)

add_executable(kast_chat server/main.cpp)
target_compile_options(kast_chat PRIVATE ${KAST_SERVICE_COMPILE_OPTIONS})
target_link_libraries(kast_chat PRIVATE chat_core)

enable_testing()
add_executable(chat_self_check tests/self_check.cpp)
target_compile_options(chat_self_check PRIVATE ${KAST_SERVICE_COMPILE_OPTIONS})
target_link_libraries(chat_self_check PRIVATE chat_core)
add_test(NAME chat_self_check COMMAND chat_self_check)
