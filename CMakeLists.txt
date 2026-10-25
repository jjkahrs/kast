cmake_minimum_required(VERSION 3.25)
project(kast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED) # the Shader Graph canvas: editor only
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED) # model import: glTF/GLB, FBX, OBJ
find_package(meshoptimizer CONFIG REQUIRED) # generated LODs: model import (meshopt_simplifyWithAttributes), lightmapped levels
find_package(Jolt CONFIG REQUIRED) # physics: rigid bodies, character controller (see engine/physics.h)
find_package(Stb REQUIRED) # header-only: stb_image (PNG/JPG decode), stb_image_resize2 (mips)
# vcpkg's luajit port ships no CMake config, only headers in include/luajit and lua51.lib/.dll.
find_path(LUAJIT_INCLUDE_DIR luajit.h PATH_SUFFIXES luajit REQUIRED)
find_library(LUAJIT_LIBRARY lua51 REQUIRED)
# vcpkg's miniaudio port is header-only (miniaudio.h, no CMake config); engine/audio.cpp holds its implementation.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
# networking (see engine/network.h): TCP/TLS, WebSocket and HTTP clients on Boost.Asio + Beast, TLS on OpenSSL
find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Kast code only; vcpkg headers are SYSTEM includes, so their warnings are suppressed. /utf-8: sources and string
# literals are UTF-8.
set(KAST_COMPILE_OPTIONS /W4 /WX /permissive- /utf-8)
# Added to every exe: makes the process code page UTF-8 (see the manifest).
set(KAST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest")

# --- shaders: compiled to DX11 bytecode headers with vcpkg's shaderc; each dir has its own varying.def.sc. vs_* are
# vertex, cs_* compute, the rest fragment shaders ---
# ponytail: host triplet == target triplet (x64-windows); look in the host triplet dir if cross-compiling ever happens.
find_program(KAST_SHADERC shaderc PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx" NO_DEFAULT_PATH REQUIRED)
set(KAST_SHADER_INCLUDE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/bgfx")
set(KAST_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(KAST_EDITOR_SHADERS)
set(KAST_ENGINE_SHADERS)
foreach(path editor/shaders/vs_imgui editor/shaders/fs_imgui
             engine/shaders/vs_mesh engine/shaders/vs_mesh_skinned engine/shaders/fs_mesh engine/shaders/vs_line
             engine/shaders/vs_mesh_instanced engine/shaders/vs_shadow engine/shaders/vs_shadow_skinned
             engine/shaders/vs_shadow_instanced engine/shaders/fs_shadow
             engine/shaders/fs_line engine/shaders/vs_sky engine/shaders/fs_sky
             engine/shaders/cs_particle_sim engine/shaders/vs_particle engine/shaders/fs_particle engine/shaders/cs_ibl
             engine/shaders/cs_voxelgi_inject engine/shaders/cs_voxelgi_mip engine/shaders/cs_voxelgi_bounce
             engine/shaders/cs_sdfgi_build engine/shaders/cs_sdfgi_light engine/shaders/cs_sdfgi_probe
             engine/shaders/cs_hiz_build engine/shaders/cs_hiz_test engine/shaders/cs_hiz_debug
             engine/shaders/cs_volfog_inject engine/shaders/cs_volfog_scatter engine/shaders/cs_volfog_integrate
             engine/shaders/gui/vs_gui engine/shaders/gui/fs_gui engine/shaders/gui/fs_gui_matrix
             engine/shaders/gui/fs_gui_blur engine/shaders/gui/fs_gui_gradient
             engine/shaders/post/vs_post engine/shaders/post/fs_post_bright engine/shaders/post/fs_post_down
             engine/shaders/post/fs_post_up engine/shaders/post/fs_post_final engine/shaders/post/fs_post_fxaa
             engine/shaders/post/fs_ssao_gtao engine/shaders/post/fs_ssao_blur engine/shaders/post/fs_ssao_composite
             engine/shaders/post/fs_ssr_trace engine/shaders/post/fs_ssr_composite
             engine/shaders/post/fs_dof_setup engine/shaders/post/fs_dof_gather engine/shaders/post/fs_dof_composite
             engine/shaders/post/fs_volfog_composite)
  get_filename_component(shader ${path} NAME)
  get_filename_component(dir ${path} DIRECTORY)
  set(varying "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/varying.def.sc")
  string(SUBSTRING ${shader} 0 2 stage)
  if(stage STREQUAL "vs")
    set(type vertex)
  elseif(stage STREQUAL "cs")
    set(type compute)
  else()
    set(type fragment)
  endif()
  set(src "${CMAKE_CURRENT_SOURCE_DIR}/${path}.sc")
  set(out "${KAST_SHADER_OUT}/${shader}.bin.h")
  file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.sh") # e.g. engine/shaders/mesh.sh, included from the dir
  list(APPEND headers "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/volumetric_fog.sh") # post/fs_volfog_composite.sc's too
  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAST_SHADER_OUT}"
    COMMAND "${KAST_SHADERC}" -f "${src}" -o "${out}" --bin2c ${shader} --type ${type}
            --platform windows -p s_5_0 -i "${KAST_SHADER_INCLUDE}" -i "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
            --varyingdef "${varying}"
    DEPENDS "${src}" "${varying}" ${headers}
    COMMENT "shaderc ${shader}.sc"
    VERBATIM)
  if(dir MATCHES "^engine/")
    list(APPEND KAST_ENGINE_SHADERS "${out}")
  else()
    list(APPEND KAST_EDITOR_SHADERS "${out}")
  endif()
endforeach()
# fs_mesh's SDFGI variant (docs/engine/sdfgi.html): the same source with KAST_SDFGI, for the draws that take SDFGI, so
# every other draw's shader compiles exactly as before.
file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/*.sh")
add_custom_command(
  OUTPUT "${KAST_SHADER_OUT}/fs_mesh_sdfgi.bin.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAST_SHADER_OUT}"
  COMMAND "${KAST_SHADERC}" -f "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/fs_mesh.sc" -o "${KAST_SHADER_OUT}/fs_mesh_sdfgi.bin.h"
          --bin2c fs_mesh_sdfgi --type fragment --platform windows -p s_5_0 -i "${KAST_SHADER_INCLUDE}"
          -i "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders" --varyingdef "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/varying.def.sc"
          --define KAST_SDFGI
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/fs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/varying.def.sc" ${headers}
  COMMENT "shaderc fs_mesh.sc (KAST_SDFGI)"
  VERBATIM)
list(APPEND KAST_ENGINE_SHADERS "${KAST_SHADER_OUT}/fs_mesh_sdfgi.bin.h")

# --- shader tools: .kshader files compile in the editor at run time (editor/shader_compiler.cpp) with this shaderc,
# next to the editor, plus bgfx's shader headers and the mesh templates in kshader/ beside it. Images are compressed
# with texturec (editor/texture_cooker.cpp, with the DLLs it loads) next to it too. The player gets none. ---
find_program(KAST_TEXTUREC texturec PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx" NO_DEFAULT_PATH REQUIRED)
set(KAST_SHADER_TOOLS)
get_filename_component(KAST_BGFX_TOOLS "${KAST_TEXTUREC}" DIRECTORY)
foreach(pair "${KAST_SHADERC}|shaderc.exe" "${KAST_TEXTUREC}|texturec.exe" "${KAST_BGFX_TOOLS}/miniz.dll|miniz.dll"
             "${KAST_BGFX_TOOLS}/squish.dll|squish.dll"
             "${KAST_SHADER_INCLUDE}/bgfx_shader.sh|kshader/include/bgfx_shader.sh"
             "${KAST_SHADER_INCLUDE}/bgfx_compute.sh|kshader/include/bgfx_compute.sh"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/fs_mesh.sc|kshader/mesh/fs_mesh.sc"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/vs_mesh.sc|kshader/mesh/vs_mesh.sc"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/vs_mesh_skinned.sc|kshader/mesh/vs_mesh_skinned.sc"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/vs_mesh_instanced.sc|kshader/mesh/vs_mesh_instanced.sc"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/mesh.sh|kshader/mesh/mesh.sh"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/ltc_table.sh|kshader/mesh/ltc_table.sh"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/volumetric_fog.sh|kshader/mesh/volumetric_fog.sh"
             "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/varying.def.sc|kshader/mesh/varying.def.sc")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 from)
  list(GET pair 1 to)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${to}"
                     COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${from}" "${CMAKE_CURRENT_BINARY_DIR}/${to}"
                     DEPENDS "${from}" VERBATIM)
  list(APPEND KAST_SHADER_TOOLS "${CMAKE_CURRENT_BINARY_DIR}/${to}")
endforeach()
add_custom_target(kast_shader_tools DEPENDS ${KAST_SHADER_TOOLS})

# --- Fonts: TTFs embedded as byte array headers, regenerated when the TTF changes ---
set(KAST_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(kast_embed_font font header array)
  set(path "${CMAKE_CURRENT_SOURCE_DIR}/editor/assets/fonts/${font}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
  file(READ "${path}" font_hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," font_bytes "${font_hex}")
  string(REGEX REPLACE "((0x..,){32})" "\\1\n" font_bytes "${font_bytes}")
  file(CONFIGURE OUTPUT "${KAST_GENERATED}/${header}" @ONLY CONTENT
"#pragma once
// Generated by CMakeLists.txt from editor/assets/fonts/@font@. Do not edit.
inline unsigned char @array@[] = {
@font_bytes@
};
")
endfunction()
# The editor UI font and the game GUI fallback face.
kast_embed_font(Roboto-Regular.ttf roboto_regular.h kRobotoRegularTtf)
# The game GUI's real bold / italic faces of Roboto (rich text [b] / [i], font-weight / font-style).
kast_embed_font(Roboto-Bold.ttf roboto_bold.h kRobotoBoldTtf)
kast_embed_font(Roboto-Italic.ttf roboto_italic.h kRobotoItalicTtf)
kast_embed_font(Roboto-BoldItalic.ttf roboto_bold_italic.h kRobotoBoldItalicTtf)
# The editor's Play / Pause / Stop icons: Google Fonts' static subset of Material Symbols Outlined (FILL 1, wght 400,
# opsz 24) with only play_arrow, pause and stop (css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@24,400,1,0
# &icon_names=pause,play_arrow,stop).
kast_embed_font(MaterialSymbolsOutlined-Filled.ttf material_symbols.h kMaterialSymbolsTtf)

# --- protocol: the world server's frame codec (services/world/protocol), compiled from its sources unchanged; it needs
# only the standard library, none of the server's vcpkg deps ---
add_library(kast_protocol STATIC services/world/protocol/codec.cpp)
target_include_directories(kast_protocol PUBLIC services/world/protocol)
target_compile_options(kast_protocol PRIVATE ${KAST_COMPILE_OPTIONS})

# --- map: the world server's map manifests, tile math and heightmaps (services/world/map), compiled from its sources
# unchanged; std + nlohmann_json + stb only ---
add_library(kast_map STATIC services/world/map/map.cpp services/world/map/heights.cpp services/world/map/objects.cpp)
target_include_directories(kast_map PUBLIC services/world/map)
target_include_directories(kast_map SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_compile_options(kast_map PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_map PUBLIC nlohmann_json::nlohmann_json)

# --- xatlas: lightmap UV unwrapping for Bake Lightmaps (engine/lightmap_bake.cpp). Vendored (third_party/xatlas,
# MIT, jpcy/xatlas at f700c77): the pinned vcpkg baseline has no port. Its own warnings stay its own. ---
add_library(xatlas STATIC third_party/xatlas/xatlas.cpp)
target_include_directories(xatlas SYSTEM PUBLIC third_party/xatlas)
target_compile_options(xatlas PRIVATE /W0 /utf-8)

# --- engine: reusable by the shipped client, no ImGui/editor code ---
add_library(kast_engine STATIC
  engine/app.cpp engine/undo.cpp engine/crash_dialogs.cpp engine/log.cpp engine/math3d.cpp engine/scene.cpp engine/scene_io.cpp engine/prefab.cpp engine/components.cpp
  engine/systems.cpp engine/renderer.cpp engine/shader.cpp engine/shader_graph.cpp engine/shadow.cpp engine/frustum.cpp engine/lod.cpp engine/occlusion.cpp engine/material.cpp engine/input.cpp engine/lua_systems.cpp engine/lua_chat_api.cpp engine/lua_world_api.cpp engine/project.cpp
  engine/state_machines.cpp engine/lua_state_machines.cpp engine/post_process.cpp
  engine/lua_sandbox.cpp engine/lua_components.cpp engine/lua_items.cpp engine/pak.cpp engine/files.cpp engine/play.cpp engine/event_bus.cpp
  engine/gui.cpp engine/gui_render.cpp engine/gui_effects.cpp engine/gui_events.cpp engine/gui_models.cpp engine/gui_widgets.cpp engine/gui_lists.cpp engine/gui_richtext.cpp engine/gui_prefabs.cpp engine/gui_scale.cpp engine/scene_gui.cpp engine/window_display.cpp engine/model.cpp engine/animation.cpp engine/physics.cpp
  engine/audio.cpp engine/network.cpp engine/auth_client.cpp engine/chat_client.cpp engine/world_client.cpp engine/stats_overlay.cpp engine/replication.cpp engine/terrain.cpp engine/sky.cpp engine/ibl.cpp engine/bvh.cpp engine/lightmap.cpp engine/lightmap_bake.cpp engine/voxel_gi.cpp engine/sdfgi.cpp engine/volumetric_fog.cpp engine/particles.cpp engine/texture_cook.cpp ${KAST_ENGINE_SHADERS})
target_include_directories(kast_engine PUBLIC engine PRIVATE "${KAST_SHADER_OUT}" "${KAST_GENERATED}")
target_include_directories(kast_engine SYSTEM PRIVATE "${LUAJIT_INCLUDE_DIR}" "${Stb_INCLUDE_DIR}" "${MINIAUDIO_INCLUDE_DIR}")
# sol2 on the LuaJIT (Lua 5.1) API; safeties check argument types instead of trusting scripts. sol2 lets C++
# exceptions unwind through LuaJIT by default, which reports them only as "C++ exception"; catching them in sol2
# keeps the binding's error message (with the script's file:line). The message reaches the Console, not sol2's stdout print.
target_compile_definitions(kast_engine PRIVATE SOL_LUAJIT=1 SOL_ALL_SAFETIES_ON=1 SOL_EXCEPTIONS_SAFE_PROPAGATION=0 SOL_PRINT_ERRORS=0)
target_compile_options(kast_engine PRIVATE ${KAST_COMPILE_OPTIONS})
# Asio targets Windows 10 (and its templates need /bigobj); crypt32 reads the ROOT certificate store.
set_source_files_properties(engine/network.cpp PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00" COMPILE_OPTIONS /bigobj)
set_source_files_properties(engine/lua_systems.cpp PROPERTIES COMPILE_OPTIONS /bigobj) # sol2 templates
target_link_libraries(kast_engine
  PUBLIC bgfx::bgfx bgfx::bx EnTT::EnTT nlohmann_json::nlohmann_json RmlUi::RmlUi pugixml::pugixml kast_map
  PRIVATE SDL3::SDL3 bgfx::bimg sol2::sol2 Jolt::Jolt "${LUAJIT_LIBRARY}" kast_protocol xatlas meshoptimizer::meshoptimizer
          Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto crypt32 shell32) # shell32: recycleFile

# --- model import: the Assimp importer (editor/model_importer.h) the editor injects into kast::models(); only the editor
# and the tests link it, so the shipped kast_player has no Assimp and loads the models Build cooked into game.pak ---
add_library(kast_model_import STATIC editor/model_importer.cpp)
target_compile_options(kast_model_import PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_model_import PUBLIC kast_engine PRIVATE assimp::assimp meshoptimizer::meshoptimizer)

# --- player: the prebuilt game client the editor's Build menu copies; engine only, no ImGui ---
# A Windows-subsystem app (no console window); SDL_main.h supplies WinMain.
add_executable(kast_player WIN32 player/main.cpp "${KAST_MANIFEST}")
target_compile_options(kast_player PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_player PRIVATE kast_engine SDL3::SDL3)

# --- editor ---
add_executable(kast_editor
  editor/main.cpp editor/imgui_bgfx.cpp editor/document.cpp editor/scene_undo.cpp editor/gui_undo.cpp editor/build.cpp editor/camera.cpp editor/viewport.cpp
  editor/game_view.cpp editor/panels.cpp editor/lightmap_job.cpp editor/voxelgi_job.cpp editor/launcher.cpp editor/recent_projects.cpp editor/ui_common.cpp
  editor/assets.cpp editor/asset_ids.cpp editor/gui_doc.cpp editor/gui_builder.cpp editor/shader_graph_doc.cpp editor/shader_graph_editor.cpp editor/shader_preview.cpp editor/model_preview.cpp editor/external_editor.cpp editor/editor_settings.cpp
  editor/shader_compiler.cpp editor/texture_cooker.cpp editor/online.cpp editor/online_panel.cpp editor/zone_session.cpp editor/zone_render.cpp editor/zone_panel.cpp editor/zone_terrain.cpp editor/zone_objects.cpp editor/zone_cli.cpp
  editor/kast_editor.rc
  "${KAST_MANIFEST}" ${KAST_EDITOR_SHADERS})
target_include_directories(kast_editor PRIVATE "${KAST_SHADER_OUT}" "${KAST_GENERATED}")
# The Online panel's default compose folder: services/world of this source tree, if it is still there at run time.
target_compile_definitions(kast_editor PRIVATE KAST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(kast_editor PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_editor PRIVATE kast_engine kast_model_import kast_zone SDL3::SDL3 imgui::imgui imguizmo::imguizmo
                      unofficial::imgui-node-editor::imgui-node-editor pugixml::pugixml shell32) # shell32: Recycle Bin
add_dependencies(kast_editor kast_player kast_shader_tools) # Build copies kast_player.exe from next to kast_editor.exe

# --- zone: the zone (map) document, file and terrain editing logic (engine/zone), no window. Its own library, not
# part of kast_engine: only editing tools link it, so the shipped kast_player stays free of it (and of KAST_SOURCE_DIR). ---
add_library(kast_zone STATIC
  engine/zone/zone_doc.cpp engine/zone/zone_io.cpp engine/zone/terrain_edit.cpp engine/zone/zone_edit.cpp
  engine/zone/process_runner.cpp)
target_compile_options(kast_zone PRIVATE ${KAST_COMPILE_OPTIONS})
target_include_directories(kast_zone SYSTEM PRIVATE "${Stb_INCLUDE_DIR}") # stb_perlin, stb_image_write
target_link_libraries(kast_zone PUBLIC kast_engine kast_map)
# Bake Navmesh's default kast_mapbake: services/world/build of this source tree, if it is still there at run time.
target_compile_definitions(kast_zone PRIVATE KAST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(kast_zone_self_check tests/zone_self_check.cpp "${KAST_MANIFEST}")
target_compile_definitions(kast_zone_self_check PRIVATE KAST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(kast_zone_self_check PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_zone_self_check PRIVATE kast_zone)
target_include_directories(kast_zone_self_check SYSTEM PRIVATE "${Stb_INCLUDE_DIR}") # stb_image_write: 8-bit PNGs to reject

# --- LTC fit: the offline tool that writes engine/shaders/ltc_table.sh, the area light lookup table (tools/ltc_fit/README.md).
# Built, never run by the build: the table is checked in. ---
add_executable(kast_ltc_fit tools/ltc_fit/ltc_fit.cpp "${KAST_MANIFEST}")
target_compile_options(kast_ltc_fit PRIVATE ${KAST_COMPILE_OPTIONS})

# --- engine self-check: assert-style checks of math and hierarchy logic, no window ---
# Also builds the editor Document (Play mode) so it can be checked without a window.
add_executable(kast_self_check tests/self_check.cpp tests/lightmap_self_check.cpp tests/voxelgi_self_check.cpp tests/sdfgi_self_check.cpp tests/net_self_check.cpp tests/net_test_util.cpp editor/document.cpp editor/scene_undo.cpp editor/gui_undo.cpp editor/build.cpp editor/camera.cpp
  editor/recent_projects.cpp editor/assets.cpp editor/asset_ids.cpp editor/gui_doc.cpp editor/shader_graph_doc.cpp editor/external_editor.cpp
  editor/editor_settings.cpp editor/shader_compiler.cpp editor/texture_cooker.cpp editor/online.cpp "${KAST_MANIFEST}")
target_include_directories(kast_self_check PRIVATE editor)
add_dependencies(kast_self_check kast_shader_tools) # the .kshader checks compile with them
target_compile_definitions(kast_self_check PRIVATE KAST_TEST_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/project"
                           KAST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(kast_self_check PRIVATE ${KAST_COMPILE_OPTIONS} /bigobj) # one big file of checks
target_link_libraries(kast_self_check PRIVATE kast_engine kast_model_import kast_protocol SDL3::SDL3 imgui::imgui pugixml::pugixml shell32
                      Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto) # Boost, OpenSSL: the loopback net servers
set_source_files_properties(tests/net_self_check.cpp tests/net_test_util.cpp tests/net_test_server.cpp
                            PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00" COMPILE_OPTIONS /bigobj)

# --- net test server: the fixed-port loopback servers the tests/project net scenes talk to (see tests/net_test_server.cpp) ---
add_executable(kast_net_test_server tests/net_test_server.cpp tests/net_test_util.cpp "${KAST_MANIFEST}")
target_compile_options(kast_net_test_server PRIVATE ${KAST_COMPILE_OPTIONS})
target_link_libraries(kast_net_test_server PRIVATE kast_protocol Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)

# --- focus probe: runs a command and fails if its window takes the foreground (see tests/focus_probe.cpp) ---
add_executable(kast_focus_probe tests/focus_probe.cpp)
target_compile_options(kast_focus_probe PRIVATE ${KAST_COMPILE_OPTIONS})

# --- tests ---
enable_testing()
set(KAST_TEST_SCENES "${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes")
set(KAST_TEST_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/tests/project")
add_test(NAME engine_self_check COMMAND kast_self_check)
add_test(NAME zone_self_check COMMAND kast_zone_self_check)
# The checked-in area light table is what tools/ltc_fit fits (regenerate it after changing the fit or the BRDF).
add_test(NAME ltc_table_up_to_date
  COMMAND kast_ltc_fit --verify "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/ltc_table.sh")
# kast.auth against the real auth service in the services/world compose stack (cd services/world && docker compose
# up -d --build); skipped, not failed, when it isn't running.
add_test(NAME auth_live COMMAND kast_self_check --live-auth http://localhost:3100 http://localhost:8026)
set_tests_properties(auth_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# kast.world against the same stack's world server: world_live_setup copies its dev certificate out of the container and
# adds the realm "Kast Live" (127.0.0.1:7777) to auth's database; world_live then plays two accounts through it (about
# 90 s: it idles 85 s, past the server's 60 s idle timeout plus one 20 s heartbeat). Both skip when the stack isn't running.
add_test(NAME world_live_setup
  COMMAND "${CMAKE_COMMAND}" "-DWORLD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/services/world"
          "-DCA_OUT=${CMAKE_CURRENT_BINARY_DIR}/world_live_ca.pem"
          "-DRESTORE_OUT=${CMAKE_CURRENT_BINARY_DIR}/world_live_restore.txt"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/world_live_setup.cmake")
set_tests_properties(world_live_setup PROPERTIES FIXTURES_SETUP world_live_ready TIMEOUT 660)
# Puts back the compose overrides the setup took off the world server (e.g. samples/starter-mmo's), if any.
add_test(NAME world_live_teardown
  COMMAND "${CMAKE_COMMAND}" "-DWORLD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/services/world"
          "-DRESTORE_OUT=${CMAKE_CURRENT_BINARY_DIR}/world_live_restore.txt"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/world_live_teardown.cmake")
set_tests_properties(world_live_teardown PROPERTIES FIXTURES_CLEANUP world_live_ready TIMEOUT 600)
add_test(NAME world_live COMMAND kast_self_check --live-world http://localhost:3100 http://localhost:8026
                                 "${CMAKE_CURRENT_BINARY_DIR}/world_live_ca.pem" "Kast Live")
set_tests_properties(world_live PROPERTIES FIXTURES_REQUIRED world_live_ready SKIP_RETURN_CODE 77 TIMEOUT 240)
# Replication against the same stack: two players see each other cloned, smooth remote motion, a corrected teleport.
add_test(NAME replication_live COMMAND kast_self_check --live-replication http://localhost:3100 http://localhost:8026
                                       "${CMAKE_CURRENT_BINARY_DIR}/world_live_ca.pem" "Kast Live")
set_tests_properties(replication_live PROPERTIES FIXTURES_REQUIRED world_live_ready SKIP_RETURN_CODE 77 TIMEOUT 120)
# The editor's Online panel model against the same stack (docs/editor/online-tooling.html): starts it through
# the panel's Start if it isn't running (and stops it again after), health, an admin login, realm create/delete, Play
# started logged in, no secrets on disk. Skipped without docker.
add_test(NAME online_live COMMAND kast_self_check --live-online http://localhost:3100 http://localhost:8026
                                  "${CMAKE_CURRENT_SOURCE_DIR}/services/world")
set_tests_properties(online_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200 RUN_SERIAL TRUE)
add_test(NAME editor_smoke
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/smoke.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_smoke_scaled
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/smoke_scaled.png"
          "-DEXTRA_ARGS=--ui-scale 2.0" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME scene_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/scene_basic.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME scene_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/scene_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME scene_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/scene_basic.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/scene_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
# LOD and occlusion benchmark (tests/scenes/gen_lod_occlusion.py, docs/engine/lod-and-occlusion.html#benchmark): the
# far-field view draws the tori at LOD3 (--log-fps prints the Scene view's counts once a second); behind the near wall
# Hi-Z occlusion skips most of them; forced unsupported (as on a GPU without compute / read-back) it skips none and
# logs no error.
add_test(NAME lod_far_field
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lod_far_field.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lod_occlusion.scene --log-fps --window-pos -1900,40" -DFRAMES=2000
          "-DEXPECT_OUTPUT=Scene view: Tris [0-9.]+k  Draws [0-9]+  Occluded [0-9]+  LOD [0-9]+/0/0/[0-9][0-9][0-9]"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME occlusion_behind_wall
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/occlusion_behind_wall.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lod_occlusion_wall.scene --log-fps --window-pos -1900,40" -DFRAMES=2000
          "-DEXPECT_OUTPUT=Occluded [3-9][0-9][0-9]  " "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME occlusion_unsupported
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/occlusion_unsupported.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lod_occlusion_wall.scene --occlusion-unsupported --log-fps --window-pos -1900,40"
          -DFRAMES=2000 "-DEXPECT_OUTPUT=Occlusion culling: off \\(--occlusion-unsupported\\).*Occluded 0  "
          "-DFORBID_OUTPUT=\\[error\\]|Occluded [1-9]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Occlusion on and off draw the same still image (the wall hides what it skips).
foreach(mode on off)
  set(flag "")
  if(mode STREQUAL "off")
    set(flag "--no-occlusion")
  endif()
  add_test(NAME occlusion_${mode}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/occlusion_${mode}.png"
            "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lod_occlusion_wall.scene ${flag} --window-pos -1900,40" -DFRAMES=300
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME occlusion_same_image
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/occlusion_on.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/occlusion_off.png"
          -DSAME=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(occlusion_same_image PROPERTIES DEPENDS "occlusion_on_screenshot;occlusion_off_screenshot")
# Play brings the Game tab to the front: the shot shows the scene through its Main Camera, without grid or gizmos.
add_test(NAME game_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/game_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME game_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/scene_basic.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/game_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
# F3 stats overlay in the Game panel (docs/editor/controls.html#stats-overlay): --play focuses it, a pushed F3 shows
# "FPS <n>  Ping -- ms" at the image's bottom-left (ImGui text; the shot differs from game_play.png), a second F3 hides
# it, and F3 with another panel focused does nothing. Paused / Stopped: StatsToggle in engine_self_check.
add_test(NAME game_f3_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/game_f3.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --play --sim-key 30,F3"
          "-DEXPECT_OUTPUT=Stats overlay shown" -DEXPECT_COUNT=1 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME game_f3_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/game_play.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/game_f3.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
add_test(NAME game_f3_twice
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/game_f3_twice.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --play --sim-key 30,F3 --sim-key 50,F3"
          "-DEXPECT_OUTPUT=Stats overlay shown.*Stats overlay hidden" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME game_f3_unfocused
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/game_f3_unfocused.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --play --sim-key 30,F3" -DFOCUS_PANEL=Hierarchy
          "-DFORBID_OUTPUT=Stats overlay" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME game_no_camera
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/game_no_camera.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/no_camera.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# A saved layout whose Scene panel is shorter than its toolbar: no room for the image must not crash (it once made a
# 65535-pixel-tall framebuffer) or trip ImGui's assert in debug builds.
add_test(NAME scene_panel_no_room
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/scene_panel_no_room.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene" -DFOCUS_PANEL=Scene
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/short_scene_panel.ini"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The Scene view shows the zone named by the scene's Terrain entity in edit mode: the map loads (once), every prefab of
# its objects too. A Terrain map id the project has no map for is reported once, and the editor carries on.
add_test(NAME editor_zone_scene
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_scene.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo/scenes/meadow.scene"
          "-DEXPECT_OUTPUT=Zone: loaded maps/meadow \\(map 10\\)" -DEXPECT_COUNT=1
          "-DFORBID_OUTPUT=did not load|\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_zone_missing_map
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_missing_map.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/missing_map.scene"
          "-DEXPECT_OUTPUT=Zone: the Terrain map did not load: no map with id 999" -DEXPECT_COUNT=1 -DFOCUS_PANEL=Zone
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The Zone tab (docs/editor/zone-builder.html) is a tab beside Hierarchy in the default layout and in a layout saved
# before it existed (tests/layouts/pre_zone.ini); --zone-section expands its Map and Problems sections.
add_test(NAME editor_zone_tab
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_tab.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo/scenes/meadow.scene --zone-section Map,Problems --size 1600x1000"
          -DFOCUS_PANEL=Zone "-DLAYOUT_SAME_DOCK=Hierarchy;Zone" "-DEXPECT_OUTPUT=Zone: loaded maps/meadow \\(map 10\\)"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_zone_tab_old_layout
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_tab_old_layout.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo/scenes/meadow.scene --zone-section Map,Problems --size 1600x1000"
          -DFOCUS_PANEL=Zone "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/pre_zone.ini"
          "-DLAYOUT_SAME_DOCK=Hierarchy;Zone;Systems" "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# No project: the tab says one is required (tests/scenes has a project.kast, so a copy of basic.scene outside it).
file(COPY_FILE "${KAST_TEST_SCENES}/basic.scene" "${CMAKE_CURRENT_BINARY_DIR}/no_project_basic.scene")
add_test(NAME editor_zone_no_project
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_no_project.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/no_project_basic.scene" -DFOCUS_PANEL=Zone
          "-DFORBID_OUTPUT=Zone: |\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(scene_screenshot PROPERTIES FIXTURES_SETUP basic_shot)
set_tests_properties(scene_play_screenshot PROPERTIES FIXTURES_SETUP play_shot)
set_tests_properties(game_play_screenshot PROPERTIES FIXTURES_SETUP game_shot)
set_tests_properties(scene_play_differs PROPERTIES FIXTURES_REQUIRED "basic_shot;play_shot")
set_tests_properties(game_play_differs PROPERTIES FIXTURES_REQUIRED "basic_shot;game_shot")
set_tests_properties(game_f3_screenshot PROPERTIES FIXTURES_SETUP game_f3_shot)
set_tests_properties(game_f3_differs PROPERTIES FIXTURES_REQUIRED "game_shot;game_f3_shot")
# Lua systems: lua.scene in Play vs the same scene with its "systems" emptied, so only the scripts can differ.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_SCENES}/lua.scene")
file(READ "${KAST_TEST_SCENES}/lua.scene" lua_scene)
string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" lua_scene "${lua_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lua_no_systems.scene" "${lua_scene}")
add_test(NAME lua_scene_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_scene.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lua.scene" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/lua.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_play_no_systems_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_play_no_systems.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/lua_no_systems.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/lua_play_no_systems.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/lua_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(lua_play_screenshot PROPERTIES FIXTURES_SETUP lua_play_shot)
set_tests_properties(lua_play_no_systems_screenshot PROPERTIES FIXTURES_SETUP lua_no_systems_shot)
set_tests_properties(lua_play_differs PROPERTIES FIXTURES_REQUIRED "lua_play_shot;lua_no_systems_shot")
add_test(NAME scene_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/basic.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME scene_bad_json
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/malformed.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME scene_missing
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/does_not_exist.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME scene_v1_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/v1.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME scene_v3_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/v3.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME scene_v4_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_SCENES}/v4.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
# Prefabs: tests/project/prefabs.scene (Crate instances, one with overrides; Tower, which nests a Crate; Statue, a
# missing-prefab placeholder; scripts/prefab_spawn.lua calls kast.instantiate; the built game "prefabs" in KAST_GAMES
# runs it in the player). Each prefab_* test runs the headless prefab steps on a fresh copy of the project
# (cmake/prefab_test.cmake) and checks the saved scene and .prefab files.
set(KAST_PREFABS "${KAST_TEST_PROJECT}/prefabs.scene")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/prefabs_roundtrip")
add_test(NAME prefabs_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_PREFABS}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/prefabs_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
# kast_prefab_test(<name> <args> <prefab_test.cmake -D...>...): prefab_<name> in its own copy of the test project.
function(kast_prefab_test name args)
  add_test(NAME prefab_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${KAST_TEST_PROJECT}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/prefab_tests/${name}" -DSCENE=prefabs.scene "-DARGS=${args}" ${ARGN}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prefab_test.cmake")
endfunction()
kast_prefab_test(create "--select 3 --create-prefab prefabs"
  "-DEXPECT=out.scene|\"id\":3,\"name\":\"Ground\",\"parent\":null,\"prefab\":\"prefabs/Ground.prefab\"\;prefabs/Ground.prefab|\"name\":\"Ground\",\"parent\":null,\"components\"")
kast_prefab_test(create_root "--select 3 --create-prefab /" "-DEXPECT=out.scene|\"prefab\":\"Ground.prefab\"\;Ground.prefab|\"Ground\"")
kast_prefab_test(drop_hierarchy "--drop-prefab prefabs/Crate.prefab --drop-prefab prefabs/Crate.prefab,3"
  "-DEXPECT=out.scene|\"id\":14,\"name\":\"Crate\",\"parent\":null,\"prefab\":\"prefabs/Crate.prefab\"\;out.scene|\"id\":16,\"name\":\"Crate\",\"parent\":3,\"prefab\":\"prefabs/Crate.prefab\"")
# Scene view drops along the editor camera's center ray: onto the Crate at (-3, 0.5, 0); far off the Ground onto the
# y = 0 plane; at the sky, 10 m ahead.
kast_prefab_test(drop_view_mesh "--drop-prefab-view prefabs/Crate.prefab,0.5,0.5"
  "-DCAMERA={\"pivot\": [-3, 0.5, 0], \"yaw\": 0, \"pitch\": -20, \"distance\": 6}" -DLAST_Y=above:0.05)
kast_prefab_test(drop_view_plane "--drop-prefab-view prefabs/Crate.prefab,0.5,0.5"
  "-DCAMERA={\"pivot\": [0, 0, -40], \"yaw\": 0, \"pitch\": -30, \"distance\": 10}" -DLAST_Y=zero -DLAST_FAR=5)
kast_prefab_test(drop_view_sky "--drop-prefab-view prefabs/Crate.prefab,0.5,0.5"
  "-DCAMERA={\"pivot\": [0, 5, 0], \"yaw\": 0, \"pitch\": 20, \"distance\": 5}" -DLAST_Y=above:5)
kast_prefab_test(override "--set-field 5,PointLight.range,4"
  "-DEXPECT=out.scene|{\"source\":2,\"id\":5,\"components\":{\"PointLight\":{\"range\":4.0}}}")
kast_prefab_test(apply_field "--set-field 5,PointLight.range,4 --prefab-apply 5:PointLight.range"
  "-DEXPECT=prefabs/Crate.prefab|\"range\":4.0\;out.scene|{\"source\":2,\"id\":5}" "-DFORBID=out.scene|\"range\":4.0")
kast_prefab_test(apply_inner "--set-field 9,PointLight.range,6 --prefab-apply 9:PointLight.range@1"
  "-DEXPECT=prefabs/Crate.prefab|\"range\":6.0" "-DFORBID=prefabs/Tower.prefab|\"range\"")
kast_prefab_test(apply_outer "--set-field 9,PointLight.range,6 --prefab-apply 9:PointLight.range@0"
  "-DEXPECT=prefabs/Tower.prefab|\"PointLight\":{\"range\":6.0}" "-DFORBID=prefabs/Crate.prefab|\"range\":6.0")
kast_prefab_test(apply_all "--prefab-apply 10"
  "-DEXPECT=prefabs/Crate.prefab|gold_r05.mat\;out.scene|{\"source\":2,\"id\":11}\;out.scene|\"id\":4,\"name\":\"Crate\"" "-DFORBID=out.scene|gold_r05")
kast_prefab_test(revert_field "--prefab-revert 11:PointLight.color"
  "-DEXPECT=out.scene|{\"source\":2,\"id\":11}\;out.scene|gold_r05.mat")
kast_prefab_test(revert_all "--prefab-revert 10" "-DFORBID=out.scene|gold_r05\;prefabs/Crate.prefab|gold_r05")
kast_prefab_test(unpack "--prefab-unpack 6"
  "-DEXPECT=out.scene|\"id\":6,\"name\":\"Tower\",\"parent\":null,\"components\"\;out.scene|\"id\":8,\"name\":\"Crate\",\"parent\":6,\"prefab\":\"prefabs/Crate.prefab\"")
kast_prefab_test(edit_mode "--edit-prefab prefabs/Crate.prefab --set-field 2,PointLight.intensity,5 --prefab-save --prefab-back"
  "-DEXPECT=prefabs/Crate.prefab|\"intensity\":5.0\;out.scene|\"color\"\;out.scene|missing.prefab")
kast_prefab_test(cycle_refused "--edit-prefab prefabs/Crate.prefab --drop-prefab prefabs/Tower.prefab" -DEXPECT_FAIL=1)
# Windows (on the screen KAST_WINDOW_POS names): an instance's override marks with the Apply menu, the inner/outer
# Apply submenu, prefab edit mode, its unsaved-changes prompt on Back, and the Delete Asset confirmation listing the
# prefab's users.
add_test(NAME prefab_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/prefab_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_PREFABS} --select 10 --inspector-apply-menu MeshRenderer.material"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME prefab_apply_submenu_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/prefab_apply_submenu.png"
          "-DEXTRA_ARGS=--scene ${KAST_PREFABS} --select 9 --set-field 9,PointLight.range,5 --inspector-apply-menu PointLight.range"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME prefab_edit_mode_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/prefab_edit_mode.png"
          "-DEXTRA_ARGS=--scene ${KAST_PREFABS} --edit-prefab prefabs/Tower.prefab --play"
          "-DEXPECT_OUTPUT=Editing prefab 'prefabs/Tower.prefab'.*Play is disabled while a prefab is open"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME prefab_back_prompt_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/prefab_back_prompt.png"
          "-DEXTRA_ARGS=--scene ${KAST_PREFABS} --edit-prefab prefabs/Crate.prefab --set-field 2,PointLight.range,7 --prefab-back-prompt"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME prefab_delete_dialog_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/prefab_delete_dialog.png"
          "-DEXTRA_ARGS=--scene ${KAST_PREFABS} --assets-delete prefabs/Crate.prefab" -DFOCUS_PANEL=Assets
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Point lights: tests/project/point_lights.scene (dim sun, red / green / blue point lights, bluish ambient; the built
# game "lights" in KAST_GAMES runs it in the player). Editor with Blue Light selected (range sphere), the Lighting panel,
# Play, and Play of a configure-time copy with every PointLight intensity 0, so only point light shading can differ.
# Copies with a zero range or a negative ambient intensity must fail to load.
set(KAST_POINT_LIGHTS "${KAST_TEST_PROJECT}/point_lights.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_POINT_LIGHTS}")
file(READ "${KAST_POINT_LIGHTS}" lights_scene)
string(REGEX REPLACE "(\"PointLight\": {[^}]*\"intensity\": )[0-9.]+" "\\10" lights_off "${lights_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/point_lights_off.scene" "${lights_off}")
string(REPLACE "\"range\": 5" "\"range\": 0" lights_bad "${lights_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/point_lights_bad_range.scene" "${lights_bad}")
string(REPLACE "\"ambientIntensity\": 0.1" "\"ambientIntensity\": -1" lights_bad "${lights_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/point_lights_bad_lighting.scene" "${lights_bad}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/point_lights_roundtrip")
add_test(NAME point_lights_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/point_lights.png"
          "-DEXTRA_ARGS=--scene ${KAST_POINT_LIGHTS} --select 7" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lighting_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lighting_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POINT_LIGHTS}" -DFOCUS_PANEL=Lighting
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME point_lights_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/point_lights_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_POINT_LIGHTS} --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME point_lights_off_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/point_lights_off_play.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/point_lights_off.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME point_lights_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/point_lights_off_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/point_lights_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(point_lights_play_screenshot PROPERTIES FIXTURES_SETUP point_lights_play_shot)
set_tests_properties(point_lights_off_play_screenshot PROPERTIES FIXTURES_SETUP point_lights_off_play_shot)
set_tests_properties(point_lights_play_differs PROPERTIES FIXTURES_REQUIRED "point_lights_play_shot;point_lights_off_play_shot")
add_test(NAME point_lights_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POINT_LIGHTS}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/point_lights_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
foreach(bad range lighting)
  add_test(NAME point_lights_bad_${bad}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCENE=${CMAKE_CURRENT_BINARY_DIR}/point_lights_bad_${bad}.scene"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()
# Physics: tests/project/physics.scene (a ground with a scaled box collider, two falling cubes, a bouncy sphere, a
# trigger zone the first cube falls through, a character, scripts/physics_log.lua logging the events and a raycast; the
# built game "physics" in KAST_GAMES runs it in the player). Edit mode vs Play after 240 frames; Play vs a configure-time
# copy with zero gravity, so only the simulation can differ; the selected Trigger Zone (no mesh) vs the same selection
# in a copy without its Collider, so only the collider wireframe can differ; the selected character's capsule; the
# Physics panel. A copy with bad gravity must fail to load.
set(KAST_PHYSICS "${KAST_TEST_PROJECT}/physics.scene")
set(KAST_PHYSICS_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/physics_project")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_PHYSICS}")
file(READ "${KAST_PHYSICS}" physics_scene)
file(REMOVE_RECURSE "${KAST_PHYSICS_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_PHYSICS_PROJECT}") # copies run physics_log.lua too
string(REPLACE "\"gravity\": [0, -9.81, 0]" "\"gravity\": [0, 0, 0]" physics_variant "${physics_scene}")
file(WRITE "${KAST_PHYSICS_PROJECT}/physics_no_gravity.scene" "${physics_variant}")
string(REGEX REPLACE ",\n *\"Collider\": [^\n]*\"isTrigger\": true[^\n]*" "" physics_variant "${physics_scene}")
file(WRITE "${KAST_PHYSICS_PROJECT}/physics_no_trigger_collider.scene" "${physics_variant}")
string(REPLACE "\"gravity\": [0, -9.81, 0]" "\"gravity\": [0, \"down\", 0]" physics_variant "${physics_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/physics_bad_gravity.scene" "${physics_variant}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/physics_roundtrip")
set(KAST_PHYSICS_LOG "raycast hit entity 2 at y=0.00, distance 10.00.*triggerEnter a=3 b=6.*triggerExit a=3 b=6.*collisionEnter a=2 b=3 normal=.0.00, 1.00, 0.00.*collisionExit a=2 b=5")
# kast_physics_shot(<name> <smoke_test.cmake args...>): writes physics_<name>.png.
function(kast_physics_shot name)
  add_test(NAME physics_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/physics_${name}.png"
            ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(physics_${name}_screenshot PROPERTIES FIXTURES_SETUP physics_${name}_shot)
endfunction()
# kast_physics_differs(<a> <b>): physics_<a>_vs_<b>_differs: physics_<a>.png and physics_<b>.png must differ.
function(kast_physics_differs a b)
  add_test(NAME physics_${a}_vs_${b}_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/physics_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/physics_${b}.png"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(physics_${a}_vs_${b}_differs PROPERTIES FIXTURES_REQUIRED "physics_${a}_shot;physics_${b}_shot")
endfunction()
kast_physics_shot(edit "-DEXTRA_ARGS=--scene ${KAST_PHYSICS}")
kast_physics_shot(play "-DEXTRA_ARGS=--scene ${KAST_PHYSICS} --play" -DFRAMES=240 "-DEXPECT_OUTPUT=${KAST_PHYSICS_LOG}")
kast_physics_shot(play_no_gravity "-DEXTRA_ARGS=--scene ${KAST_PHYSICS_PROJECT}/physics_no_gravity.scene --play" -DFRAMES=240)
kast_physics_shot(collider "-DEXTRA_ARGS=--scene ${KAST_PHYSICS} --select 6")
kast_physics_shot(collider_none "-DEXTRA_ARGS=--scene ${KAST_PHYSICS_PROJECT}/physics_no_trigger_collider.scene --select 6")
kast_physics_shot(character "-DEXTRA_ARGS=--scene ${KAST_PHYSICS} --select 7")
kast_physics_shot(panel "-DEXTRA_ARGS=--scene ${KAST_PHYSICS}" -DFOCUS_PANEL=Physics)
kast_physics_differs(edit play)
kast_physics_differs(play_no_gravity play)
kast_physics_differs(collider_none collider)
add_test(NAME physics_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_PHYSICS}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/physics_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME physics_bad_gravity_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${CMAKE_CURRENT_BINARY_DIR}/physics_bad_gravity.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
# Audio: tests/project/audio.scene (Music: a looping playOnStart AudioSource on the Music bus; Jukebox: a UI-bus
# AudioSource; scripts/audio_sfx.lua fires one-shot SFX through kast.audio; the built game "audio" in KAST_GAMES runs it
# from game.pak). Every run uses --no-audio-device: silent, and the null backend logs its one warning. The Inspector on
# Music (clip picker, Preview), the Mixer panel during Play (voices listed) and the Assets panel with the sound files.
set(KAST_AUDIO "${KAST_TEST_PROJECT}/audio.scene")
set(KAST_AUDIO_LOG "audio_sfx: one-shot started, playing=true")
add_test(NAME audio_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/audio_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_AUDIO} --select 1 --no-audio-device" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME audio_mixer_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/audio_mixer_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_AUDIO} --play --no-audio-device" -DFOCUS_PANEL=Mixer -DFRAMES=90
          "-DEXPECT_OUTPUT=No audio output device" -DEXPECT_COUNT=1 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME audio_play_log
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/audio_play_log.png"
          "-DEXTRA_ARGS=--scene ${KAST_AUDIO} --play --no-audio-device" "-DEXPECT_OUTPUT=${KAST_AUDIO_LOG}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME audio_assets_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/audio_assets.png"
          "-DEXTRA_ARGS=--scene ${KAST_AUDIO} --no-audio-device --assets-select sounds/wave-demo.wav" -DFOCUS_PANEL=Assets
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME audio_mixer_edit_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/audio_mixer_edit.png"
          "-DEXTRA_ARGS=--scene ${KAST_AUDIO} --no-audio-device" -DFOCUS_PANEL=Mixer
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")

# Shadows: tests/project/shadows.scene (a sun with cascaded shadows over a cube, a sphere, a posed skinned character,
# an alpha-mask cutout, alpha-blend glass and a pillar ~49 m out) and point_shadows.scene (5 castShadows point lights
# with pillars; Far Right Light is the farthest from both cameras). The built games "shadows" and "point_shadows" in
# KAST_GAMES run them in the player. Configure-time copies each change one thing; Play screenshots of each are compared
# with the unchanged scene's: *_differs must differ (the change shows), *_same must be identical (it must not show).
set(KAST_SHADOWS "${KAST_TEST_PROJECT}/shadows.scene")
set(KAST_POINT_SHADOWS "${KAST_TEST_PROJECT}/point_shadows.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_SHADOWS}" "${KAST_POINT_SHADOWS}")
file(READ "${KAST_SHADOWS}" shadows_scene)
file(READ "${KAST_POINT_SHADOWS}" point_shadows_scene)
# kast_shadow_variant(<name> <scene text> <regex> <replacement>): writes <name>.scene into the project (so models and
# materials resolve) under the build folder.
set(KAST_SHADOW_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shadow_project")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_SHADOW_PROJECT}")
function(kast_shadow_variant name scene regex replacement)
  string(REGEX REPLACE "${regex}" "${replacement}" variant "${scene}")
  if(variant STREQUAL scene)
    message(FATAL_ERROR "Shadow variant ${name}: '${regex}' matched nothing")
  endif()
  file(WRITE "${KAST_SHADOW_PROJECT}/${name}.scene" "${variant}")
endfunction()
set(sun "(\"intensity\": 0.8)( })")
kast_shadow_variant(shadows_sun_off "${shadows_scene}" "${sun}" "\\1, \"castShadows\": false\\2")
kast_shadow_variant(shadows_sphere_cast_off "${shadows_scene}" "(\"primitive\": \"sphere\", \"material\": \"\")"
                    "\\1, \"castShadows\": false")
kast_shadow_variant(shadows_ground_receive_off "${shadows_scene}" "(\"primitive\": \"plane\", \"material\": \"\")"
                    "\\1, \"receiveShadows\": false")
kast_shadow_variant(shadows_cutout_cast_off "${shadows_scene}" "(\"materials/cutout.mat\")" "\\1, \"castShadows\": false")
kast_shadow_variant(shadows_glass_cast_off "${shadows_scene}" "(\"materials/glass_blue.mat\")"
                    "\\1, \"castShadows\": false")
kast_shadow_variant(shadows_character_cast_off "${shadows_scene}" "(\"materials\": \\[\\])" "\\1, \"castShadows\": false")
set(far_pillar "(\\[2, 8, 2\\] },[^}]*\"material\": \"\")")
kast_shadow_variant(shadows_far_cast_off "${shadows_scene}" "${far_pillar}" "\\1, \"castShadows\": false")
kast_shadow_variant(shadows_short "${shadows_scene}" "${sun}" "\\1, \"maxDistance\": 20\\2")
string(REGEX REPLACE "${far_pillar}" "\\1, \"castShadows\": false" short_far_off "${shadows_scene}")
kast_shadow_variant(shadows_short_far_cast_off "${short_far_off}" "${sun}" "\\1, \"maxDistance\": 20\\2")
kast_shadow_variant(point_shadows_off "${point_shadows_scene}" "\"castShadows\": true" "\"castShadows\": false")
kast_shadow_variant(point_shadows_far_off "${point_shadows_scene}"
                    "(\"color\": \\[1, 0.3, 1\\], \"intensity\": 6, \"range\": 7, \"castShadows\": )true" "\\1false")
kast_shadow_variant(point_shadows_center_off "${point_shadows_scene}"
                    "(\"color\": \\[1, 0.9, 0.7\\], \"intensity\": 6, \"range\": 7, \"castShadows\": )true" "\\1false")
file(COPY_FILE "${KAST_SHADOWS}" "${KAST_SHADOW_PROJECT}/shadows.scene")
file(COPY_FILE "${KAST_POINT_SHADOWS}" "${KAST_SHADOW_PROJECT}/point_shadows.scene")
# Scene view (edit mode) of the base scenes, with and without the shadow; the Inspector on the Sun (shadow fields).
foreach(shot shadows shadows_sun_off point_shadows point_shadows_off)
  add_test(NAME ${shot}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${shot}.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${shot}.scene" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${shot}_screenshot PROPERTIES FIXTURES_SETUP ${shot}_shot)
endforeach()
foreach(pair "shadows|shadows_sun_off" "point_shadows|point_shadows_off")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  add_test(NAME ${b}_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${b}.png"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${b}_differs PROPERTIES FIXTURES_REQUIRED "${a}_shot;${b}_shot")
endforeach()
add_test(NAME shadows_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/shadows_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/shadows.scene --select 1" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Game view (Play) of every variant against its base: expect differs or same. The Assets panel covers the Console,
# whose log names the scene file.
foreach(shot shadows shadows_sun_off shadows_sphere_cast_off shadows_ground_receive_off shadows_cutout_cast_off
             shadows_glass_cast_off shadows_character_cast_off shadows_far_cast_off shadows_short
             shadows_short_far_cast_off point_shadows point_shadows_off point_shadows_far_off point_shadows_center_off)
  add_test(NAME ${shot}_play_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${shot}_play.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${shot}.scene --play" -DFOCUS_PANEL=Assets
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${shot}_play_screenshot PROPERTIES FIXTURES_SETUP ${shot}_play_shot)
endforeach()
# base|variant|differs or same
foreach(compare "shadows|shadows_sun_off|differs" "shadows|shadows_sphere_cast_off|differs"
                "shadows|shadows_ground_receive_off|differs" "shadows|shadows_cutout_cast_off|differs"
                "shadows|shadows_glass_cast_off|same" "shadows|shadows_character_cast_off|differs"
                "shadows|shadows_far_cast_off|differs" "shadows|shadows_short|differs"
                "shadows_short|shadows_short_far_cast_off|same" "point_shadows|point_shadows_off|differs"
                "point_shadows|point_shadows_far_off|same" "point_shadows|point_shadows_center_off|differs")
  string(REPLACE "|" ";" compare "${compare}")
  list(GET compare 0 a)
  list(GET compare 1 b)
  list(GET compare 2 expect)
  set(same OFF)
  if(expect STREQUAL "same")
    set(same ON)
  endif()
  add_test(NAME ${b}_play_${expect}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${a}_play.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${b}_play.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${b}_play_${expect} PROPERTIES FIXTURES_REQUIRED "${a}_play_shot;${b}_play_shot")
endforeach()
foreach(scene shadows point_shadows)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip")
  add_test(NAME ${scene}_roundtrip
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/${scene}.scene"
            "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
endforeach()
# Spot and area lights: tests/project/spot_lights.scene (three SpotLights: soft cones on the ground, a short-range one
# that only reaches the pillar's top), area_lights.scene (a one-sided and a two-sided AreaLight panel, each with a
# sphere in front and behind, a ceiling panel over a glossy floor, a point and a spot light by the center cube: the
# light types share its 8 slots) and spot_area_shadows.scene (a castShadows spot and area light over pillars; the
# built games "spot_area" and "spot_area_off" in KAST_GAMES run it in the player). Configure-time copies in the shadow
# project change one thing: every SpotLight / AreaLight intensity 0, the two-sided panel one-sided, castShadows false.
# Their Scene view and Play shots must differ from the unchanged scene's; so must the selected spot's and area light's
# gizmos. Copies breaking each SpotLight / AreaLight validation rule must fail to load, naming it.
set(KAST_SPOT_LIGHTS "${KAST_TEST_PROJECT}/spot_lights.scene")
set(KAST_AREA_LIGHTS "${KAST_TEST_PROJECT}/area_lights.scene")
set(KAST_SPOT_AREA_SHADOWS "${KAST_TEST_PROJECT}/spot_area_shadows.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_SPOT_LIGHTS}" "${KAST_AREA_LIGHTS}"
             "${KAST_SPOT_AREA_SHADOWS}")
file(READ "${KAST_SPOT_LIGHTS}" spot_scene)
file(READ "${KAST_AREA_LIGHTS}" area_scene)
file(READ "${KAST_SPOT_AREA_SHADOWS}" spot_area_scene)
set(spot_intensity "(\"SpotLight\": {[^}]*\"intensity\": )[0-9.]+")
set(area_intensity "(\"AreaLight\": {[^}]*\"intensity\": )[0-9.]+")
kast_shadow_variant(spot_lights_off "${spot_scene}" "${spot_intensity}" "\\10")
kast_shadow_variant(area_lights_off "${area_scene}" "${area_intensity}" "\\10")
kast_shadow_variant(area_lights_one_sided "${area_scene}" "\"twoSided\": true" "\"twoSided\": false")
kast_shadow_variant(spot_area_shadows_off "${spot_area_scene}" "\"castShadows\": true" "\"castShadows\": false")
string(REGEX REPLACE "${spot_intensity}" "\\10" spot_area_lights_off "${spot_area_scene}")
kast_shadow_variant(spot_area_shadows_lights_off "${spot_area_lights_off}" "${area_intensity}" "\\10")
# kind|base scene text|field|regex|replacement|error the load must log (. for the quotes)
foreach(bad "spot|spot_scene|color|\\[1, 0.75, 0.4\\]|[1.5, 0.75, 0.4]|.color. channels must be between 0 and 1"
            "spot|spot_scene|intensity|\"intensity\": 8,|\"intensity\": -1,|.intensity. must be >= 0"
            "spot|spot_scene|range|\"range\": 7,|\"range\": 0,|.range. must be > 0"
            "spot|spot_scene|inner_angle|\"innerAngle\": 15|\"innerAngle\": 0|.innerAngle. must be > 0"
            "spot|spot_scene|outer_angle|\"outerAngle\": 30|\"outerAngle\": 90|.outerAngle. must be < 90"
            "spot|spot_scene|inner_over_outer|\"innerAngle\": 15|\"innerAngle\": 35|.innerAngle. must be <= .outerAngle."
            "spot|spot_scene|bias|\"outerAngle\": 30 }|\"outerAngle\": 30, \"bias\": -0.5 }|.bias. must be >= 0"
            "area|area_scene|color|\\[1, 0.8, 0.5\\]|[1, 0.8, -0.5]|.color. channels must be between 0 and 1"
            "area|area_scene|intensity|\"intensity\": 8,|\"intensity\": -1,|.intensity. must be >= 0"
            "area|area_scene|width|\"width\": 1.6|\"width\": 0|.width. and .height. must be > 0"
            "area|area_scene|height|\"height\": 0.8|\"height\": -1|.width. and .height. must be > 0"
            "area|area_scene|range|\"height\": 0.8, \"range\": 8|\"height\": 0.8, \"range\": 0|.range. must be > 0"
            "area|area_scene|bias|\"range\": 8 }|\"range\": 8, \"bias\": -1 }|.bias. must be >= 0")
  string(REPLACE "|" ";" bad "${bad}")
  list(GET bad 0 kind)
  list(GET bad 1 base)
  list(GET bad 2 field)
  list(GET bad 3 regex)
  list(GET bad 4 replacement)
  list(GET bad 5 message)
  kast_shadow_variant(${kind}_lights_bad_${field} "${${base}}" "${regex}" "${replacement}")
  add_test(NAME ${kind}_lights_bad_${field}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCENE=${KAST_SHADOW_PROJECT}/${kind}_lights_bad_${field}.scene" "-DEXPECT_OUTPUT=${message}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()
foreach(scene spot_lights area_lights spot_area_shadows)
  file(COPY_FILE "${KAST_TEST_PROJECT}/${scene}.scene" "${KAST_SHADOW_PROJECT}/${scene}.scene")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip")
  add_test(NAME ${scene}_roundtrip
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/${scene}.scene"
            "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
endforeach()
# Scene view: shot|scene|extra editor flags. Gizmos: the Cool Spot's cone, the Two Sided Panel's rect.
foreach(shot "spot_lights|spot_lights|" "spot_lights_off|spot_lights_off|" "spot_lights_gizmo|spot_lights|--select 5"
             "area_lights|area_lights|" "area_lights_off|area_lights_off|" "area_lights_gizmo|area_lights|--select 7"
             "spot_area_shadows|spot_area_shadows|" "spot_area_shadows_off|spot_area_shadows_off|")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 scene)
  list(GET shot 2 flags)
  add_test(NAME ${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${scene}.scene ${flags}" "-DFORBID_OUTPUT=\\[error\\]"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${name}_screenshot PROPERTIES FIXTURES_SETUP ${name}_shot)
endforeach()
foreach(pair "spot_lights|spot_lights_off" "spot_lights|spot_lights_gizmo" "area_lights|area_lights_off"
             "area_lights|area_lights_gizmo" "spot_area_shadows|spot_area_shadows_off")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  add_test(NAME ${b}_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${b}.png"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${b}_differs PROPERTIES FIXTURES_REQUIRED "${a}_shot;${b}_shot")
endforeach()
# Game view (Play) of each variant against its base: all must differ.
foreach(shot spot_lights spot_lights_off area_lights area_lights_off area_lights_one_sided spot_area_shadows
             spot_area_shadows_off)
  add_test(NAME ${shot}_play_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${shot}_play.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${shot}.scene --play" -DFOCUS_PANEL=Assets
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${shot}_play_screenshot PROPERTIES FIXTURES_SETUP ${shot}_play_shot)
endforeach()
foreach(pair "spot_lights|spot_lights_off" "area_lights|area_lights_off" "area_lights|area_lights_one_sided"
             "spot_area_shadows|spot_area_shadows_off")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  add_test(NAME ${b}_play_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${a}_play.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${b}_play.png"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${b}_play_differs PROPERTIES FIXTURES_REQUIRED "${a}_play_shot;${b}_play_shot")
endforeach()
# Terrain: tests/project/terrain.scene (Terrain on map 3 "hills", 2x2 PNG16 tiles; a CharacterController dropped
# above the ground; docs/engine/terrain.html). Play of it, of a copy without the Terrain component (the hills must show) and of
# a copy whose sun casts no shadows (the hills' and the character's shadows must show).
set(KAST_TERRAIN "${KAST_TEST_PROJECT}/terrain.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TERRAIN}")
file(READ "${KAST_TERRAIN}" terrain_scene)
kast_shadow_variant(terrain_off "${terrain_scene}" ",\n *\"Terrain\": {[^}]*}" "")
kast_shadow_variant(terrain_sun_cast_off "${terrain_scene}" "(\"intensity\": 1)( })" "\\1, \"castShadows\": false\\2")
file(COPY_FILE "${KAST_TERRAIN}" "${KAST_SHADOW_PROJECT}/terrain.scene")
# Map switch (what Replication does on world.mapChanged, by scripts/terrain_map_switch.lua): start on map 1 (flat),
# then map 3 (the hills). The "before" copy has no script, so it stays on map 1.
string(REGEX REPLACE "(\"Terrain\": { \"map\": )3" "\\11" terrain_map1 "${terrain_scene}")
if(terrain_map1 STREQUAL terrain_scene)
  message(FATAL_ERROR "terrain.scene: no \"Terrain\": { \"map\": 3")
endif()
file(WRITE "${KAST_SHADOW_PROJECT}/terrain_switch_before.scene" "${terrain_map1}")
kast_shadow_variant(terrain_switch_after "${terrain_map1}" "\"systems\": \\[\\]"
                    "\"systems\": [\"scripts/terrain_map_switch.lua\"]")

foreach(shot terrain terrain_off terrain_sun_cast_off terrain_switch_before)
  add_test(NAME ${shot}_play_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${shot}_play.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${shot}.scene --play" -DFOCUS_PANEL=Assets
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${shot}_play_screenshot PROPERTIES FIXTURES_SETUP ${shot}_play_shot)
endforeach()
add_test(NAME terrain_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/terrain_play.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/terrain_off_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(terrain_play_differs PROPERTIES FIXTURES_REQUIRED "terrain_play_shot;terrain_off_play_shot")
add_test(NAME terrain_sun_cast_off_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/terrain_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/terrain_sun_cast_off_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
add_test(NAME terrain_switch_after_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/terrain_switch_after_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain_switch_after.scene --play" -DFOCUS_PANEL=Assets
          "-DEXPECT_OUTPUT=terrain switch: 4 tiles on map 3" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(terrain_switch_after_play_screenshot PROPERTIES FIXTURES_SETUP terrain_switch_after_play_shot)
add_test(NAME terrain_switch_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/terrain_switch_before_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/terrain_switch_after_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(terrain_switch_play_differs PROPERTIES
                     FIXTURES_REQUIRED "terrain_switch_before_play_shot;terrain_switch_after_play_shot")
set_tests_properties(terrain_sun_cast_off_play_differs PROPERTIES
                     FIXTURES_REQUIRED "terrain_play_shot;terrain_sun_cast_off_play_shot")
# The Zone tab's Terrain entity cases: none (terrain_off: the tab offers Add Terrain entity and the map picker, nothing
# loads) and two (a second Terrain on map 1 added after the first: the tab names the one whose map is the zone).
string(JSON terrain_count LENGTH "${terrain_scene}" entities)
string(JSON terrain_two SET "${terrain_scene}" entities ${terrain_count}
       [=[{"id": 90, "name": "Second Terrain", "parent": null, "components": {"Transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}, "Terrain": {"map": 1, "radius": 1}}}]=])
file(WRITE "${KAST_SHADOW_PROJECT}/zone_two_terrains.scene" "${terrain_two}")
add_test(NAME editor_zone_no_terrain
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_no_terrain.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain_off.scene --zone-section Map,Problems" -DFOCUS_PANEL=Zone
          "-DFORBID_OUTPUT=Zone: |\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# During Play the Zone tab is read-only (and the Scene view has no zone overlays).
add_test(NAME editor_zone_tab_play
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_tab_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain.scene --play" -DFOCUS_PANEL=Zone
          "-DEXPECT_OUTPUT=Zone: loaded maps/hills \\(map 3\\)" "-DFORBID_OUTPUT=Play uses the saved|\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_zone_two_terrains
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_two_terrains.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/zone_two_terrains.scene" -DFOCUS_PANEL=Zone
          "-DEXPECT_OUTPUT=Zone: loaded maps/[a-z]+ \\(map [13]\\)" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Zone tab tools in the Scene view (--zone-tool, --sim-mouse on the Scene view): zone_tools is terrain.scene with a Box
# at the editor camera's pivot, so the scripted click lands on the Box over the terrain. With a tool the click is the
# zone's (a stroke, the spawn moved; the Box is not selected); without one it selects the Box (no zone edit). In memory
# only: nothing is saved.
string(JSON zone_tools SET "${terrain_scene}" entities ${terrain_count}
       [=[{"id": 91, "name": "Box", "parent": null, "components": {"Transform": {"position": [256, 26, 256], "rotation": [0, 0, 0, 1], "scale": [6, 6, 6]}, "MeshRenderer": {"primitive": "cube", "material": ""}}}]=])
file(WRITE "${KAST_SHADOW_PROJECT}/zone_tools.scene" "${zone_tools}")
function(kast_zone_tool_test name args)
  add_test(NAME editor_zone_tool_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_tool_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/zone_tools.scene --zone-section Terrain ${args}" -DFOCUS_PANEL=Zone
            ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endfunction()
kast_zone_tool_test(raise "--zone-tool Raise --sim-mouse 40,left,40,0,0" "-DEXPECT_OUTPUT=Zone: Raise stroke"
                    -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\\\[error\\\\]")
kast_zone_tool_test(ring "--zone-tool Lower --sim-mouse 40,none,0,0,0" "-DFORBID_OUTPUT=stroke|\\\\[error\\\\]")
kast_zone_tool_test(set_spawn "--zone-tool SetSpawn --sim-mouse 40,left,0,0,0"
                    "-DEXPECT_OUTPUT=Zone: player spawn at 25[0-9][.][0-9], [0-9.]+, 25[0-9][.][0-9]" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
kast_zone_tool_test(none "--sim-mouse 40,left,0,0,0" "-DFORBID_OUTPUT=Zone: (.* stroke|player spawn)|\\\\[error\\\\]")
# Areas, spawns, paths, objects (zone_objects.cpp): each tool's click or drag is one logged edit at the pivot.
kast_zone_tool_test(area_rect "--zone-tool AreaRect --sim-mouse 40,left,80,-40,0"
                    "-DEXPECT_OUTPUT=Zone: area area added [(][0-9]+ x [0-9]+ m[)]" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
kast_zone_tool_test(place_spawn "--zone-tool PlaceSpawn --sim-mouse 40,left,0,0,0"
                    "-DEXPECT_OUTPUT=Zone: spawn npc_1 [(]npc[)] placed at 25[0-9][.][0-9]" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
kast_zone_tool_test(path_points "--zone-tool PathPoints --zone-select sentry_1 --sim-mouse 40,left,0,0,0"
                    "-DEXPECT_OUTPUT=Zone: path point added to sentry_1 at 25[0-9][.][0-9]" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
kast_zone_tool_test(place_object "--zone-tool PlaceObject --sim-mouse 40,left,0,0,0"
                    "-DEXPECT_OUTPUT=Zone: Obstacle [A-Za-z]+ [(]tile 1_1, #[0-9]+[)] placed at 25[0-9]" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
# A scatter drag (several dabs) is one stroke: one undo step, logged once.
kast_zone_tool_test(scatter "--zone-tool Scatter --sim-mouse 40,left,200,0,0"
                    "-DEXPECT_OUTPUT=Zone: Scatter stroke [(][1-9][0-9]* visual" -DEXPECT_COUNT=1
                    "-DFORBID_OUTPUT=\\\\[error\\\\]")
# No tool: a drag starting on a zone handle (the Zone tab in front) moves it, one undo step, instead of picking.
# zone_handles is zone_tools with the editor camera's pivot on spawn sentry_1 (maps/hills, ground y ~7).
string(JSON zone_handles SET "${zone_tools}" editor camera pivot [=[[150, 7, 331]]=])
file(WRITE "${KAST_SHADOW_PROJECT}/zone_handles.scene" "${zone_handles}")
add_test(NAME editor_zone_handle_drag
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_handle_drag.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/zone_handles.scene --zone-section Spawns --sim-mouse 40,left,60,0,0"
          -DFOCUS_PANEL=Zone "-DEXPECT_OUTPUT=Zone: moved spawn sentry_1" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Zone objects: tests/project/zone_objects.scene on the hills' fixtures (maps/hills/objects; docs/engine/terrain.html#objects).
# In Play the Hero walks into the obstacle crate and stops at its side, the Walker through the visual crate.
add_test(NAME zone_objects_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_objects_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/zone_objects.scene --play" -DFOCUS_PANEL=Game -DFRAMES=420
          "-DEXPECT_OUTPUT=zone objects: hero x 26[34]\\.[0-9]+, walker x (249\\.[5-9]|25[0-9]\\.)"
          "-DFORBID_OUTPUT=\\[error\\]|Lua error" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/terrain_roundtrip")
add_test(NAME terrain_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/terrain.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/terrain_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
# Post-processing: tests/project/post_processing.scene (every effect on: ACES, bloom, color grading, vignette, FXAA,
# fog; an emissive sphere, bright point lights, near and far glass, pillars out to 55 m, a game GUI; the built game
# "post" in KAST_GAMES runs it in the player). Scene view with post on and toggled off, the Post Processing panel, Play
# (the GUI over the post-processed Game view), and Play of a project-copy scene without the "postProcessing" block, so
# only post-processing can differ. A copy with an unknown tonemapper must fail to load.
set(KAST_POST "${KAST_TEST_PROJECT}/post_processing.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_POST}")
file(READ "${KAST_POST}" post_scene)
set(KAST_POST_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/post_project")
file(REMOVE_RECURSE "${KAST_POST_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_POST_PROJECT}")
string(JSON post_off REMOVE "${post_scene}" postProcessing)
file(WRITE "${KAST_POST_PROJECT}/post_processing_off.scene" "${post_off}")
string(REPLACE "\"ACES\"" "\"Reinhard\"" post_bad "${post_scene}")
file(WRITE "${KAST_POST_PROJECT}/post_processing_bad.scene" "${post_bad}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/post_processing_roundtrip")
add_test(NAME post_processing_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST} --size 1600x900" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME post_processing_toggled_off_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_toggled_off.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST} --size 1600x900 --no-scene-post"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME post_processing_toggle_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/post_processing_toggled_off.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/post_processing.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(post_processing_screenshot PROPERTIES FIXTURES_SETUP post_processing_shot)
set_tests_properties(post_processing_toggled_off_screenshot PROPERTIES FIXTURES_SETUP post_processing_toggled_off_shot)
set_tests_properties(post_processing_toggle_differs PROPERTIES
                     FIXTURES_REQUIRED "post_processing_shot;post_processing_toggled_off_shot")
add_test(NAME post_processing_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST} --size 1600x900" "-DFOCUS_PANEL=Post Processing"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME post_processing_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST} --size 1600x900 --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME post_processing_off_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_off_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/post_processing_off.scene --size 1600x900 --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME post_processing_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/post_processing_off_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/post_processing_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(post_processing_play_screenshot PROPERTIES FIXTURES_SETUP post_processing_play_shot)
set_tests_properties(post_processing_off_play_screenshot PROPERTIES FIXTURES_SETUP post_processing_off_play_shot)
set_tests_properties(post_processing_differs PROPERTIES
                     FIXTURES_REQUIRED "post_processing_play_shot;post_processing_off_play_shot")
add_test(NAME post_processing_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/post_processing_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME post_processing_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/post_processing_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
# Skies (docs/engine/skybox.html): tests/project/sky_procedural.scene (a red horizon between a blue top and a green bottom,
# fog on, a fully fogged far cube), sky_cubemap.scene / sky_cubemap_back.scene (the tests/project/textures/sky_test
# faces: a color each with a white top-left marker, seen from two corners), sky_missing_face.scene (nx missing: the
# procedural sky instead) and sky_mismatched.scene (ny smaller than px). Scene view, Play, the Lighting panel, the
# fallback warnings, a round trip and a copy with an unknown mode, which must fail to load. The built games "sky*" in
# KAST_GAMES read the colors back from the player (player_sky_*_probe).
set(KAST_SKY "${KAST_TEST_PROJECT}/sky_procedural.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_SKY}")
file(READ "${KAST_SKY}" sky_scene)
string(JSON sky_off REMOVE "${sky_scene}" sky)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sky_off.scene" "${sky_off}")
string(REPLACE "\"position\": [0, 1.5, 0]" "\"position\": [500, 80, -900]" sky_moved "${sky_scene}") # the camera only
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sky_moved.scene" "${sky_moved}")
string(REPLACE "\"procedural\"" "\"sunset\"" sky_bad "${sky_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sky_bad.scene" "${sky_bad}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sky_roundtrip")
add_test(NAME sky_procedural_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sky_procedural.png"
          "-DEXTRA_ARGS=--scene ${KAST_SKY} --size 1600x900" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME sky_procedural_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sky_procedural_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_SKY} --size 1600x900 --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME sky_cubemap_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sky_cubemap_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/sky_cubemap.scene --size 1600x900" -DFOCUS_PANEL=Lighting
          "-DFORBID_OUTPUT=Sky cubemap" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME sky_fallback_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sky_fallback_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/sky_missing_face.scene --size 1600x900 --play" -DFOCUS_PANEL=Lighting
          "-DEXPECT_OUTPUT=Sky cubemap: face nx 'textures/sky_test/missing.png': cannot read the file. using the procedural sky"
          -DEXPECT_COUNT=1 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME sky_mismatched_falls_back
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sky_mismatched.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/sky_mismatched.scene"
          "-DEXPECT_OUTPUT=Sky cubemap: face ny is 8x8, but px is 16x16. using the procedural sky"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME sky_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_PROJECT}/sky_cubemap.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/sky_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME sky_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${CMAKE_CURRENT_BINARY_DIR}/sky_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
# Projects: --project opens tests/project (its startScene, scene_a); a folder without project.kast must exit 1.
# Materials: tests/project/materials.scene (gold / red plastic spheres at roughness 0.1, 0.5, 0.9, PNG and JPG textured
# cubes, a metallic-roughness textured sphere, emissive, a mask cutout, two overlapping blended planes, a broken and a
# missing .mat, a missing texture; the built game "materials" in KAST_GAMES runs it in the player). Its Game view, Play,
# and Play of a configure-time copy with every material path emptied (all default grey), so only materials can differ.
# A project copy with the sun off shows metals under ambient light only; the Inspector edits a .mat selected in Assets;
# Assets > Create > Material writes a default .mat into a fresh project copy.
set(KAST_MATERIALS "${KAST_TEST_PROJECT}/materials.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_MATERIALS}")
file(READ "${KAST_MATERIALS}" materials_scene)
string(REGEX REPLACE "\"material\": \"[^\"]*\"" "\"material\": \"\"" materials_default "${materials_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/materials_default.scene" "${materials_default}")
set(KAST_MATERIALS_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/materials_project")
file(REMOVE_RECURSE "${KAST_MATERIALS_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_MATERIALS_PROJECT}")
string(REGEX REPLACE "(\"DirectionalLight\": {[^}]*\"intensity\": )[0-9.]+" "\\10" materials_ambient "${materials_scene}")
file(WRITE "${KAST_MATERIALS_PROJECT}/materials_ambient.scene" "${materials_ambient}")
# Authored KTX / DDS (docs/engine/materials.html, docs/engine/skybox.html): tests/project/gpu_textures.scene draws
# cubes textured by textures/gpu/checker_{bc1.ktx, bc3.dds, bc7.ktx, rgba8.dds} (texturec from checker.png, stored
# format and mips kept) under the cube sky textures/sky_test/cube.ktx (the sky_test faces as one RGBA8 cube); it must
# match a copy using checker.png and the six PNG faces (only BC block artifacts differ). gpu_textures_bad.scene uses an
# ETC1 texture (no D3D11 support), a corrupt KTX, a truncated DDS and a 2D KTX as the sky cube: each warns once and draws
# exactly as a missing PNG texture / face does (a copy using those). The Lighting panel shows the Cube file picker; the
# Inspector lists a .ktx in the material's texture picker. The built game "gpu_textures" in KAST_GAMES ships them as-is.
set(KAST_GPU_TEXTURES "${KAST_TEST_PROJECT}/gpu_textures.scene")
set(KAST_GPU_TEXTURES_BAD "${KAST_TEST_PROJECT}/gpu_textures_bad.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_GPU_TEXTURES}" "${KAST_GPU_TEXTURES_BAD}")
file(READ "${KAST_GPU_TEXTURES}" gpu_scene)
string(REGEX REPLACE "materials/gpu_[a-z0-9]+\\.mat" "materials/checker.mat" gpu_png "${gpu_scene}")
string(REGEX REPLACE ",[ \r\n]*\"cube\": \"[^\"]*\"" "" gpu_png "${gpu_png}")
file(WRITE "${KAST_MATERIALS_PROJECT}/gpu_textures_png.scene" "${gpu_png}")
file(READ "${KAST_GPU_TEXTURES_BAD}" gpu_bad_scene)
string(REGEX REPLACE "materials/gpu_[a-z0-9]+\\.mat" "materials/gpu_missing.mat" gpu_missing "${gpu_bad_scene}")
string(REGEX REPLACE ",[ \r\n]*\"cube\": \"[^\"]*\"" "" gpu_missing "${gpu_missing}")
string(REPLACE "sky_test/px.png" "sky_test/missing.png" gpu_missing "${gpu_missing}")
file(WRITE "${KAST_MATERIALS_PROJECT}/gpu_textures_missing.scene" "${gpu_missing}")
foreach(shot "gpu_textures|${KAST_GPU_TEXTURES}" "gpu_textures_png|${KAST_MATERIALS_PROJECT}/gpu_textures_png.scene"
             "gpu_textures_bad|${KAST_GPU_TEXTURES_BAD}" "gpu_textures_missing|${KAST_MATERIALS_PROJECT}/gpu_textures_missing.scene")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 scene)
  set(check "")
  if(name STREQUAL "gpu_textures")
    set(check "-DFORBID_OUTPUT=Texture '|Sky cubemap")
  elseif(name STREQUAL "gpu_textures_bad")
    set(check "-DEXPECT_OUTPUT=(Texture 'textures/gpu/truncated.dds': the file is truncated|Texture 'textures/gpu/corrupt.ktx': not a valid KTX or DDS file|Texture 'textures/gpu/checker_etc1.ktx': this GPU has no ETC1|Sky cubemap: 'textures/gpu/checker_bc1.ktx': a 2D texture, not a cubemap)"
              -DEXPECT_COUNT=4)
  endif()
  add_test(NAME ${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            "-DEXTRA_ARGS=--scene ${scene} --size 1600x900" ${check} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${name}_screenshot PROPERTIES FIXTURES_SETUP ${name}_shot)
endforeach()
# Mean |difference| per channel over the Scene view (the logs below it differ): BC1 / BC3 / BC7 blocks against the PNG;
# none at all for the fallbacks.
foreach(pair "gpu_textures|gpu_textures_png|1" "gpu_textures_bad|gpu_textures_missing|0")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  list(GET pair 2 limit)
  add_test(NAME ${a}_matches_${b}
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/${a}.png" "${CMAKE_CURRENT_BINARY_DIR}/${b}.png" ${limit}
            330,80,940,540)
  set_tests_properties(${a}_matches_${b} PROPERTIES FIXTURES_REQUIRED "${a}_shot;${b}_shot")
endforeach()
add_test(NAME gpu_textures_sky_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gpu_textures_sky_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_GPU_TEXTURES} --size 1600x900" -DFOCUS_PANEL=Lighting
          "-DFORBID_OUTPUT=Sky cubemap" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gpu_textures_picker_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gpu_textures_picker.png"
          "-DEXTRA_ARGS=--scene ${KAST_GPU_TEXTURES} --size 1600x900 --assets-select materials/gpu_bc7.mat"
          "-DFORBID_OUTPUT=Texture '" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME materials_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/materials.png"
          "-DEXTRA_ARGS=--scene ${KAST_MATERIALS} --size 1600x900" "-DFOCUS_PANEL=Game"
          "-DEXPECT_OUTPUT=Material 'materials/broken.mat': .metallic. must be a number between 0 and 1"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME materials_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/materials_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_MATERIALS} --play"
          "-DEXPECT_OUTPUT=Texture 'textures/does_not_exist.png': cannot read the file"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME materials_default_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/materials_default_play.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/materials_default.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME materials_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/materials_default_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/materials_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(materials_play_screenshot PROPERTIES FIXTURES_SETUP materials_play_shot)
set_tests_properties(materials_default_play_screenshot PROPERTIES FIXTURES_SETUP materials_default_play_shot)
set_tests_properties(materials_differs PROPERTIES FIXTURES_REQUIRED "materials_play_shot;materials_default_play_shot")
add_test(NAME materials_ambient_only_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/materials_ambient_only.png"
          "-DEXTRA_ARGS=--scene ${KAST_MATERIALS_PROJECT}/materials_ambient.scene --size 1600x900" "-DFOCUS_PANEL=Game"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME material_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/material_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_MATERIALS} --select 3 --assets-select materials/cutout.mat"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME mesh_renderer_material_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/mesh_renderer_material.png"
          "-DEXTRA_ARGS=--scene ${KAST_MATERIALS} --select 3" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Material features (normal / AO / emissive textures, UV tiling, cull modes, unlit, additive): every texture loads, and
# the Inspector shows the version 2 fields of a .mat.
add_test(NAME material_features_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/material_features.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/material_features.scene --size 1600x900 --fixed-dt 0.016"
          "-DFOCUS_PANEL=Game" "-DFORBID_OUTPUT=Texture '|Material '" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME material_features_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/material_features_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/material_features.scene --size 1600x1000 --select 8 --assets-select materials/mf_tiled.mat"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME materials_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_MATERIALS}"
          "-DOUT_DIR=${KAST_MATERIALS_PROJECT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
set(KAST_NEW_MATERIAL_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/new_material_project")
add_test(NAME new_material_project_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KAST_NEW_MATERIAL_PROJECT}")
add_test(NAME new_material_project_copy
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${KAST_NEW_MATERIAL_PROJECT}")
add_test(NAME assets_create_material_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/assets_create_material.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_MATERIAL_PROJECT} --assets-create material:materials" "-DFOCUS_PANEL=Assets"
          "-DMUST_EXIST=${KAST_NEW_MATERIAL_PROJECT}/materials/new_material.mat"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME assets_create_material_written
  COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_MATERIAL_PROJECT}/materials/new_material.mat")
set_tests_properties(assets_create_material_written PROPERTIES PASS_REGULAR_EXPRESSION
  "\"version\": 3,.*\"baseColor\": \\[.*0.8.*\"metallic\": 0[.0]*,.*\"roughness\": 0.5,.*\"alphaMode\": \"opaque\".*\"cullMode\": \"back\",.*\"unlit\": false,.*\"shader\": \"\",.*\"params\": {}")
set_tests_properties(new_material_project_clean PROPERTIES FIXTURES_SETUP new_material_clean)
set_tests_properties(new_material_project_copy PROPERTIES FIXTURES_REQUIRED new_material_clean
                     FIXTURES_SETUP new_material_copy)
set_tests_properties(assets_create_material_screenshot PROPERTIES FIXTURES_REQUIRED new_material_copy
                     FIXTURES_SETUP new_material_made)
set_tests_properties(assets_create_material_written PROPERTIES FIXTURES_REQUIRED new_material_made)
# User shaders (.kshader, docs/engine/shaders.html). Every shot here has the Assets panel in front, covering the Console
# (whose log differs), and compares two runs of one project layout:
# - shader_empty_*: tests/project's materials.scene and material_features.scene in a copy whose every .mat (without a
#   shader of its own) uses shaders/empty.kshader, an empty fragment(), must be identical to the same scenes in a plain
#   copy (both folders are named "project", so the panels match).
# - shader_errors: shader_errors.scene's four cubes (a GLSL error on line 7, a bad param line, a missing .kshader, a
#   wrong-typed param value) must render exactly as the error material (a copy with every material a missing .mat),
#   logging the .kshader line.
# - shader_showcase: shader_showcase.scene (params, a texture param, TIME, vertex colors) must differ from a copy
#   without its shader materials.
set(KAST_SHADER_BUILTIN_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_builtin/project")
set(KAST_EMPTY_SHADER_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_empty/project")
set(KAST_SHADER_TEST_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_test_project")
foreach(dir "${KAST_SHADER_BUILTIN_PROJECT}" "${KAST_EMPTY_SHADER_PROJECT}" "${KAST_SHADER_TEST_PROJECT}")
  file(REMOVE_RECURSE "${dir}")
  file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${dir}")
endforeach()
file(GLOB empty_shader_mats "${KAST_TEST_PROJECT}/materials/*.mat")
foreach(mat ${empty_shader_mats})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mat}")
  file(READ "${mat}" mat_text)
  if(NOT mat_text MATCHES "\"shader\"")
    string(REGEX REPLACE "(\"version\": [0-9]+)" "\\1, \"shader\": \"shaders/empty.kshader\"" mat_text "${mat_text}")
    get_filename_component(mat_name "${mat}" NAME)
    file(WRITE "${KAST_EMPTY_SHADER_PROJECT}/materials/${mat_name}" "${mat_text}")
  endif()
endforeach()
# kast_shader_shot(<name> <project> <scene> [PLAY] [smoke_test.cmake -D...]): shader_<name>.png, fixture
# shader_<name>_shot. PLAY shoots the Game view in Play (Lua systems run) instead of the Scene view.
function(kast_shader_shot name project scene)
  set(play "")
  if(PLAY IN_LIST ARGN)
    list(REMOVE_ITEM ARGN PLAY)
    set(play " --play")
  endif()
  add_test(NAME shader_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/shader_${name}.png"
            "-DEXTRA_ARGS=--scene ${project}/${scene} --size 1600x900 --fixed-dt 0.016${play}" -DFOCUS_PANEL=Assets ${ARGN}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(shader_${name}_screenshot PROPERTIES FIXTURES_SETUP shader_${name}_shot)
endfunction()
# kast_shader_compare(<test> <a> <b> <same: ON / OFF>): shader_<a>.png against shader_<b>.png.
function(kast_shader_compare test a b same)
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/shader_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/shader_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "shader_${a}_shot;shader_${b}_shot")
endfunction()
foreach(scene materials material_features)
  kast_shader_shot(builtin_${scene} "${KAST_SHADER_BUILTIN_PROJECT}" ${scene}.scene "-DFORBID_OUTPUT=Compiled shader")
  kast_shader_shot(empty_${scene} "${KAST_EMPTY_SHADER_PROJECT}" ${scene}.scene
                   "-DEXPECT_OUTPUT=Compiled shader 'shaders/empty.kshader'" -DEXPECT_COUNT=1)
  kast_shader_compare(shader_empty_${scene}_same builtin_${scene} empty_${scene} ON)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/shader_errors.scene"
             "${KAST_TEST_PROJECT}/shader_showcase.scene")
file(READ "${KAST_TEST_PROJECT}/shader_errors.scene" shader_scene)
string(REGEX REPLACE "materials/shader_[a-z_]+\\.mat" "materials/does_not_exist.mat" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_errors_reference.scene" "${shader_scene}")
file(READ "${KAST_TEST_PROJECT}/shader_showcase.scene" shader_scene)
string(REGEX REPLACE "materials/shader_[a-z_]+\\.mat" "" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_showcase_builtin.scene" "${shader_scene}")
kast_shader_shot(errors "${KAST_SHADER_TEST_PROJECT}" shader_errors.scene
  "-DEXPECT_OUTPUT=\\[error\\] Shader 'shaders/broken.kshader': line 7: error X3004: undeclared identifier 'undeclaredThing'"
  -DEXPECT_COUNT=1)
kast_shader_shot(errors_log "${KAST_SHADER_TEST_PROJECT}" shader_errors.scene
  "-DEXPECT_OUTPUT=Shader 'shaders/showcase.kshader': param 'stripes' is a float: its value must be a number.*Shader 'shaders/does_not_exist.kshader': cannot read the file.*Shader 'shaders/bad_param.kshader': line 4: unknown param type 'vec5'")
kast_shader_shot(errors_reference "${KAST_SHADER_TEST_PROJECT}" shader_errors_reference.scene)
kast_shader_compare(shader_errors_magenta_same errors_reference errors ON)
kast_shader_shot(showcase "${KAST_SHADER_TEST_PROJECT}" shader_showcase.scene
  "-DEXPECT_OUTPUT=Shader 'shaders/showcase.kshader' declares no param 'removed_param'" -DEXPECT_COUNT=1
  "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(showcase_builtin "${KAST_SHADER_TEST_PROJECT}" shader_showcase_builtin.scene)
kast_shader_compare(shader_showcase_differs showcase_builtin showcase OFF)
# - shader_vertex_offset: lift.kshader's vertex() raising a cube and an instanced batch of spheres, and a mask material
#   whose fragment() cuts every pixel, must render exactly as the plain meshes placed higher and a fully transparent
#   cutout (shader_vertex_offset_reference.scene): shadows follow vertex() and fragment()'s ALPHA.
# - shader_vertex: sway.kshader's vertex() on a cutout primitive, an instanced batch and a skinned model must compile
#   and differ from a copy without its shader materials.
kast_shader_shot(vertex_offset "${KAST_SHADER_TEST_PROJECT}" shader_vertex_offset.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/lift.kshader'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(vertex_offset_reference "${KAST_SHADER_TEST_PROJECT}" shader_vertex_offset_reference.scene)
kast_shader_compare(shader_vertex_offset_same vertex_offset_reference vertex_offset ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/shader_vertex.scene")
file(READ "${KAST_TEST_PROJECT}/shader_vertex.scene" shader_scene)
string(REGEX REPLACE "materials/shader_[a-z_]+\\.mat" "" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_vertex_builtin.scene" "${shader_scene}")
kast_shader_shot(vertex "${KAST_SHADER_TEST_PROJECT}" shader_vertex.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/sway.kshader'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(vertex_builtin "${KAST_SHADER_TEST_PROJECT}" shader_vertex_builtin.scene)
kast_shader_compare(shader_vertex_differs vertex_builtin vertex OFF)
# light(), SCREEN_TEXTURE and DEPTH_TEXTURE (phase 5):
# - shader_light: light_bands.kshader's light() bands under a shadowing sun, a point light and fog; differs from a copy
#   without its shader materials. shader_light_pbr_same: light_pbr.kshader's light(), calling the template's own shade()
#   with its inputs, must render exactly as the built-in shader (a copy whose sphere has no material).
# - shader_screen: screen_water.kshader's refraction (SCREEN_TEXTURE) and foam (DEPTH_TEXTURE), moving with TIME.
# - shader_screen_identity_same: an unlit blend plane showing SCREEN_TEXTURE at SCREEN_UV (magenta where linearDepth()
#   of DEPTH_TEXTURE puts the scene in front of it) must render exactly as a fully transparent plane.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/shader_light.scene"
             "${KAST_TEST_PROJECT}/shader_screen_identity.scene")
file(READ "${KAST_TEST_PROJECT}/shader_light.scene" shader_scene)
string(REGEX REPLACE "materials/shader_light_pbr\\.mat" "" light_reference "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_light_reference.scene" "${light_reference}")
string(REGEX REPLACE "materials/shader_[a-z_]+\\.mat" "" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_light_builtin.scene" "${shader_scene}")
file(READ "${KAST_TEST_PROJECT}/shader_screen_identity.scene" shader_scene)
string(REGEX REPLACE "materials/shader_screen_identity\\.mat" "materials/screen_clear.mat" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_screen_identity_reference.scene" "${shader_scene}")
kast_shader_shot(light "${KAST_SHADER_TEST_PROJECT}" shader_light.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/light_bands.kshader'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(light_reference "${KAST_SHADER_TEST_PROJECT}" shader_light_reference.scene
  "-DFORBID_OUTPUT=\\[error\\]|magenta|light_pbr")
kast_shader_shot(light_builtin "${KAST_SHADER_TEST_PROJECT}" shader_light_builtin.scene)
kast_shader_compare(shader_light_pbr_same light_reference light ON)
kast_shader_compare(shader_light_differs light_builtin light OFF)
# - shader_light_local: light() under a SpotLight and an AreaLight only (no sun), every mesh custom: light_bands.kshader,
#   light_pbr.kshader (also the ground) and the toon Shader Graph's Light Output. It must differ from a copy whose spot
#   and area intensities are 0, so light() runs for both.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/shader_light_local.scene")
file(READ "${KAST_TEST_PROJECT}/shader_light_local.scene" shader_scene)
string(REGEX REPLACE "(\"(Spot|Area)Light\": {[^}]*\"intensity\": )[0-9.]+" "\\10" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_light_local_off.scene" "${shader_scene}")
kast_shader_shot(light_local "${KAST_SHADER_TEST_PROJECT}" shader_light_local.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shader_graphs/toon.kshadergraph'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(light_local_off "${KAST_SHADER_TEST_PROJECT}" shader_light_local_off.scene "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_compare(shader_light_local_differs light_local_off light_local OFF)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shader_light_local_roundtrip")
add_test(NAME shader_light_local_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADER_TEST_PROJECT}/shader_light_local.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/shader_light_local_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
kast_shader_shot(screen "${KAST_SHADER_TEST_PROJECT}" shader_screen.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/screen_water.kshader'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(screen_f60 "${KAST_SHADER_TEST_PROJECT}" shader_screen.scene -DFRAMES=60)
kast_shader_compare(shader_screen_animates screen_f60 screen OFF)
kast_shader_shot(screen_identity "${KAST_SHADER_TEST_PROJECT}" shader_screen_identity.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/screen_identity.kshader'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]|magenta")
kast_shader_shot(screen_identity_reference "${KAST_SHADER_TEST_PROJECT}" shader_screen_identity_reference.scene)
kast_shader_compare(shader_screen_identity_same screen_identity_reference screen_identity ON)
# The samples (phase 6): custom_shaders.scene, all five tests/project/shaders samples (pulse, wind, dissolve, water,
# toon) under a shadowing sun, a point light and fog, wind also on a skinned model.
# - shader_custom: every sample compiles, nothing renders magenta; shader_custom_animates: TIME moves them (frame 60
#   against 120).
# - shader_custom_play: in Play, scripts/dissolve_driver.lua animates the dissolve's threshold param (Lua
#   kast.setMaterialProperty on a .kshader param from the first frame); shader_custom_play_lua_differs: a copy without
#   the script differs at the same frame.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/custom_shaders.scene")
file(READ "${KAST_TEST_PROJECT}/custom_shaders.scene" shader_scene)
string(REPLACE "\"scripts/dissolve_driver.lua\"" "" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/custom_shaders_no_systems.scene" "${shader_scene}")
set(custom_shaders_compiled "Compiled shader 'shaders/(pulse|wind|dissolve|water|toon)\\.kshader'")
kast_shader_shot(custom "${KAST_SHADER_TEST_PROJECT}" custom_shaders.scene
  "-DEXPECT_OUTPUT=${custom_shaders_compiled}" -DEXPECT_COUNT=5 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
kast_shader_shot(custom_f60 "${KAST_SHADER_TEST_PROJECT}" custom_shaders.scene -DFRAMES=60)
kast_shader_compare(shader_custom_animates custom_f60 custom OFF)
kast_shader_shot(custom_play "${KAST_SHADER_TEST_PROJECT}" custom_shaders.scene PLAY
  "-DEXPECT_OUTPUT=${custom_shaders_compiled}" -DEXPECT_COUNT=5 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
kast_shader_shot(custom_play_f60 "${KAST_SHADER_TEST_PROJECT}" custom_shaders.scene PLAY -DFRAMES=60)
kast_shader_shot(custom_play_no_systems "${KAST_SHADER_TEST_PROJECT}" custom_shaders_no_systems.scene PLAY)
kast_shader_compare(shader_custom_play_animates custom_play_f60 custom_play OFF)
kast_shader_compare(shader_custom_play_lua_differs custom_play_no_systems custom_play OFF)
# Shader graphs (.kshadergraph, docs/engine/shader-graph.html), phase 1:
# - shader_graph_empty_*: materials.scene and material_features.scene in a copy whose every .mat (without a shader of its
#   own) uses shaders/empty.kshadergraph (an unconnected Fragment Output) must match the built-in shots
#   (kast_self_check --diff-png, mean at most 1/255).
# - shader_graph_demo: shader_graph_demo.scene (graph_demo.kshadergraph: params, a texture param, UV, TIME, Add,
#   Multiply, Sample Texture, a constant) compiles, nothing renders magenta, and differs from a copy without it.
set(KAST_EMPTY_GRAPH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_empty/project")
file(REMOVE_RECURSE "${KAST_EMPTY_GRAPH_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_EMPTY_GRAPH_PROJECT}")
foreach(mat ${empty_shader_mats})
  file(READ "${mat}" mat_text)
  if(NOT mat_text MATCHES "\"shader\"")
    string(REGEX REPLACE "(\"version\": [0-9]+)" "\\1, \"shader\": \"shaders/empty.kshadergraph\"" mat_text "${mat_text}")
    get_filename_component(mat_name "${mat}" NAME)
    file(WRITE "${KAST_EMPTY_GRAPH_PROJECT}/materials/${mat_name}" "${mat_text}")
  endif()
endforeach()
foreach(scene materials material_features)
  kast_shader_shot(graph_empty_${scene} "${KAST_EMPTY_GRAPH_PROJECT}" ${scene}.scene
                   "-DEXPECT_OUTPUT=Compiled shader 'shaders/empty.kshadergraph'" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]")
  add_test(NAME shader_graph_empty_${scene}_diff
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/shader_builtin_${scene}.png"
            "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_empty_${scene}.png" 0.00392)
  set_tests_properties(shader_graph_empty_${scene}_diff PROPERTIES
                       FIXTURES_REQUIRED "shader_builtin_${scene}_shot;shader_graph_empty_${scene}_shot")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/shader_graph_demo.scene")
file(READ "${KAST_TEST_PROJECT}/shader_graph_demo.scene" shader_scene)
string(REGEX REPLACE "materials/shader_graph_demo\\.mat" "" shader_scene "${shader_scene}")
file(WRITE "${KAST_SHADER_TEST_PROJECT}/shader_graph_demo_builtin.scene" "${shader_scene}")
kast_shader_shot(graph_demo "${KAST_SHADER_TEST_PROJECT}" shader_graph_demo.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/graph_demo.kshadergraph'" -DEXPECT_COUNT=1
  "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
kast_shader_shot(graph_demo_builtin "${KAST_SHADER_TEST_PROJECT}" shader_graph_demo_builtin.scene)
kast_shader_compare(shader_graph_demo_differs graph_demo_builtin graph_demo OFF)
# Editor workflow (phase 3), in a fresh copy of the test project (the shader cache and new files land in it):
# - shader_cache_first / _second: the second start loads showcase.kshader from <project>/.kast/shader_cache.
# - shader_new: Assets > Create > Shader writes a starter that compiles (the Inspector shows it).
# - shader_inspector_params: the Inspector's Shader section for params.kshader (one param of every type);
#   shader_inspector_broken: a material whose .kshader fails shows the error at its line.
# - shader_material_edit: Inspector edits of params (--material-set) save the .mat; undo / redo save it back
#   (shader_material_undo: two edits then Undo; shader_material_redo: an edit, Undo, Redo).
# - shader_hot_reload: cmake/shader_hot_reload_test.ps1 (a .kshader saved while the editor runs shows within 2 s; a
#   broken save keeps the last good shader on screen and shows the error).
set(KAST_SHADER_EDITOR_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_editor_project")
add_test(NAME shader_editor_project_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KAST_SHADER_EDITOR_PROJECT}")
add_test(NAME shader_editor_project_copy
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${KAST_SHADER_EDITOR_PROJECT}")
set_tests_properties(shader_editor_project_clean PROPERTIES FIXTURES_SETUP shader_editor_clean)
set_tests_properties(shader_editor_project_copy PROPERTIES FIXTURES_REQUIRED shader_editor_clean
                     FIXTURES_SETUP shader_editor_copy)
kast_shader_shot(cache_first "${KAST_SHADER_EDITOR_PROJECT}" shader_showcase.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/showcase.kshader' in [0-9.]+ s" -DEXPECT_COUNT=1)
kast_shader_shot(cache_second "${KAST_SHADER_EDITOR_PROJECT}" shader_showcase.scene
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/showcase.kshader' \\(cache hit\\)" -DEXPECT_COUNT=1
  "-DFORBID_OUTPUT=Compiled shader 'shaders/showcase.kshader' in")
set_tests_properties(shader_cache_first_screenshot PROPERTIES FIXTURES_REQUIRED shader_editor_copy
                     FIXTURES_SETUP shader_cache_filled)
set_tests_properties(shader_cache_second_screenshot PROPERTIES FIXTURES_REQUIRED shader_cache_filled)
kast_shader_compare(shader_cache_same cache_first cache_second ON)
add_test(NAME shader_new_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/shader_new.png"
          "-DEXTRA_ARGS=--project ${KAST_SHADER_EDITOR_PROJECT} --assets-create shader:shaders --size 1600x900"
          "-DFOCUS_PANEL=Assets" "-DMUST_EXIST=${KAST_SHADER_EDITOR_PROJECT}/shaders/new_shader.kshader"
          "-DEXPECT_OUTPUT=Created shader 'shaders/new_shader.kshader'.*Compiled shader 'shaders/new_shader.kshader'"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Every editor run in the shared copy waits for cache_first (it must compile, not hit the cache) and holds a lock, so
# parallel ctest runs don't race on its files.
set_tests_properties(shader_new_screenshot PROPERTIES FIXTURES_REQUIRED "shader_editor_copy;shader_cache_filled")
kast_shader_shot(inspector_params "${KAST_SHADER_TEST_PROJECT}" shader_showcase.scene
  "-DEXTRA_ARGS=--scene ${KAST_SHADER_TEST_PROJECT}/shader_showcase.scene --size 1600x900 --assets-select materials/shader_params.mat"
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/params.kshader'" "-DFORBID_OUTPUT=Shader 'shaders/params.kshader'")
kast_shader_shot(inspector_broken "${KAST_SHADER_TEST_PROJECT}" shader_errors.scene
  "-DEXTRA_ARGS=--scene ${KAST_SHADER_TEST_PROJECT}/shader_errors.scene --size 1600x900 --assets-select materials/shader_broken.mat")
# Undo: amount 0.2 stays, the offset edit after it is undone. Redo: the scale edit is undone, then redone.
kast_shader_shot(material_undo "${KAST_SHADER_EDITOR_PROJECT}" shader_showcase.scene
  "-DEXTRA_ARGS=--scene ${KAST_SHADER_EDITOR_PROJECT}/shader_showcase.scene --size 1600x900 --assets-select materials/shader_params.mat --material-set amount=0.2 --material-set offset=[1,1] --material-undo 1"
  "-DEXPECT_OUTPUT=--material-set amount = 0.2.*--material-set offset = \\[1,1\\]" "-DFORBID_OUTPUT=\\[error\\]")
set_tests_properties(shader_material_undo_screenshot PROPERTIES FIXTURES_REQUIRED "shader_editor_copy;shader_cache_filled"
                     FIXTURES_SETUP shader_material_undone)
add_test(NAME shader_material_undo_written
  COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_SHADER_EDITOR_PROJECT}/materials/shader_params.mat")
set_tests_properties(shader_material_undo_written PROPERTIES FIXTURES_REQUIRED shader_material_undone
  PASS_REGULAR_EXPRESSION "\"amount\": 0.2,.*\"tint\"" FAIL_REGULAR_EXPRESSION "offset")
kast_shader_shot(material_redo "${KAST_SHADER_EDITOR_PROJECT}" shader_showcase.scene
  "-DEXTRA_ARGS=--scene ${KAST_SHADER_EDITOR_PROJECT}/shader_showcase.scene --size 1600x900 --assets-select materials/shader_params.mat --material-set scale=5 --material-undo 1 --material-redo 1"
  "-DFORBID_OUTPUT=\\[error\\]")
set_tests_properties(shader_material_redo_screenshot PROPERTIES FIXTURES_REQUIRED shader_material_undone
                     FIXTURES_SETUP shader_material_redone)
set_tests_properties(shader_cache_first_screenshot shader_cache_second_screenshot shader_new_screenshot
                     shader_material_undo_screenshot shader_material_redo_screenshot
                     PROPERTIES RESOURCE_LOCK shader_editor_project)
add_test(NAME shader_material_redo_written
  COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_SHADER_EDITOR_PROJECT}/materials/shader_params.mat")
set_tests_properties(shader_material_redo_written PROPERTIES FIXTURES_REQUIRED shader_material_redone
  PASS_REGULAR_EXPRESSION "\"amount\": 0.2,.*\"scale\": 5[.0]*,.*\"tint\"")
add_test(NAME shader_hot_reload
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_hot_reload_test.ps1"
          -Editor "$<TARGET_FILE:kast_editor>" -Project "${KAST_TEST_PROJECT}" -Out "${CMAKE_CURRENT_BINARY_DIR}/shader_hot_reload")
set_tests_properties(shader_hot_reload PROPERTIES PASS_REGULAR_EXPRESSION "\nOK" TIMEOUT 300)
add_test(NAME project_startup
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/project_startup.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME project_missing
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DFLAG=--project" "-DSCENE=${KAST_TEST_SCENES}/scripts"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
# Scene switching: scene_a's script calls kast.loadScene("scene_b.scene") on its first frame, so Play shows scene B;
# a copy of scene_a with "systems" emptied (generated at configure time) stays on scene A.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/scene_a.scene")
file(READ "${KAST_TEST_PROJECT}/scene_a.scene" switch_scene)
string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" switch_scene "${switch_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/switch_no_systems.scene" "${switch_scene}")
add_test(NAME switch_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/switch_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/scene_a.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME switch_play_no_systems_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/switch_play_no_systems.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/switch_no_systems.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME switch_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/switch_play_no_systems.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/switch_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(switch_play_screenshot PROPERTIES FIXTURES_SETUP switch_shot)
set_tests_properties(switch_play_no_systems_screenshot PROPERTIES FIXTURES_SETUP switch_no_systems_shot)
set_tests_properties(switch_play_differs PROPERTIES FIXTURES_REQUIRED "switch_shot;switch_no_systems_shot")
# Lua components: tests/project/health.scene (Health from components/health.lua, drained and tinted by
# scripts/health_drain.lua). A configure-time copy of tests/project (so its components/ stay active for scenes saved
# there) holds the "systems": [] variant and the round-trip output.
set(KAST_HEALTH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/health_project")
file(GLOB_RECURSE health_project_files CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${health_project_files})
file(REMOVE_RECURSE "${KAST_HEALTH_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${KAST_HEALTH_PROJECT}")
file(READ "${KAST_TEST_PROJECT}/health.scene" health_scene)
string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" health_scene "${health_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/health_no_systems.scene" "${health_scene}")
add_test(NAME lua_components_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_components.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/health.scene --select 3 --size 1280x1000"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_components_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_components_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/health.scene --play" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_components_play_no_systems_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lua_components_play_no_systems.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/health_no_systems.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_components_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/lua_components_play_no_systems.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/lua_components_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(lua_components_play_screenshot PROPERTIES FIXTURES_SETUP health_play_shot)
set_tests_properties(lua_components_play_no_systems_screenshot PROPERTIES FIXTURES_SETUP health_no_systems_shot)
set_tests_properties(lua_components_play_differs PROPERTIES FIXTURES_REQUIRED "health_play_shot;health_no_systems_shot")
# Events: tests/project/events.scene runs scripts/health_events.lua (publishes "health.empty", changes nothing itself)
# and scripts/on_health_empty.lua (greys the named entity), so only event delivery makes Play differ from the copy
# with "systems": [] (in the health_project copy, so Health stays defined). --focus-panel shows the Events panel.
file(READ "${KAST_TEST_PROJECT}/events.scene" events_scene)
string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" events_scene "${events_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/events_no_systems.scene" "${events_scene}")
add_test(NAME events_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/events_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/events.scene --play --focus-panel Events"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME events_play_no_systems_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/events_play_no_systems.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/events_no_systems.scene --play --focus-panel Events"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME events_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/events_play_no_systems.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/events_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(events_play_screenshot PROPERTIES FIXTURES_SETUP events_play_shot)
set_tests_properties(events_play_no_systems_screenshot PROPERTIES FIXTURES_SETUP events_no_systems_shot)
set_tests_properties(events_play_differs PROPERTIES FIXTURES_REQUIRED "events_play_shot;events_no_systems_shot")
# State machines: tests/project/fsm.scene (two traffic_light entities, game machine game_flow from project.kast) runs
# scripts/fsm_driver.lua, which steps the lights once game_flow is playing and has one disallowed change denied. Both
# copies enter their initial states at Play start, so only the driven transitions make Play differ from the copy with
# "systems": []. --focus-panel Events shows the fsm.changed events.
file(READ "${KAST_TEST_PROJECT}/fsm.scene" fsm_scene)
string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" fsm_scene "${fsm_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/fsm_no_systems.scene" "${fsm_scene}")
add_test(NAME fsm_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/fsm_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/fsm.scene --play --focus-panel Events"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME fsm_play_no_systems_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/fsm_play_no_systems.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/fsm_no_systems.scene --play --focus-panel Events"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME fsm_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/fsm_play_no_systems.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/fsm_play.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(fsm_play_screenshot PROPERTIES FIXTURES_SETUP fsm_play_shot)
set_tests_properties(fsm_play_no_systems_screenshot PROPERTIES FIXTURES_SETUP fsm_no_systems_shot)
set_tests_properties(fsm_play_differs PROPERTIES FIXTURES_REQUIRED "fsm_play_shot;fsm_no_systems_shot")
# State Machines panel while playing (both definitions OK, game_flow Global with its live state) with the StateMachine
# Inspector on Light A (live current state), the Inspector stopped (machine / initial state combos), and the New State
# Machine modal: valid (preview fsms/traffic_signal.lua, Create enabled; must not write it) and colliding
# (traffic_light exists: inline error, Create disabled).
set(fsm_args "--scene ${KAST_TEST_PROJECT}/fsm.scene --select 3 --size 1280x1000")
add_test(NAME state_machines_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/state_machines_panel.png" "-DEXTRA_ARGS=${fsm_args} --play"
          "-DFOCUS_PANEL=State Machines" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME state_machine_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/state_machine_inspector.png" "-DEXTRA_ARGS=${fsm_args}"
          "-DFOCUS_PANEL=State Machines" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_state_machine_valid_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_state_machine_valid.png"
          "-DEXTRA_ARGS=${fsm_args} --new-state-machine-prompt TrafficSignal"
          "-DMUST_NOT_EXIST=${KAST_TEST_PROJECT}/fsms/traffic_signal.lua"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_state_machine_collide_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_state_machine_collide.png"
          "-DEXTRA_ARGS=${fsm_args} --new-state-machine-prompt traffic_light"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME lua_components_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_PROJECT}/health.scene"
          "-DOUT_DIR=${KAST_HEALTH_PROJECT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
# Project Manager (no project or scene; editor_smoke* above also start it): tests/project plus a missing folder (greyed,
# "Folder not found"); a corrupt list (backed up to projects.json.bad, empty list); --launcher-open tests/project (the
# started editor takes the screenshot; tests/project is first in the list); and the editor's --project startup, which
# records the project.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/launcher_projects.json" "[
  {\"path\": \"${KAST_TEST_PROJECT}\", \"name\": \"Switch Test\", \"lastOpened\": 1767225600},
  {\"path\": \"${CMAKE_CURRENT_BINARY_DIR}/does_not_exist/old-game\", \"name\": \"Old Game\", \"lastOpened\": 1760000000}
]
")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/launcher_corrupt_projects.json" "[{\"path\": ")
add_test(NAME launcher_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/launcher.png"
          "-DEXTRA_ARGS=--launcher" "-DPROJECTS_FROM=${CMAKE_CURRENT_BINARY_DIR}/launcher_projects.json"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME launcher_corrupt_list
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/launcher_corrupt.png"
          "-DPROJECTS_FROM=${CMAKE_CURRENT_BINARY_DIR}/launcher_corrupt_projects.json"
          "-DMUST_EXIST=${CMAKE_CURRENT_BINARY_DIR}/launcher_corrupt.png.run/projects.json.bad"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME launcher_open
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${KAST_TEST_PROJECT}"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/launcher_open.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/launcher_open_test.cmake")
set_tests_properties(launcher_open PROPERTIES TIMEOUT 120)
add_test(NAME editor_project_records_recent
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_project_records_recent.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT}" "-DLIST_MUST_CONTAIN=${KAST_TEST_PROJECT}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# New Project modal (Project Manager) opened by --new-project: valid (existing parent, preview ...\my-game, Create enabled) and invalid
# (missing parent, inline message, Create disabled). Neither may create anything.
set(KAST_NEW_PROJECT_PARENT "${CMAKE_CURRENT_BINARY_DIR}/new_project_parent")
file(MAKE_DIRECTORY "${KAST_NEW_PROJECT_PARENT}")
add_test(NAME new_project_valid_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_project_valid.png"
          "-DNEW_PROJECT_NAME=My Game" "-DNEW_PROJECT_DIR=${KAST_NEW_PROJECT_PARENT}"
          "-DMUST_NOT_EXIST=${KAST_NEW_PROJECT_PARENT}/my-game" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_project_invalid_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_project_invalid.png"
          "-DNEW_PROJECT_NAME=My Game" "-DNEW_PROJECT_DIR=${CMAKE_CURRENT_BINARY_DIR}/does_not_exist/parent"
          "-DMUST_NOT_EXIST=${CMAKE_CURRENT_BINARY_DIR}/does_not_exist" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# New Lua Component modal (Inspector > Add Component) opened by --new-component-prompt on tests/project, Hero selected:
# valid (preview components/player_health.lua, Create enabled) and colliding (Health exists: inline error). Neither may
# write anything. --new-component then creates PlayerHealth on a fresh copy of tests/project and adds it to Hero; the
# shot differs from lua_components.png (same scene and selection, before).
set(KAST_NEW_COMPONENT_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/new_component_project")
set(new_component_args "--scene ${KAST_TEST_PROJECT}/health.scene --select 3 --size 1280x1000")
add_test(NAME new_component_valid_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_component_valid.png"
          "-DEXTRA_ARGS=${new_component_args} --new-component-prompt PlayerHealth"
          "-DMUST_NOT_EXIST=${KAST_TEST_PROJECT}/components/player_health.lua"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_component_collide_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_component_collide.png"
          "-DEXTRA_ARGS=${new_component_args} --new-component-prompt Health"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# --add-component-popup: the popup's first item, "New Lua Component...", enabled (stopped) and disabled (--play).
add_test(NAME add_component_popup_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/add_component_popup.png"
          "-DEXTRA_ARGS=${new_component_args} --add-component-popup" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME add_component_popup_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/add_component_popup_play.png"
          "-DEXTRA_ARGS=${new_component_args} --add-component-popup --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_component_project_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KAST_NEW_COMPONENT_PROJECT}")
add_test(NAME new_component_project_copy
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${KAST_NEW_COMPONENT_PROJECT}")
add_test(NAME new_component_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/new_component.png"
          "-DEXTRA_ARGS=--scene ${KAST_NEW_COMPONENT_PROJECT}/health.scene --select 3 --size 1280x1000 --new-component PlayerHealth"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME new_component_file_written
  COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_COMPONENT_PROJECT}/components/player_health.lua")
set_tests_properties(new_component_file_written PROPERTIES PASS_REGULAR_EXPRESSION "name = \"PlayerHealth\"")
add_test(NAME new_component_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/lua_components.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/new_component.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(new_component_project_clean PROPERTIES FIXTURES_SETUP new_component_clean)
set_tests_properties(new_component_project_copy PROPERTIES FIXTURES_REQUIRED new_component_clean
                     FIXTURES_SETUP new_component_copy)
set_tests_properties(new_component_screenshot PROPERTIES FIXTURES_REQUIRED new_component_copy
                     FIXTURES_SETUP new_component_shot)
set_tests_properties(new_component_file_written PROPERTIES FIXTURES_REQUIRED new_component_shot)
set_tests_properties(lua_components_screenshot PROPERTIES FIXTURES_SETUP lua_components_shot)
set_tests_properties(new_component_differs PROPERTIES FIXTURES_REQUIRED "lua_components_shot;new_component_shot")
# Models & animation: tests/project/models*.scene. Scene view of OBJ / glTF / GLB / FBX models next to primitives; Play
# of an animated character at two frame counts (the pose must advance) and of a configure-time copy without its
# Animator (bind pose); slot 0 overridden with a .mat vs a copy using the embedded material; a missing model (magenta
# cube, one warning); the Lua animation API (log lines); the Animator Inspector preview with the character selected.
# The copies go into a configure-time project (models + materials only) so their project-relative paths resolve.
set(KAST_MODEL_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/model_project")
foreach(scene models_anim.scene models_material.scene)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/${scene}")
endforeach()
file(REMOVE_RECURSE "${KAST_MODEL_PROJECT}")
file(COPY "${KAST_TEST_PROJECT}/models" "${KAST_TEST_PROJECT}/materials" "${KAST_TEST_PROJECT}/textures"
     DESTINATION "${KAST_MODEL_PROJECT}")
file(WRITE "${KAST_MODEL_PROJECT}/project.kast"
     "{\n  \"version\": 1,\n  \"name\": \"Model Test\",\n  \"startScene\": \"no_animator.scene\"\n}\n")
file(READ "${KAST_TEST_PROJECT}/models_anim.scene" model_scene)
string(REGEX REPLACE ",[ \n]*\"Animator\": {[^}]*}" "" model_scene "${model_scene}")
file(WRITE "${KAST_MODEL_PROJECT}/no_animator.scene" "${model_scene}")
file(READ "${KAST_TEST_PROJECT}/models_material.scene" model_scene)
string(REGEX REPLACE "\"materials\": \\[[^]]*\\]" "\"materials\": []" model_scene "${model_scene}")
file(WRITE "${KAST_MODEL_PROJECT}/embedded_material.scene" "${model_scene}")
macro(kast_model_shot name)
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${name}_shot)
endmacro()
macro(kast_differs name a b)
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${b}.png"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${a}_shot;${b}_shot")
endmacro()
kast_model_shot(models_screenshot "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models.scene --size 1600x900")
kast_model_shot(models_anim_play_60 -DFRAMES=60 "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_anim.scene --play")
kast_model_shot(models_anim_play_120 "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_anim.scene --play")
kast_model_shot(models_no_animator_play_120 "-DEXTRA_ARGS=--scene ${KAST_MODEL_PROJECT}/no_animator.scene --play")
kast_differs(models_anim_advances models_anim_play_60 models_anim_play_120)
kast_differs(models_anim_vs_bind_pose models_no_animator_play_120 models_anim_play_120)
kast_model_shot(models_material_override "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_material.scene")
kast_model_shot(models_material_embedded "-DEXTRA_ARGS=--scene ${KAST_MODEL_PROJECT}/embedded_material.scene")
kast_differs(models_material_override_differs models_material_embedded models_material_override)
kast_model_shot(models_missing "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_missing.scene"
                "-DEXPECT_OUTPUT=Model 'models/does_not_exist.glb': cannot read the file. rendering a magenta cube"
                -DEXPECT_COUNT=1)
kast_model_shot(models_lua "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_lua.scene --play"
                "-DEXPECT_OUTPUT=clips: 178, first Angry.*unknown clip: false [^\n]*has no clip \"No_Such_Clip\".*bad entity: false invalid entity id 999999.*no animator: false entity [0-9]+ needs an \"Animator\" and a \"ModelRenderer\".*state: Angry [0-9.]+ true, speed 1.5")
kast_model_shot(models_inspector_preview "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/models_anim.scene --select 3 --size 1600x900")
# Build: --build packages configure-time copies of tests/project (tests never change their content; Build fills their
# .kast/shader_cache) into
# build/<preset>/game_out/<name>/, then the built player runs from a copy of that folder. Each copy gets its own
# project.kast; the "systems": [] variants show that only the Lua code (run from game.pak) changes the shot.
set(KAST_GAME_OUT "${CMAKE_CURRENT_BINARY_DIR}/game_out")
# An optional fifth argument names a game state machine for project.kast "stateMachines".
function(kast_game_project name project_name start_scene empty_systems)
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}")
  file(REMOVE_RECURSE "${dir}")
  file(COPY "${KAST_TEST_PROJECT}/" DESTINATION "${dir}")
  set(machines "")
  if(ARGV4)
    set(machines ",\n  \"stateMachines\": [\n    \"${ARGV4}\"\n  ]")
  endif()
  file(WRITE "${dir}/project.kast"
       "{\n  \"version\": 1,\n  \"name\": \"${project_name}\",\n  \"startScene\": \"${start_scene}\"${machines}\n}\n")
  file(WRITE "${dir}/.kast/editor.json" "{\n  \"buildFolder\": \"C:/elsewhere\"\n}\n") # must stay out of game.pak
  # Build compiles every shipped .kshader and fails on a broken one: drop the deliberately broken test shaders.
  file(REMOVE "${dir}/shaders/broken.kshader" "${dir}/shaders/bad_param.kshader")
  # Build cooks every shipped model and fails on one it can't import: drop the deliberately corrupt test model.
  file(REMOVE "${dir}/models/corrupt.glb")
  if(empty_systems)
    file(READ "${dir}/${start_scene}" scene)
    string(REGEX REPLACE "\"systems\": \\[[^]]*\\]" "\"systems\": []" scene "${scene}")
    file(WRITE "${dir}/${start_scene}" "${scene}")
  endif()
endfunction()
# name, project name, startScene, empty its systems[, game state machine]
set(KAST_GAMES "switch|Switch Test|scene_a.scene|FALSE" "switch_no_systems|Switch Test|scene_a.scene|TRUE"
               "health|Health Test|health.scene|FALSE" "health_no_systems|Health Test|health.scene|TRUE"
               "events|Events Test|events.scene|FALSE" "events_no_systems|Events Test|events.scene|TRUE"
               "fsm|FSM Test|fsm.scene|FALSE|game_flow" "fsm_no_systems|FSM Test|fsm.scene|TRUE|game_flow"
               "ui|UI Test|ui.scene|FALSE" "effects|Effects Test|ui_effects.scene|FALSE" "bind|Bind Test|ui_bind.scene|FALSE"
               "widgets|Widgets Test|ui_widgets.scene|FALSE" "lists|Lists Test|ui_lists.scene|FALSE"
               "richtext|Rich Text Test|ui_richtext.scene|FALSE" "guiprefab|GUI Prefab Test|ui_prefab.scene|FALSE"
               "lights|Lights Test|point_lights.scene|FALSE"
               "materials|Materials Test|materials.scene|FALSE" "post|Post Test|post_processing.scene|FALSE"
               "models|Models Test|models_anim.scene|FALSE" "shadows|Shadows Test|shadows.scene|FALSE"
               "shadows_off|Shadows Test|shadows_sun_off.scene|FALSE"
               "point_shadows|Point Shadows Test|point_shadows.scene|FALSE"
               "spot_area|Spot Area Test|spot_area_shadows.scene|FALSE"
               "spot_area_off|Spot Area Test|spot_area_shadows_lights_off.scene|FALSE"
               "physics|Physics Test|physics.scene|FALSE"
               "terrain|Terrain Test|terrain.scene|FALSE" "terrain_off|Terrain Test|terrain_off.scene|FALSE"
               "prefabs|Prefabs Test|prefabs.scene|FALSE" "zone_objects|Zone Objects Test|zone_objects.scene|FALSE"
               "sky|Sky Test|sky_procedural.scene|FALSE" "sky_off|Sky Test|sky_off.scene|FALSE"
               "sky_moved|Sky Test|sky_moved.scene|FALSE" "sky_cube|Sky Test|sky_cubemap.scene|FALSE"
               "sky_cube_back|Sky Test|sky_cubemap_back.scene|FALSE" "sky_fallback|Sky Test|sky_missing_face.scene|FALSE"
               "particles|Particles Test|particles_shapes.scene|FALSE"
               "gpu_textures|GPU Textures Test|gpu_textures.scene|FALSE"
               "sky_cube_ktx|Sky Test|sky_cubemap_ktx.scene|FALSE" "sky_cube_dds|Sky Test|sky_cubemap_dds.scene|FALSE"
               "ssao|SSAO Test|ssao.scene|FALSE" "ssao_off|SSAO Test|ssao_off.scene|FALSE"
               "ssr|SSR Test|ssr.scene|FALSE" "ssr_off|SSR Test|ssr_off.scene|FALSE"
               "dof|DoF Test|dof.scene|FALSE" "dof_off|DoF Test|dof_off.scene|FALSE"
               "vfog|Volumetric Fog Test|volumetric_fog.scene|FALSE"
               "vfog_off|Volumetric Fog Test|volumetric_fog_off.scene|FALSE")
foreach(game ${KAST_GAMES})
  string(REPLACE "|" ";" game "${game}")
  list(GET game 0 name)
  list(GET game 1 project_name)
  list(GET game 2 start_scene)
  list(GET game 3 empty_systems)
  set(machine "")
  list(LENGTH game fields)
  if(fields GREATER 4)
    list(GET game 4 machine)
  endif()
  kast_game_project(${name} "${project_name}" ${start_scene} ${empty_systems} "${machine}")
  add_test(NAME build_game_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}"
            "-DOUT=${KAST_GAME_OUT}/${name}" "-DEXE=${project_name}.exe" -DEXPECT=0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
  add_test(NAME player_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/${name}" "-DEXE=${project_name}.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/${name}.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png"
            -DEXPECT=0 "-DEXPECT_OUTPUT=Window title: ${project_name}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  set_tests_properties(build_game_${name} PROPERTIES FIXTURES_SETUP game_${name}_built)
  set_tests_properties(player_${name}_screenshot PROPERTIES FIXTURES_REQUIRED game_${name}_built
                       FIXTURES_SETUP player_${name}_shot TIMEOUT 90)
endforeach()
# F3 stats overlay (docs/editor/controls.html#stats-overlay): a pushed F3 on frame 30 shows "FPS <n>  Ping -- ms" at
# the bottom-left (offline: no world server), so the shot differs from the plain one; off at start, so that one has none.
add_test(NAME player_lights_f3_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/lights" "-DEXE=Lights Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/lights_f3.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_lights_f3.png"
          -DEXPECT=0 "-DEXTRA_ARGS=--sim-key 30,F3" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_lights_f3_screenshot PROPERTIES FIXTURES_REQUIRED game_lights_built
                     FIXTURES_SETUP player_lights_f3_shot TIMEOUT 90)
add_test(NAME player_lights_f3_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_lights.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_lights_f3.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_lights_f3_differs PROPERTIES FIXTURES_REQUIRED "player_lights_shot;player_lights_f3_shot")
# Skies in the player (the scenes are under Skies above). Colors read back at known screen positions (1280x720, from the
# cameras' orientations): the gradient (the same from a camera 1 km away: the sky ignores translation), the fogged far
# cube at the fog color while the sky stays unfogged, sky off clearing to the fog color as before skies, every cube
# face's color and marker corner from two corners of the cube, and a missing face's procedural fallback.
file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/sky_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/sky_off/sky_off.scene")
# sky_cubemap.scene with the sky_test faces as one cube file (texturec --strip; RGBA8 .ktx, BC7 .dds): the same probes
# as sky_cube must pass, so the cube's sides face the same way as the six faces. The faces stay set but unused.
file(READ "${KAST_TEST_PROJECT}/sky_cubemap.scene" sky_cube_scene)
foreach(ext ktx dds)
  string(REPLACE "\"faces\":" "\"cube\": \"textures/sky_test/cube.${ext}\",\n    \"faces\":" sky_cube_file "${sky_cube_scene}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/sky_cube_${ext}/sky_cubemap_${ext}.scene" "${sky_cube_file}")
endforeach()
file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/sky_moved.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/sky_moved/sky_moved.scene")
set(KAST_SKY_GRADIENT 0.5004,0.4201,204,0,169,6 0.5004,0.0299,115,0,235,6 0.5004,0.9701,115,235,0,6
                      0.1004,0.2507,183,0,192,6 0.9004,0.7507,183,192,0,6 0.5004,0.3007,168,0,205,6 0.2004,0.6007,208,164,0,6)
# kast_sky_probe(<game> <fx,fy,r,g,b,tolerance>...): player_<game>_probe reads player_<game>.png (kast_self_check --probe-png).
function(kast_sky_probe name)
  add_test(NAME player_${name}_probe
    COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png" ${ARGN})
  set_tests_properties(player_${name}_probe PROPERTIES FIXTURES_REQUIRED player_${name}_shot)
endfunction()
kast_sky_probe(sky ${KAST_SKY_GRADIENT} 0.5750,0.5000,188,188,188,6)
kast_sky_probe(sky_moved ${KAST_SKY_GRADIENT})
kast_sky_probe(sky_off 0.5004,0.0299,188,188,188,6 0.5004,0.9701,188,188,188,6)
kast_sky_probe(sky_fallback ${KAST_SKY_GRADIENT})
kast_sky_probe(sky_cube 0.1520,0.2448,255,255,255,12 0.4801,0.5204,255,0,0,12 0.3011,0.7041,255,0,0,12
               0.7983,0.0918,255,255,255,12 0.5000,0.4592,0,255,0,12 0.5000,0.0918,0,255,0,12
               0.5199,0.5204,255,255,255,12 0.8480,0.2448,0,0,255,12 0.6989,0.7041,0,0,255,12)
foreach(ext ktx dds)
  kast_sky_probe(sky_cube_${ext} 0.1520,0.2448,255,255,255,12 0.4801,0.5204,255,0,0,12 0.3011,0.7041,255,0,0,12
                 0.7983,0.0918,255,255,255,12 0.5000,0.4592,0,255,0,12 0.5000,0.0918,0,255,0,12
                 0.5199,0.5204,255,255,255,12 0.8480,0.2448,0,0,255,12 0.6989,0.7041,0,0,255,12)
endforeach()
add_test(NAME game_gpu_textures_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/gpu_textures/game.pak"
          "-DMUST_CONTAIN=textures/gpu/checker_bc1.ktx;textures/gpu/checker_bc3.dds;textures/gpu/checker_bc7.ktx;textures/gpu/checker_rgba8.dds;textures/sky_test/cube.ktx;textures/sky_test/cube.dds"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_gpu_textures_pak PROPERTIES FIXTURES_REQUIRED game_gpu_textures_built)
kast_sky_probe(sky_cube_back 0.4122,0.2669,255,255,255,12 0.6885,0.1982,0,255,255,12 0.4220,0.6595,0,255,255,12
               0.5287,0.4747,0,255,255,12 0.3731,0.1025,255,255,255,12 0.7035,0.8928,255,255,255,12
               0.3963,0.7789,255,0,255,12 0.3355,0.2171,0,0,255,12 0.8640,0.0378,255,255,255,12
               0.7882,0.8207,255,255,0,12)
add_test(NAME player_sky_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_sky_off.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_sky.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_sky_differs PROPERTIES FIXTURES_REQUIRED "player_sky_shot;player_sky_off_shot")
add_test(NAME player_sky_fallback_warns
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/sky_fallback" "-DEXE=Sky Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/sky_fallback_warns.run" -DEXPECT=0
          "-DEXPECT_OUTPUT=Sky cubemap: face nx 'textures/sky_test/missing.png': cannot read the file"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_sky_fallback_warns PROPERTIES FIXTURES_REQUIRED game_sky_fallback_built TIMEOUT 90)
# The sun-shadow-off copy for the "shadows_off" game (configured above, under Shadows).
file(COPY_FILE "${KAST_SHADOW_PROJECT}/shadows_sun_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/shadows_off/shadows_sun_off.scene")
# The spot / area light-less copy for the "spot_area_off" game (configured above, under Spot and area lights): the
# built player lights and shadows with both, so the two shots must differ.
file(COPY_FILE "${KAST_SHADOW_PROJECT}/spot_area_shadows_lights_off.scene"
     "${CMAKE_CURRENT_BINARY_DIR}/game_projects/spot_area_off/spot_area_shadows_lights_off.scene")
add_test(NAME player_spot_area_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_spot_area_off.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_spot_area.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_spot_area_differs PROPERTIES FIXTURES_REQUIRED "player_spot_area_shot;player_spot_area_off_shot")
# The terrain-less copy for the "terrain_off" game (configured above, under Terrain): the built player streams the
# hills from game.pak, so the two shots must differ.
file(COPY_FILE "${KAST_SHADOW_PROJECT}/terrain_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/terrain_off/terrain_off.scene")
add_test(NAME player_terrain_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_terrain_off.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_terrain.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_terrain_differs PROPERTIES FIXTURES_REQUIRED "player_terrain_shot;player_terrain_off_shot")
# The built prefabs game loads its instances and nested prefabs from game.pak, and kast.instantiate works in the player.
add_test(NAME player_prefabs_instantiate
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/prefabs" "-DEXE=Prefabs Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/prefabs_instantiate.run" -DEXPECT=0
          "-DEXPECT_OUTPUT=prefab_spawn: instantiated true.*prefab_spawn: missing gives nil"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_prefabs_instantiate PROPERTIES FIXTURES_REQUIRED game_prefabs_built TIMEOUT 90)
# The built zone objects game streams the hills' objects and their prefabs from game.pak in the player: the Hero stops
# at the obstacle crate, the Walker walks through the visual one (as zone_objects_play_screenshot in the editor).
add_test(NAME player_zone_objects_play
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/zone_objects" "-DEXE=Zone Objects Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/zone_objects_play.run" -DEXPECT=0 "-DEXTRA_ARGS=--frames 420"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_zone_objects_play.png"
          "-DEXPECT_OUTPUT=zone objects: hero x 26[34]\\.[0-9]+, walker x (249\\.[5-9]|25[0-9]\\.).*nearest 1 Test Shrine"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_zone_objects_play PROPERTIES FIXTURES_REQUIRED game_zone_objects_built TIMEOUT 90)
# The built physics game logs the same events from game.pak in the player.
add_test(NAME player_physics_events
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/physics" "-DEXE=Physics Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/physics_events.run" -DEXPECT=0 "-DEXPECT_OUTPUT=${KAST_PHYSICS_LOG}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_physics_events PROPERTIES FIXTURES_REQUIRED game_physics_built TIMEOUT 90)
# The built audio game decodes and plays its clips from game.pak. Not in KAST_GAMES: its player runs pass
# --no-audio-device, so tests stay silent.
kast_game_project(audio "Audio Test" audio.scene FALSE "")
add_test(NAME build_game_audio
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/audio"
          "-DOUT=${KAST_GAME_OUT}/audio" "-DEXE=Audio Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_audio PROPERTIES FIXTURES_SETUP game_audio_built)
add_test(NAME player_audio_from_pak
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/audio" "-DEXE=Audio Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/audio_pak.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_audio.png" -DEXPECT=0
          "-DEXPECT_OUTPUT=${KAST_AUDIO_LOG}" "-DEXTRA_ARGS=--no-audio-device"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_audio_from_pak PROPERTIES FIXTURES_REQUIRED game_audio_built TIMEOUT 90)
add_test(NAME player_audio_no_focus
  COMMAND kast_focus_probe "${KAST_GAME_OUT}/audio/Audio Test.exe" --frames 120 --no-audio-device
  WORKING_DIRECTORY "${KAST_GAME_OUT}/audio")
set_tests_properties(player_audio_no_focus PROPERTIES FIXTURES_REQUIRED game_audio_built TIMEOUT 90)
# Networking: tests/project/net_a.scene opens TCP, TLS, ws and wss connections and two requests, then switches to
# net_b.scene, which checks they carried over (scripts/net_a.lua, net_b.lua). kast_net_test_server runs the editor or
# the built game with its fixed-port loopback servers up, so these tests share the net_test_ports lock.
set(KAST_NET_SERVER "$<TARGET_FILE:kast_net_test_server>;--cert;${KAST_TEST_PROJECT}/certs/net_test_ca.pem;--key;${CMAKE_CURRENT_SOURCE_DIR}/tests/net_test_server_data/net_test_key.pem;--")
set(KAST_NET_PASS "scripts/net_b.lua: PASS: 4 connections kept across kast.loadScene, 100/100 ticks, 2/2 responses")
add_test(NAME net_play_scene_switch
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/net_play.png"
          "-DWRAPPER=${KAST_NET_SERVER}" "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/net_a.scene --play --no-audio-device"
          -DFOCUS_PANEL=Network -DFRAMES=300 "-DEXPECT_OUTPUT=${KAST_NET_PASS}" -DEXPECT_COUNT=1
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(net_play_scene_switch PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME net_panel_idle_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/net_panel_idle.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT}" -DFOCUS_PANEL=Network -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
kast_game_project(net "Net Test" net_a.scene FALSE "")
add_test(NAME build_game_net
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/net"
          "-DOUT=${KAST_GAME_OUT}/net" "-DEXE=Net Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_net PROPERTIES FIXTURES_SETUP game_net_built)
# PATH is System32 only (player_test.cmake), so this also fails if Build left out the OpenSSL DLLs.
add_test(NAME player_net_scene_switch
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/net" "-DEXE=Net Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/net.run"
          "-DWRAPPER=${KAST_NET_SERVER}" -DEXPECT=0 "-DEXPECT_OUTPUT=${KAST_NET_PASS}"
          "-DEXTRA_ARGS=--frames 300 --no-audio-device" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_net_scene_switch PROPERTIES FIXTURES_REQUIRED game_net_built RESOURCE_LOCK net_test_ports
                     TIMEOUT 90)
add_test(NAME player_shadows_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_shadows_off.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_shadows.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_shadows_differs PROPERTIES FIXTURES_REQUIRED "player_shadows_shot;player_shadows_off_shot")
foreach(name switch health events fsm)
  add_test(NAME player_${name}_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_${name}_no_systems.png"
            "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(player_${name}_differs PROPERTIES FIXTURES_REQUIRED "player_${name}_shot;player_${name}_no_systems_shot")
endforeach()
foreach(pak missing corrupt)
  add_test(NAME player_${pak}_pak
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/switch" "-DEXE=Switch Test.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/${pak}_pak.run" -DPAK=${pak} -DEXPECT=1
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  set_tests_properties(player_${pak}_pak PROPERTIES FIXTURES_REQUIRED game_switch_built TIMEOUT 90)
endforeach()
# Models in the player: Build cooks every shipped model file into game.pak (<path>.kmodel, kast::modelPakPath) in place
# of the model files and their .bin / .mtl sidecars, through the project's .kast/model_cache ("Cooked model ... (cache
# hit)" on the second Build); the player loads them by their source path ("(cooked)") with no Assimp next to it
# (player_test.cmake checks). player_model_not_cooked: a model game.pak has no cooked entry for (models_missing.scene's)
# is one clear error, and the game keeps running. build_game_corrupt_model: a model Assimp can't read fails the Build
# naming it. editor_model_cache_cold / _warm: the editor imports a project's models once, then reads its cache.
kast_game_project(model_cook "Model Cook" models.scene FALSE "")
add_test(NAME model_cook_clear_cache
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_cook/.kast/model_cache")
foreach(run "cold|Cooked model 'models/female-128-anim.glb' for game.pak|Cooked model 'models/female-128-anim.glb' for game.pak (cache hit)"
            "warm|Cooked model 'models/female-128-anim.glb' for game.pak (cache hit)|-")
  string(REPLACE "|" ";" run "${run}")
  list(GET run 0 name)
  list(GET run 1 expect)
  list(GET run 2 forbid)
  set(forbid_arg "")
  if(NOT forbid STREQUAL "-")
    set(forbid_arg "-DFORBID_OUTPUT=${forbid}")
  endif()
  add_test(NAME build_game_model_cook_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_cook"
            "-DOUT=${KAST_GAME_OUT}/model_cook_${name}" "-DEXE=Model Cook.exe" -DEXPECT=0 "-DEXPECT_OUTPUT=${expect}" ${forbid_arg}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
endforeach()
set_tests_properties(model_cook_clear_cache PROPERTIES FIXTURES_SETUP model_cook_clean RESOURCE_LOCK model_cook_project)
set_tests_properties(build_game_model_cook_cold PROPERTIES FIXTURES_REQUIRED model_cook_clean
                     FIXTURES_SETUP model_cook_cold_built RESOURCE_LOCK model_cook_project)
set_tests_properties(build_game_model_cook_warm PROPERTIES FIXTURES_REQUIRED model_cook_cold_built
                     FIXTURES_SETUP model_cook_built RESOURCE_LOCK model_cook_project)
add_test(NAME game_model_cook_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/model_cook_warm/game.pak"
          "-DMUST_CONTAIN=models/female-128-anim.glb.kmodel;models/cube.obj.kmodel;models/cube.gltf.kmodel;models/kenney/characterMedium.fbx.kmodel;models.scene"
          "-DFORBID_REGEX=^[^.].*\\.(glb|gltf|fbx|obj|bin|mtl)$" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_model_cook_pak PROPERTIES FIXTURES_REQUIRED model_cook_built)
add_test(NAME player_model_cook_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/model_cook_warm" "-DEXE=Model Cook.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/model_cook.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_model_cook.png"
          -DEXPECT=0 "-DEXPECT_OUTPUT=Loaded model 'models/female-128-anim.glb' \\(cooked\\)"
          "-DFORBID_OUTPUT=\\[error\\]|magenta" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_model_cook_screenshot PROPERTIES FIXTURES_REQUIRED model_cook_built TIMEOUT 90)
kast_game_project(model_not_cooked "Model Not Cooked" models_missing.scene FALSE "")
add_test(NAME build_game_model_not_cooked
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_not_cooked"
          "-DOUT=${KAST_GAME_OUT}/model_not_cooked" "-DEXE=Model Not Cooked.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_model_not_cooked PROPERTIES FIXTURES_SETUP game_model_not_cooked_built)
add_test(NAME player_model_not_cooked
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/model_not_cooked" "-DEXE=Model Not Cooked.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/model_not_cooked.run" -DEXPECT=0
          "-DEXPECT_OUTPUT=Model 'models/does_not_exist.glb': game.pak has no cooked model for it"
          "-DFORBID_OUTPUT=no cooked model for it.*no cooked model for it" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_model_not_cooked PROPERTIES FIXTURES_REQUIRED game_model_not_cooked_built TIMEOUT 90)
kast_game_project(corrupt_model "Corrupt Model" models.scene FALSE "")
file(COPY "${KAST_TEST_PROJECT}/models/corrupt.glb" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/game_projects/corrupt_model/models")
add_test(NAME build_game_corrupt_model
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/corrupt_model"
          "-DOUT=${KAST_GAME_OUT}/corrupt_model" -DEXPECT=1 "-DEXPECT_OUTPUT=Build failed: model 'models/corrupt.glb':"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
kast_game_project(model_cache "Model Cache" models.scene FALSE "")
add_test(NAME editor_model_cache_clear
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_cache/.kast/model_cache")
set_tests_properties(editor_model_cache_clear PROPERTIES FIXTURES_SETUP editor_model_cache_clean RESOURCE_LOCK model_cache_project)
foreach(run "cold|Loaded model 'models/female-128-anim.glb' \\(imported in|glb' \\(cache hit"
            "warm|Loaded model 'models/female-128-anim.glb' \\(cache hit\\)|imported in")
  string(REPLACE "|" ";" run "${run}")
  list(GET run 0 name)
  list(GET run 1 expect)
  list(GET run 2 forbid)
  add_test(NAME editor_model_cache_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_model_cache_${name}.png"
            "-DEXTRA_ARGS=--project ${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_cache --scene ${CMAKE_CURRENT_BINARY_DIR}/game_projects/model_cache/models.scene --size 1600x900"
            "-DEXPECT_OUTPUT=${expect}" "-DFORBID_OUTPUT=${forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
set_tests_properties(editor_model_cache_cold PROPERTIES FIXTURES_REQUIRED editor_model_cache_clean
                     FIXTURES_SETUP editor_model_cache_cold_run RESOURCE_LOCK model_cache_project)
set_tests_properties(editor_model_cache_warm PROPERTIES FIXTURES_REQUIRED editor_model_cache_cold_run
                     RESOURCE_LOCK model_cache_project)
# Textures: Build compresses every shipped PNG / JPG a material, model, particle, .kshader param or sky samples into
# game.pak as <path>.<usage>.ktx (kast::texturePakPath: BC1 color / linear, BC3 with alpha, BC5 normal) and each sky's faces into one
# cube (.kast_sky/), through the project's .kast/texture_cache ("Cooked texture ... (cache hit)" on the second Build),
# and leaves those source images out; GUI images (ui/drag.rml's checker.png) and terrain heightmaps still ship as files.
# The player loads the cooked ones ("(cooked)"), with no texturec.exe next to it (player_test.cmake checks).
# build_game_corrupt_texture: an image texturec can't read fails the Build naming it. editor_texture_cache_cold / _warm:
# the editor cooks a project's textures once, then reads its cache; editor_texture_cache_async: without --screenshot's
# synchronous cooks, the editor draws the RGBA8 images and swaps each cooked one in while frames keep running.
kast_game_project(texture_cook "Texture Cook" material_features.scene FALSE "")
add_test(NAME texture_cook_clear_cache
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cook/.kast/texture_cache")
foreach(run "cold|Cooked texture 'textures/normal_bumps.png' as normal for game.pak|Cooked texture 'textures/normal_bumps.png' as normal for game.pak (cache hit)"
            "warm|Cooked texture 'textures/normal_bumps.png' as normal for game.pak (cache hit)|-")
  string(REPLACE "|" ";" run "${run}")
  list(GET run 0 name)
  list(GET run 1 expect)
  list(GET run 2 forbid)
  set(forbid_arg "")
  if(NOT forbid STREQUAL "-")
    set(forbid_arg "-DFORBID_OUTPUT=${forbid}")
  endif()
  add_test(NAME build_game_texture_cook_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cook"
            "-DOUT=${KAST_GAME_OUT}/texture_cook_${name}" "-DEXE=Texture Cook.exe" -DEXPECT=0 "-DEXPECT_OUTPUT=${expect}" ${forbid_arg}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
endforeach()
set_tests_properties(texture_cook_clear_cache PROPERTIES FIXTURES_SETUP texture_cook_clean RESOURCE_LOCK texture_cook_project)
set_tests_properties(build_game_texture_cook_cold PROPERTIES FIXTURES_REQUIRED texture_cook_clean
                     FIXTURES_SETUP texture_cook_cold_built RESOURCE_LOCK texture_cook_project)
set_tests_properties(build_game_texture_cook_warm PROPERTIES FIXTURES_REQUIRED texture_cook_cold_built
                     FIXTURES_SETUP texture_cook_built RESOURCE_LOCK texture_cook_project)
add_test(NAME game_texture_cook_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/texture_cook_warm/game.pak"
          "-DMUST_CONTAIN=textures/normal_bumps.png.normal.ktx;textures/checker.png.color.ktx;textures/orm.png.linear.ktx;textures/ao_grid.png.linear.ktx;textures/emissive_stripes.png.color.ktx;textures/noise.png.linear.ktx;models/kenney/skaterMaleA.png.color.ktx;textures/checker.png;maps/hills/heights/0_0.png;textures/gpu/checker_bc7.ktx"
          "-DFORBID_REGEX=^textures/(normal_bumps|orm|ao_grid|emissive_stripes|noise|mr|bricks|cutout)\\.(png|jpg)$"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_texture_cook_pak PROPERTIES FIXTURES_REQUIRED texture_cook_built)
add_test(NAME player_texture_cook_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/texture_cook_warm" "-DEXE=Texture Cook.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/texture_cook.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_texture_cook.png"
          -DEXPECT=0 "-DEXPECT_OUTPUT=Loaded texture 'textures/normal_bumps.png' as normal \\(cooked\\)"
          "-DFORBID_OUTPUT=\\[error\\]|magenta" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_texture_cook_screenshot PROPERTIES FIXTURES_REQUIRED texture_cook_built TIMEOUT 90)
kast_game_project(corrupt_texture "Corrupt Texture" material_features.scene FALSE "")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/corrupt_texture/textures/corrupt.png" "this is not an image")
add_test(NAME build_game_corrupt_texture
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/corrupt_texture"
          "-DOUT=${KAST_GAME_OUT}/corrupt_texture" -DEXPECT=1 "-DEXPECT_OUTPUT=Build failed: texture 'textures/corrupt.png':"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
kast_game_project(texture_cache "Texture Cache" material_features.scene FALSE "")
add_test(NAME editor_texture_cache_clear
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cache/.kast/texture_cache")
set_tests_properties(editor_texture_cache_clear PROPERTIES FIXTURES_SETUP editor_texture_cache_clean RESOURCE_LOCK texture_cache_project)
set(KAST_TEXTURE_CACHE_ARGS "--project ${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cache --scene ${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cache/material_features.scene --size 1600x900")
foreach(run "cold|Loaded texture 'textures/normal_bumps.png' as normal \\(cooked in|cannot compress"
            "warm|Loaded texture 'textures/normal_bumps.png' as normal \\(cache hit\\)|cooked in")
  string(REPLACE "|" ";" run "${run}")
  list(GET run 0 name)
  list(GET run 1 expect)
  list(GET run 2 forbid)
  add_test(NAME editor_texture_cache_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_texture_cache_${name}.png"
            "-DEXTRA_ARGS=${KAST_TEXTURE_CACHE_ARGS}" "-DEXPECT_OUTPUT=${expect}" "-DFORBID_OUTPUT=${forbid}|cannot compress"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME editor_texture_cache_async_clear
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/game_projects/texture_cache/.kast/texture_cache")
add_test(NAME editor_texture_cache_async
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_texture_cache_async.png"
          "-DEXTRA_ARGS=${KAST_TEXTURE_CACHE_ARGS} --screenshot-on-log 10,(cooked" -DFRAMES=600
          "-DEXPECT_OUTPUT=Loaded texture '[^']+' as [a-z]+ \\(cooked in" "-DFORBID_OUTPUT=as [a-z]+ \\(cache hit|cannot compress"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(editor_texture_cache_cold PROPERTIES FIXTURES_REQUIRED editor_texture_cache_clean
                     FIXTURES_SETUP editor_texture_cache_cold_run RESOURCE_LOCK texture_cache_project)
set_tests_properties(editor_texture_cache_warm PROPERTIES FIXTURES_REQUIRED editor_texture_cache_cold_run
                     FIXTURES_SETUP editor_texture_cache_warm_run RESOURCE_LOCK texture_cache_project)
set_tests_properties(editor_texture_cache_async_clear PROPERTIES FIXTURES_REQUIRED editor_texture_cache_warm_run
                     FIXTURES_SETUP editor_texture_cache_async_clean RESOURCE_LOCK texture_cache_project)
set_tests_properties(editor_texture_cache_async PROPERTIES FIXTURES_REQUIRED editor_texture_cache_async_clean
                     RESOURCE_LOCK texture_cache_project)
# Shaders in the player: Build compiles every shipped .kshader and .kshadergraph into game.pak (kast::shaderPakPath; a
# graph's keyed by its generated text, which the player generates again from the shipped graph) and the player loads
# that bytecode ("Loaded shader ... (precompiled)"; player_test.cmake fails if a shaderc.exe is next to it). The vertex
# scenes have no Main Camera (editor views), so their game copies get one at their editor camera's view. Each game
# runs at --fixed-dt 0.016 like the editor's shader shots. player_shader_offset_same: lift.kshader's vertex() and
# hidden.kshader's cutout render exactly as the plain meshes placed higher (as shader_vertex_offset_same in the editor).
# build_game_broken_shader: a broken .kshader fails the Build naming the file and line, writing nothing.
foreach(game "shader_showcase|shader_showcase.scene|showcase"
             "shader_vertex|shader_vertex.scene|sway|0, 4.5285, 5.3245|-0.300706, 0, 0, 0.953717"
             "shader_offset|shader_vertex_offset.scene|lift|0, 4.6036, 8.1568|-0.216440, 0, 0, 0.976296"
             "shader_offset_ref|shader_vertex_offset_reference.scene|-|0, 4.6036, 8.1568|-0.216440, 0, 0, 0.976296"
             "shader_light|shader_light.scene|light_bands"
             "shader_screen|shader_screen.scene|screen_water"
             "shader_screen_identity|shader_screen_identity.scene|screen_identity"
             "shader_screen_identity_ref|shader_screen_identity.scene|-"
             "custom_shaders|custom_shaders.scene|dissolve"
             "shader_graph_demo|shader_graph_demo.scene|graph_demo.kshadergraph")
  string(REPLACE "|" ";" game "${game}")
  list(GET game 0 name)
  list(GET game 1 scene)
  list(GET game 2 shader)
  list(LENGTH game fields)
  kast_game_project(${name} "Shader Test" ${scene} FALSE "")
  if(fields GREATER 3)
    list(GET game 3 position)
    list(GET game 4 rotation)
    set(scene_file "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}/${scene}")
    file(READ "${scene_file}" scene_text)
    string(REGEX REPLACE "}[\r\n]+  \\][\r\n]+}[\r\n]*$"
           "},\n    {\"id\": 900, \"name\": \"Main Camera\", \"parent\": null, \"components\": {\"Transform\": {\"position\": [${position}], \"rotation\": [${rotation}], \"scale\": [1, 1, 1]}, \"Camera\": {\"verticalFovDegrees\": 60, \"nearPlane\": 0.1, \"farPlane\": 1000}}}\n  ]\n}\n"
           camera_scene "${scene_text}")
    if(camera_scene STREQUAL scene_text)
      message(FATAL_ERROR "Shader game ${name}: no entity list end found in ${scene}")
    endif()
    file(WRITE "${scene_file}" "${camera_scene}")
  endif()
  set(expect "Window title: Shader Test")
  if(NOT shader STREQUAL "-")
    set(expect "Loaded shader 'shaders/${shader}\\.kshader' \\(precompiled\\)")
    if(shader MATCHES "\\.kshadergraph$") # the player generates a graph's .kshader text from the shipped file
      set(expect "Loaded shader 'shaders/${shader}' \\(precompiled\\)")
    endif()
  endif()
  add_test(NAME build_game_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}"
            "-DOUT=${KAST_GAME_OUT}/${name}" "-DEXE=Shader Test.exe" -DEXPECT=0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
  add_test(NAME player_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/${name}" "-DEXE=Shader Test.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/${name}.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png"
            "-DEXTRA_ARGS=--fixed-dt 0.016" -DEXPECT=0 "-DEXPECT_OUTPUT=${expect}" "-DFORBID_OUTPUT=\\[error\\]|magenta"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  set_tests_properties(build_game_${name} PROPERTIES FIXTURES_SETUP game_${name}_built)
  set_tests_properties(player_${name}_screenshot PROPERTIES FIXTURES_REQUIRED game_${name}_built
                       FIXTURES_SETUP player_${name}_shot TIMEOUT 90)
endforeach()
add_test(NAME player_shader_offset_same
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_shader_offset_ref.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_shader_offset.png" -DSAME=ON
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_shader_offset_same PROPERTIES FIXTURES_REQUIRED "player_shader_offset_shot;player_shader_offset_ref_shot")
# player_shader_screen_identity_same: SCREEN_TEXTURE / DEPTH_TEXTURE in the player (as shader_screen_identity_same);
# the reference game's plane is the fully transparent screen_clear.mat.
file(READ "${CMAKE_CURRENT_BINARY_DIR}/game_projects/shader_screen_identity_ref/shader_screen_identity.scene" scene_text)
string(REPLACE "materials/shader_screen_identity.mat" "materials/screen_clear.mat" scene_text "${scene_text}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/shader_screen_identity_ref/shader_screen_identity.scene" "${scene_text}")
add_test(NAME player_shader_screen_identity_same
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_shader_screen_identity_ref.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_shader_screen_identity.png" -DSAME=ON
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_shader_screen_identity_same PROPERTIES
                     FIXTURES_REQUIRED "player_shader_screen_identity_shot;player_shader_screen_identity_ref_shot")
# player_custom_shaders_animates: the samples in the player move between frames 60 and 120 (TIME, and the dissolve
# driven by scripts/dissolve_driver.lua as in the editor's Play).
add_test(NAME player_custom_shaders_f60_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/custom_shaders" "-DEXE=Shader Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/custom_shaders_f60.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_custom_shaders_f60.png"
          "-DEXTRA_ARGS=--fixed-dt 0.016 --frames 60" -DEXPECT=0 "-DFORBID_OUTPUT=\\[error\\]|magenta"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_custom_shaders_f60_screenshot PROPERTIES FIXTURES_REQUIRED game_custom_shaders_built
                     FIXTURES_SETUP player_custom_shaders_f60_shot TIMEOUT 90)
add_test(NAME player_custom_shaders_animates
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_custom_shaders_f60.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_custom_shaders.png" -DSAME=OFF
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_custom_shaders_animates PROPERTIES
                     FIXTURES_REQUIRED "player_custom_shaders_shot;player_custom_shaders_f60_shot")
kast_game_project(broken_shader "Broken Shader" shader_showcase.scene FALSE "")
file(COPY "${KAST_TEST_PROJECT}/shaders/broken.kshader" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/game_projects/broken_shader/shaders")
add_test(NAME build_game_broken_shader
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/broken_shader"
          "-DOUT=${KAST_GAME_OUT}/broken_shader" -DEXPECT=1
          "-DEXPECT_OUTPUT=Build failed: shader 'shaders/broken.kshader': line 7: error X3004: undeclared identifier 'undeclaredThing'"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
# A game in a non-ASCII folder still finds its game.pak (UTF-8 paths, cmake/utf8.manifest).
add_test(NAME player_non_ascii_folder
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/switch" "-DEXE=Switch Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/Jürgen 日本.run" -DEXPECT=0 "-DEXPECT_OUTPUT=Window title: Switch Test"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_non_ascii_folder PROPERTIES FIXTURES_REQUIRED game_switch_built TIMEOUT 90)
kast_game_project(bad_start "Bad Start" missing.scene FALSE)
add_test(NAME build_bad_start_scene
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/bad_start"
          "-DOUT=${KAST_GAME_OUT}/bad_start" -DEXPECT=1 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
add_test(NAME build_no_project
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DOUT=${KAST_GAME_OUT}/no_project" -DEXPECT=1
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
# Build menu opened by --open-menu Build: enabled with a project, disabled with none (a scene outside any project) and
# during Play.
add_test(NAME build_menu_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/build_menu.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-menu Build" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME build_menu_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/build_menu_play.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --play --open-menu Build"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME build_menu_no_project_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/build_menu_no_project.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_SCENES}/basic.scene --open-menu Build"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Build Settings modal opened by --open-build-settings: tests/project (scene_a.scene selected), the bad_start copy
# (missing.scene (missing), Save disabled) and a copy with no shippable scene (hint, combo disabled).
set(KAST_NO_SCENES_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/game_projects/no_scenes")
file(REMOVE_RECURSE "${KAST_NO_SCENES_PROJECT}")
file(WRITE "${KAST_NO_SCENES_PROJECT}/project.kast" "{
  \"version\": 1,
  \"name\": \"No Scenes\",
  \"startScene\": null
}
")
file(COPY "${KAST_TEST_PROJECT}/scene_a.scene" DESTINATION "${KAST_NO_SCENES_PROJECT}/.kast") # excluded, like game.pak
foreach(shot "build_settings|${KAST_TEST_PROJECT}" "build_settings_missing|${CMAKE_CURRENT_BINARY_DIR}/game_projects/bad_start"
             "build_settings_no_scenes|${KAST_NO_SCENES_PROJECT}")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 dir)
  add_test(NAME ${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            "-DEXTRA_ARGS=--project ${dir} --open-build-settings" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME build_no_start_scene
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${KAST_NO_SCENES_PROJECT}"
          "-DOUT=${KAST_GAME_OUT}/no_start" -DEXPECT=1 "-DEXPECT_OUTPUT=Build Settings"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
# Online Services (project.kast "online"): copies of tests/project enabled with the net test CA, and with a CA file that
# doesn't exist. --open-online-settings shows the modal for each and for tests/project (disabled, no block); the
# online game builds and its player logs the config it read from game.pak; the bad CA copy fails to build.
foreach(online "online|Online Test|certs/net_test_ca.pem" "online_bad_ca|Online Bad CA|certs/missing.pem")
  string(REPLACE "|" ";" online "${online}")
  list(GET online 0 name)
  list(GET online 1 project_name)
  list(GET online 2 ca_file)
  kast_game_project(${name} "${project_name}" scene_a.scene FALSE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}/project.kast"
       "{\n  \"version\": 1,\n  \"name\": \"${project_name}\",\n  \"startScene\": \"scene_a.scene\",\n  \"online\": {\n"
       "    \"enabled\": true,\n    \"authUrl\": \"https://localhost:3100\",\n    \"caFile\": \"${ca_file}\"\n  }\n}\n")
endforeach()
foreach(shot "online_settings_disabled|${KAST_TEST_PROJECT}" "online_settings|${CMAKE_CURRENT_BINARY_DIR}/game_projects/online"
             "online_settings_error|${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_bad_ca")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 dir)
  add_test(NAME ${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            "-DEXTRA_ARGS=--project ${dir} --open-online-settings" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME build_game_online
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/online"
          "-DOUT=${KAST_GAME_OUT}/online" "-DEXE=Online Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_online PROPERTIES FIXTURES_SETUP game_online_built)
add_test(NAME player_online_config
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/online" "-DEXE=Online Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/online.run"
          -DEXPECT=0 "-DEXPECT_OUTPUT=Online Services enabled: auth https://localhost:3100, CA file 'certs/net_test_ca.pem'"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_online_config PROPERTIES FIXTURES_REQUIRED game_online_built TIMEOUT 90)
add_test(NAME build_online_bad_ca
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_bad_ca"
          "-DOUT=${KAST_GAME_OUT}/online_bad_ca" -DEXPECT=1 "-DEXPECT_OUTPUT=CA file 'certs/missing.pem' is missing"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
# kast.auth: tests/project/online_auth.scene (scripts/online_auth_demo.lua) logs in and lists the realms against
# kast_net_test_server's fake auth service. online_auth's Network panel screenshot uses the "/stall" base path, so the
# login request is still in flight when it is taken.
foreach(online "online_auth|Online Auth Test|http://127.0.0.1:47705" "online_auth_stall|Online Auth Test|http://127.0.0.1:47705/stall")
  string(REPLACE "|" ";" online "${online}")
  list(GET online 0 name)
  list(GET online 1 project_name)
  list(GET online 2 auth_url)
  kast_game_project(${name} "${project_name}" online_auth.scene FALSE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}/project.kast"
       "{
  \"version\": 1,
  \"name\": \"${project_name}\",
  \"startScene\": \"online_auth.scene\",
"
       "  \"online\": {
    \"enabled\": true,
    \"authUrl\": \"${auth_url}\"
  }
}
")
endforeach()
set(KAST_AUTH_DEMO_PASS "scripts/online_auth_demo.lua: 3 realm.s.: Ashenvale .online. 127.0.0.1:4000, Duskmoor .maintenance. 127.0.0.1:4001, Testgrove .online. 127.0.0.1:47707")
add_test(NAME build_game_online_auth
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_auth"
          "-DOUT=${KAST_GAME_OUT}/online_auth" "-DEXE=Online Auth Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_online_auth PROPERTIES FIXTURES_SETUP game_online_auth_built)
add_test(NAME player_online_auth_demo
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/online_auth" "-DEXE=Online Auth Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/online_auth.run" "-DWRAPPER=${KAST_NET_SERVER}" -DEXPECT=0
          "-DEXPECT_OUTPUT=${KAST_AUTH_DEMO_PASS}" "-DEXTRA_ARGS=--frames 300 --no-audio-device"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_online_auth_demo PROPERTIES FIXTURES_REQUIRED game_online_auth_built
                     RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME auth_play_network_panel
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/auth_play_network.png"
          "-DWRAPPER=${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_auth_stall/online_auth.scene --play --no-audio-device"
          -DFOCUS_PANEL=Network -DFRAMES=120 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(auth_play_network_panel PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
# kast.world: tests/project/online_world.scene (scripts/online_world_demo.lua) logs in, connects to the fake auth's realm
# Testgrove (kast_net_test_server's fake world on 47707, trusted through certs/net_test_ca.pem), creates a character,
# enters the world and chats. world_play_network_panel shows the world's TLS connection in the Network panel.
kast_game_project(online_world "Online World Test" online_world.scene FALSE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_world/project.kast"
     "{
  \"version\": 1,
  \"name\": \"Online World Test\",
  \"startScene\": \"online_world.scene\",
"
     "  \"online\": {
    \"enabled\": true,
    \"authUrl\": \"http://127.0.0.1:47705\",
"
     "    \"caFile\": \"certs/net_test_ca.pem\"
  }
}
")
add_test(NAME build_game_online_world
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_world"
          "-DOUT=${KAST_GAME_OUT}/online_world" "-DEXE=Online World Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_online_world PROPERTIES FIXTURES_SETUP game_online_world_built)
add_test(NAME player_online_world_demo
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/online_world" "-DEXE=Online World Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/online_world.run" "-DWRAPPER=${KAST_NET_SERVER}" -DEXPECT=0
          "-DEXPECT_OUTPUT=connecting to Testgrove .127.0.0.1:47707.*world: connected as account 42 .player.*world: created Demo .100.*world: entered as entity 1 at 100, 0, 100.*world: Other is nearby.*world: chat from Demo: hello world"
          "-DEXTRA_ARGS=--frames 300 --no-audio-device" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_online_world_demo PROPERTIES FIXTURES_REQUIRED game_online_world_built
                     RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME world_play_network_panel
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/world_play_network.png"
          "-DWRAPPER=${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_world/online_world.scene --play --no-audio-device"
          "-DEXPECT_OUTPUT=world: chat from Demo: hello world" -DFOCUS_PANEL=Network -DFRAMES=180
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(world_play_network_panel PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
# Replication: tests/project/online_replication.scene (scripts/online_replication_demo.lua) enters the fake world, whose
# Spawn{2, "Other"} appears as a clone of "Remote Player". The built game logs it; replication_play_game shows the clone
# in the Game view; the Inspector shots show the Network components with Online Services enabled (the copy) and
# disabled (tests/project itself, with its warning).
kast_game_project(online_replication "Online Replication Test" online_replication.scene FALSE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_replication/project.kast"
     "{
  \"version\": 1,
  \"name\": \"Online Replication Test\",
  \"startScene\": \"online_replication.scene\",
"
     "  \"online\": {
    \"enabled\": true,
    \"authUrl\": \"http://127.0.0.1:47705\",
"
     "    \"caFile\": \"certs/net_test_ca.pem\"
  }
}
")
set(KAST_REPLICATION_DEMO_PASS "replication: entered, player at 100, 0, 100.*replication: Other spawned as entity [0-9]+ .server id 2. at 10[12][.0-9]*, 0, 100")
add_test(NAME build_game_online_replication
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_replication"
          "-DOUT=${KAST_GAME_OUT}/online_replication" "-DEXE=Online Replication Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_online_replication PROPERTIES FIXTURES_SETUP game_online_replication_built)
add_test(NAME player_online_replication_demo
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/online_replication" "-DEXE=Online Replication Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/online_replication.run" "-DWRAPPER=${KAST_NET_SERVER}" -DEXPECT=0
          "-DEXPECT_OUTPUT=${KAST_REPLICATION_DEMO_PASS}" "-DEXTRA_ARGS=--frames 300 --no-audio-device"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_online_replication_demo PROPERTIES FIXTURES_REQUIRED game_online_replication_built
                     RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME replication_play_game
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/replication_play_game.png"
          "-DWRAPPER=${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_replication/online_replication.scene --play --no-audio-device"
          "-DEXPECT_OUTPUT=${KAST_REPLICATION_DEMO_PASS}" -DFRAMES=180 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(replication_play_game PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
foreach(shot "replication_inspector_player|${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_replication|3"
             "replication_inspector_manager|${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_replication|7"
             "replication_inspector_disabled|${KAST_TEST_PROJECT}|7")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 dir)
  list(GET shot 2 entity)
  add_test(NAME ${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${name}.png"
            "-DEXTRA_ARGS=--scene ${dir}/online_replication.scene --select ${entity}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
# --world-address: a copy of the demo aimed at realm Ashenvale (127.0.0.1:4000, nothing listens) still reaches the fake
# world, because the editor's override replaces the realm's address.
set(world_dir "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_world")
file(READ "${world_dir}/scripts/online_world_demo.lua" world_demo)
string(REPLACE "local REALM, CHARACTER = \"Testgrove\"" "local REALM, CHARACTER = \"Ashenvale\"" world_demo "${world_demo}")
file(WRITE "${world_dir}/scripts/online_world_override.lua" "${world_demo}")
file(READ "${world_dir}/online_world.scene" world_scene)
string(REPLACE "online_world_demo.lua" "online_world_override.lua" world_scene "${world_scene}")
file(WRITE "${world_dir}/online_world_override.scene" "${world_scene}")
add_test(NAME world_address_override
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/world_address_override.png"
          "-DWRAPPER=${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--scene ${world_dir}/online_world_override.scene --play --no-audio-device --world-address 127.0.0.1:47707"
          "-DEXPECT_OUTPUT=World: connecting to realm 1 at 127.0.0.1:47707 .address override.*world: chat from Demo: hello world"
          -DFRAMES=180 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(world_address_override PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)

# --- Online panel (Window > Online, docs/editor/online-tooling.html) ---
# online_session: tests/project's online_session.scene (scripts/online_session_demo.lua logs auth.session and whether
# Play started logged in) with Online Services on against kast_net_test_server's fake auth (47705) and world (47707,
# trusted through certs/net_test_ca.pem). online_down: the same, its auth URL on a port nothing listens on.
foreach(online "online_session|http://127.0.0.1:47705" "online_down|http://127.0.0.1:47799")
  string(REPLACE "|" ";" online "${online}")
  list(GET online 0 name)
  list(GET online 1 auth_url)
  kast_game_project(${name} "Online Session Test" online_session.scene FALSE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}/project.kast"
       "{\n  \"version\": 1,\n  \"name\": \"Online Session Test\",\n  \"startScene\": \"online_session.scene\",\n"
       "  \"online\": {\n    \"enabled\": true,\n    \"authUrl\": \"${auth_url}\",\n    \"caFile\": \"certs/net_test_ca.pem\"\n  }\n}\n")
endforeach()
set(KAST_SESSION_DIR "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_session")
# The fake auth's accounts (net_test_util.h); its demo password is public test data. Through the environment, as
# --online-login-env reads it, never on the editor's command line.
set(KAST_ONLINE_ADMIN_ENV "${CMAKE_COMMAND};-E;env;KAST_ONLINE_EMAIL=admin@example.com;KAST_ONLINE_PASSWORD=demo-password-123")
set(KAST_ONLINE_PLAYER_ENV "${CMAKE_COMMAND};-E;env;KAST_ONLINE_EMAIL=demo@example.com;KAST_ONLINE_PASSWORD=demo-password-123")
# Never in the settings file: the password, the fake's tokens (their JWT header) and refresh tokens.
set(KAST_ONLINE_SECRETS "demo-password-123$<SEMICOLON>eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0$<SEMICOLON>refresh-$<SEMICOLON>Token")
add_test(NAME online_panel_disabled_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_panel_disabled.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT}" -DFOCUS_PANEL=Online
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME online_panel_down_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_panel_down.png"
          "-DEXTRA_ARGS=--project ${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_down --size 1280x900" -DFOCUS_PANEL=Online
          -DFRAMES=300 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Refused connections to 127.0.0.1 take about 2 s on Windows: 300 frames shows auth unreachable, not still checking.
# No docker on PATH (only System32): Start / Stop / Status disabled with the reason.
add_test(NAME online_panel_no_docker_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_panel_no_docker.png"
          "-DWRAPPER=${CMAKE_COMMAND};-E;env;PATH=C:/Windows/System32"
          "-DEXTRA_ARGS=--project ${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_down --size 1280x900" -DFOCUS_PANEL=Online
          -DFRAMES=300 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME online_panel_admin_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_panel_admin.png"
          "-DWRAPPER=${KAST_ONLINE_ADMIN_ENV};${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--project ${KAST_SESSION_DIR} --online-login-env --size 1280x1400" -DFOCUS_PANEL=Online
          "-DEXPECT_OUTPUT=Online: logged in as admin@example.com .admin." "-DSETTINGS_MUST_NOT_CONTAIN=${KAST_ONLINE_SECRETS}"
          -DFRAMES=150 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(online_panel_admin_screenshot PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
# A player login: no Admin section (compare with the admin shot).
add_test(NAME online_panel_player_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_panel_player.png"
          "-DWRAPPER=${KAST_ONLINE_PLAYER_ENV};${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--project ${KAST_SESSION_DIR} --online-login-env --size 1280x1400" -DFOCUS_PANEL=Online
          "-DEXPECT_OUTPUT=Online: logged in as demo@example.com .player." -DFRAMES=150
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(online_panel_player_screenshot PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
# Play after the panel's test login starts logged in (auth.session reaches Lua); without one it starts logged out.
add_test(NAME online_play_logged_in
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_play_logged_in.png"
          "-DWRAPPER=${KAST_ONLINE_PLAYER_ENV};${KAST_NET_SERVER}"
          "-DEXTRA_ARGS=--scene ${KAST_SESSION_DIR}/online_session.scene --online-login-env --play --no-audio-device"
          "-DEXPECT_OUTPUT=Play started logged in as demo@example.com.*auth.session: demo@example.com .account 42, player."
          "-DSETTINGS_MUST_NOT_CONTAIN=${KAST_ONLINE_SECRETS}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(online_play_logged_in PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME online_play_logged_out
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/online_play_logged_out.png"
          "-DWRAPPER=${KAST_NET_SERVER}" "-DEXTRA_ARGS=--scene ${KAST_SESSION_DIR}/online_session.scene --play --no-audio-device"
          "-DEXPECT_OUTPUT=Play started logged out" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(online_play_logged_out PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
add_test(NAME preferences_compose_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/preferences_compose.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-preferences" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The panel's world override (saved in the editor settings) aims editor Play at the fake world as --world-address does;
# the built game, started on the same scene, ignores it and tries the realm's own address.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/online_override_settings.json" "{\n  \"worldOverride\": \"127.0.0.1:47707\"\n}\n")
add_test(NAME world_override_setting
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/world_override_setting.png"
          "-DWRAPPER=${KAST_NET_SERVER}" "-DSETTINGS_FROM=${CMAKE_CURRENT_BINARY_DIR}/online_override_settings.json"
          "-DEXTRA_ARGS=--scene ${world_dir}/online_world_override.scene --play --no-audio-device"
          "-DEXPECT_OUTPUT=World: connecting to realm 1 at 127.0.0.1:47707 .address override.*world: chat from Demo: hello world"
          -DFRAMES=180 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(world_override_setting PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 90)
set(override_game "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_world_override")
file(REMOVE_RECURSE "${override_game}")
file(COPY "${world_dir}/" DESTINATION "${override_game}")
file(READ "${override_game}/project.kast" override_project)
string(REPLACE "\"online_world.scene\"" "\"online_world_override.scene\"" override_project "${override_project}")
file(WRITE "${override_game}/project.kast" "${override_project}")
add_test(NAME build_game_online_world_override
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${override_game}"
          "-DOUT=${KAST_GAME_OUT}/online_world_override" "-DEXE=Online World Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_online_world_override PROPERTIES FIXTURES_SETUP game_online_world_override_built)
add_test(NAME player_ignores_world_override
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/online_world_override" "-DEXE=Online World Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/online_world_override.run" "-DWRAPPER=${KAST_NET_SERVER}" -DEXPECT=0
          "-DEXPECT_OUTPUT=World: connecting to realm 1 at 127.0.0.1:4000[^ ]"
          "-DEXTRA_ARGS=--frames 200 --no-audio-device" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_ignores_world_override PROPERTIES FIXTURES_REQUIRED game_online_world_override_built
                     RESOURCE_LOCK net_test_ports TIMEOUT 90)
# Secrets: an admin test login and Play, then a Build of the same project; the password and token texts appear in none
# of the settings file, the project and game.pak (cmake/online_secrets_test.cmake).
set(secrets_project "${CMAKE_CURRENT_BINARY_DIR}/game_projects/online_secrets")
file(REMOVE_RECURSE "${secrets_project}")
file(MAKE_DIRECTORY "${secrets_project}/scripts" "${secrets_project}/certs")
file(COPY_FILE "${KAST_SESSION_DIR}/project.kast" "${secrets_project}/project.kast")
file(COPY_FILE "${KAST_TEST_PROJECT}/online_session.scene" "${secrets_project}/online_session.scene")
file(COPY_FILE "${KAST_TEST_PROJECT}/scripts/online_session_demo.lua" "${secrets_project}/scripts/online_session_demo.lua")
file(COPY_FILE "${KAST_TEST_PROJECT}/certs/net_test_ca.pem" "${secrets_project}/certs/net_test_ca.pem")
add_test(NAME online_secrets
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${secrets_project}"
          "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/online_secrets.run" "-DWRAPPER=${KAST_ONLINE_ADMIN_ENV};${KAST_NET_SERVER}"
          "-DSECRETS=${KAST_ONLINE_SECRETS}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/online_secrets_test.cmake")
set_tests_properties(online_secrets PROPERTIES RESOURCE_LOCK net_test_ports TIMEOUT 120)

# --- game GUI (RmlUi) and the GUI Builder ---
# Play: tests/project/ui.scene shows ui/main_menu.rml over the Game view; configure-time copies (in health_project) with
# the UIDocument hidden (must look different) and with a missing .rml (logs the file, Play goes on).
file(READ "${KAST_TEST_PROJECT}/ui.scene" ui_scene)
string(REPLACE "\"visible\": true" "\"visible\": false" ui_hidden_scene "${ui_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui_hidden.scene" "${ui_hidden_scene}")
string(REPLACE "ui/main_menu.rml" "ui/missing.rml" ui_bad_scene "${ui_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui_bad_path.scene" "${ui_bad_scene}")
add_test(NAME ui_play_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_play.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui.scene --play" "-DFORBID_OUTPUT=GUI layer framebuffer"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_play_hidden_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_play_hidden.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_hidden.scene --play"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_play_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ui_play_hidden.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ui_play.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_play_screenshot PROPERTIES FIXTURES_SETUP ui_play_shot)
set_tests_properties(ui_play_hidden_screenshot PROPERTIES FIXTURES_SETUP ui_play_hidden_shot)
set_tests_properties(ui_play_differs PROPERTIES FIXTURES_REQUIRED "ui_play_shot;ui_play_hidden_shot")
add_test(NAME ui_bad_path_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bad_path.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_bad_path.scene --play"
          "-DEXPECT_OUTPUT=UI document 'ui/missing.rml' not found" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# GUI layers: the main menu with filter: opacity(0.5) on its panel draws the panel through an offscreen layer; the plain
# menu (ui_play_screenshot) creates no layer framebuffer. Probes: 0.5 * layer + (1 - 0.5 * alpha) * scene, from
# ui_play.png's colors over ui_play_hidden.png's scene (the panel's #1b2333dd: alpha 0.87; the button and the text's
# panel opaque). The floor is outside the panel.
file(READ "${KAST_TEST_PROJECT}/ui/main_menu.rml" ui_layer_rml)
string(REPLACE "main_menu.rcss" "layer_menu.rcss" ui_layer_rml "${ui_layer_rml}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui/layer_menu.rml" "${ui_layer_rml}")
file(READ "${KAST_TEST_PROJECT}/ui/main_menu.rcss" ui_layer_rcss)
file(WRITE "${KAST_HEALTH_PROJECT}/ui/layer_menu.rcss" "${ui_layer_rcss}\n.panel { filter: opacity(0.5); }\n")
string(REPLACE "ui/main_menu.rml" "ui/layer_menu.rml" ui_layer_scene "${ui_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui_layer.scene" "${ui_layer_scene}")
add_test(NAME ui_layer_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_layer.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_layer.scene --play"
          "-DEXPECT_OUTPUT=GUI layer framebuffer 1 created"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_layer_screenshot PROPERTIES FIXTURES_SETUP ui_layer_shot)
add_test(NAME ui_layer_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_layer.png"
         0.3281,0.1806,36,45,58,4 0.4375,0.3708,146,170,200,4 0.3750,0.4861,144,144,149,4 0.2344,0.5556,233,229,224,4)
set_tests_properties(ui_layer_probe PROPERTIES FIXTURES_REQUIRED ui_layer_shot)
# GUI effects: ui_effects.scene shows ui/effects.rml (one element per effect, placed by effects.rcss). In Play the
# editor's Game view and the built player (game "effects" in KAST_GAMES) draw it at 1:1 GUI pixels, so both are probed
# with kast_effects_probes: "x,y,r,g,b,tolerance" at GUI pixels (effects.rcss), turned into --probe-png's fractions
# of a <width>x<height> screenshot whose GUI starts at (<left>, <top>). RmlUi and the GUI renderer must not warn.
# - box-shadow 0 0 12px 4px #ffaa00 (#glow): 61,41,0 2.5px outside the box, 38,25,0 at 8.5px, 3,3,0 at 17.5px; the
#   box keeps #313131. As RmlUi 6.2's GL3 backend (GeometryBoxShadow.cpp, RenderToClipMask): the shadow is drawn
#   through the box's SetInverse clip mask before its blur (sigma 6), so a 4px ring outside the border box is
#   blurred: Phi((4 - d) / 6) - Phi(-d / 6) of #ffaa00 at d px out = 0.26, 0.148, 0.010 (a browser blurs the whole
#   spread box: 0.60, 0.23, 0.01).
# - blur(4px) of #3a6fb0 (#blur) falls off across its left edge: Phi(d / 4) at d = 12.5, 6.5, 0.5, -3.5, -9.5 px
#   inside = 1, 0.95, 0.55, 0.19, 0.01.
# - grayscale r = g = b; linear-gradient ends; drop-shadow (green, offset) and its box; backdrop blur of the stripes
#   under the glass; radial center and corner; conic top; repeating-linear stripes above the glass (red 0-10px,
#   green 10-20px); then sepia, invert, hue-rotate, brightness, contrast, saturate(0) and opacity(0.5) of #3a6fb0
#   (red for grayscale and hue-rotate) by RmlUi's color matrices.
# - repeating-radial-gradient (#rradial, red 0-8px, blue 8-16px from its center) at radius 4.5, 12.5, 20.5, 27.5;
#   repeating-conic-gradient (#rconic, red 0-45deg, green 45-90deg clockwise from the top) at 22.5, 67.5, 112.5
#   and 157.5deg, radius 20.
set(KAST_EFFECTS_PROBES
    37,70,61,41,0,12 31,70,38,25,0,8 22,70,3,3,0,8 50,50,49,49,49,3
    232,70,58,111,176,3 226,70,55,105,166,4 220,70,32,61,97,8 216,70,11,21,34,8 210,70,1,1,1,4
    410,50,54,54,54,3 583,70,248,0,7,10 697,70,5,0,250,10 164,200,0,206,0,16 50,170,58,111,176,3
    232,172,136,151,32,16 460,190,252,252,3,10 402,162,13,13,242,16 640,163,253,2,0,10
    205,150,255,0,0,10 215,150,0,255,0,10 225,150,255,0,0,10 235,150,0,255,0,10
    50,290,141,126,98,3 230,290,197,144,79,3 410,290,0,113,0,3 590,290,87,167,255,3
    50,410,0,94,225,3 230,410,104,104,104,3 410,410,29,55,88,3
    614,430,255,0,0,10 622,430,0,0,255,10 630,430,255,0,0,10 637,430,0,0,255,10
    697,411,255,0,0,10 708,422,0,255,0,10 708,437,255,0,0,10 697,448,0,255,0,10)
function(kast_effects_probes out width height left top)
  set(probes "")
  foreach(probe ${KAST_EFFECTS_PROBES})
    string(REPLACE "," ";" probe "${probe}")
    list(POP_FRONT probe x y)
    set(fractions "")
    foreach(axis "${x};${left};${width}" "${y};${top};${height}")
      list(GET axis 0 at)
      list(GET axis 1 origin)
      list(GET axis 2 size)
      math(EXPR n "((2 * (${at} + ${origin}) + 1) * 10000 + ${size}) / (2 * ${size})") # the pixel's center, 4 places
      string(LENGTH "${n}" digits)
      math(EXPR zeros "4 - ${digits}")
      string(REPEAT "0" ${zeros} pad)
      list(APPEND fractions "0.${pad}${n}")
    endforeach()
    list(JOIN fractions "," fractions)
    list(JOIN probe "," color)
    list(APPEND probes "${fractions},${color}")
  endforeach()
  set(${out} ${probes} PARENT_SCOPE)
endfunction()
add_test(NAME ui_effects_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_effects.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_effects.scene --play" "-DEXPECT_OUTPUT=GUI layer framebuffer 1 created"
          "-DFORBID_OUTPUT=\\[warn\\] RmlUi|GUI filter|GUI shader|GUI effects need"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_effects_screenshot PROPERTIES FIXTURES_SETUP ui_effects_shot)
kast_effects_probes(play_effects_probes 1280 720 257 44) # the editor's Game view
add_test(NAME ui_effects_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_effects.png"
         ${play_effects_probes})
set_tests_properties(ui_effects_probe PROPERTIES FIXTURES_REQUIRED ui_effects_shot)
# The same in the built player (1280x720). backdrop-filter switches the player to a readable target.
kast_effects_probes(player_effects_probes 1280 720 0 0)
add_test(NAME player_effects_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_effects.png"
         ${player_effects_probes})
set_tests_properties(player_effects_probe PROPERTIES FIXTURES_REQUIRED player_effects_shot)
# A filter the renderer can't compile (a negative blur RmlUi parses but GuiRenderer rejects) warns exactly once however
# many frames ask for it (RmlUi adds its own "Could not compile filter on element"), and the element still draws,
# unfiltered: #3a6fb0 inside it (GUI pixel 70,60).
file(WRITE "${KAST_HEALTH_PROJECT}/ui/bad_filter.rml"
     "<rml><head><style>body { width: 100%; height: 100%; background-color: #000000; }\n"
     "#bad { position: absolute; left: 40px; top: 40px; width: 120px; height: 60px; background-color: #3a6fb0;\n"
     "       filter: blur(-4px); }</style></head><body><div id=\"bad\"></div></body></rml>\n")
string(REPLACE "ui/main_menu.rml" "ui/bad_filter.rml" ui_bad_filter_scene "${ui_scene}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui_bad_filter.scene" "${ui_bad_filter_scene}")
add_test(NAME ui_bad_filter_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bad_filter.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_bad_filter.scene --play"
          "-DEXPECT_OUTPUT=GUI filter 'blur' has invalid parameters" -DEXPECT_COUNT=1
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_bad_filter_screenshot PROPERTIES FIXTURES_SETUP ui_bad_filter_shot)
add_test(NAME ui_bad_filter_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_bad_filter.png"
         0.2555,0.1444,58,111,176,3)
set_tests_properties(ui_bad_filter_probe PROPERTIES FIXTURES_REQUIRED ui_bad_filter_shot)
# A synthetic click on the Start button publishes menu.start (Events panel) and scripts/ui_menu.lua logs it.
add_test(NAME ui_click_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_click.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui.scene --play --gui-click start_button --focus-panel Events"
          "-DEXPECT_OUTPUT=Menu start clicked: start_button in ui/main_menu.rml"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# kast.gui drag and drop (ui_drag.scene, scripts/ui_drag.lua): a scripted left drag from the Game view's centre takes
# the box onto the panel right of it (test.box.start, then test.box.drop naming the target, which lights up through
# kast.gui.setAttribute); a drag down onto empty <body> drops on no target; one with Shift held says so. The box is
# hovered before the drag. Its decorator image and the panel's <img> (PNGs) load without a warning.
add_test(NAME ui_drag_drop
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_drag_drop.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_drag.scene --play --sim-mouse 60,left,260,0,0" -DFRAMES=120
          "-DEXPECT_OUTPUT=Hovered: source.*Drag start: source in ui/drag.rml.*Drag drop: source onto 'target' in 'ui/drag.rml' shift=false"
          "-DFORBID_OUTPUT=\\[warn\\] RmlUi|GUI image|Could not load" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_drag_nowhere
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_drag_nowhere.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_drag.scene --play --sim-mouse 60,left,0,200,0" -DFRAMES=120
          "-DEXPECT_OUTPUT=Drag start: source in ui/drag.rml.*Drag drop: source onto '' in '' shift=false"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_drag_shift
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_drag_shift.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_drag.scene --play --sim-mouse 60,left+shift,260,0,0" -DFRAMES=120
          "-DEXPECT_OUTPUT=Drag drop: source onto 'target' in 'ui/drag.rml' shift=true"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Data binding and runtime elements (ui_bind.scene: ui/bind.rml driven by scripts/ui_bind.lua; the C++ side, hot reload
# included, is self_check's checkGuiDataModels). No RmlUi warning anywhere: the auto-made models satisfy every binding.
# - ui_bind_list_screenshot: 50 bound rows (nested tags, {{title}} / {{count}} / data-if), two clones and a created row,
#   shot on BIND50; ui_bind_list_shrunk_screenshot: the same after a later set of 3 rows and one clone removed (BIND3).
# - ui_bind_inputs: real typing ("hi"), a click on the checkbox, Right twice on the range (5 -> 7) and Down on the select
#   (red -> green) come back through kast.gui.get.
# - ui_bind_row_click / ui_bind_clone_click: a data-event button in a data-for row (data-attr-id) and in a clone
#   (renamed from clone_template_pick) publish bind.pick with their own ids.
set(bind_args "--scene ${KAST_TEST_PROJECT}/ui_bind.scene --play --size 1600x1000")
set(bind_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error")
add_test(NAME ui_bind_list_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_list.png"
          "-DEXTRA_ARGS=${bind_args} --screenshot-on-log 2,BIND50" -DFRAMES=120
          "-DEXPECT_OUTPUT=bind: made true true true true, BIND50" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_bind_list_shrunk_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_list_shrunk.png"
          "-DEXTRA_ARGS=${bind_args}" -DFRAMES=90
          "-DEXPECT_OUTPUT=bind: removed clone_2 true, BIND3" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_bind_list_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_list.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_list_shrunk.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_bind_list_screenshot PROPERTIES FIXTURES_SETUP ui_bind_list_shot)
set_tests_properties(ui_bind_list_shrunk_screenshot PROPERTIES FIXTURES_SETUP ui_bind_shrunk_shot)
set_tests_properties(ui_bind_list_differs PROPERTIES FIXTURES_REQUIRED "ui_bind_list_shot;ui_bind_shrunk_shot")
add_test(NAME ui_bind_inputs
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_inputs.png"
          "-DEXTRA_ARGS=${bind_args} --gui-click form_agree --sim-on-log KEYS --sim-key 5,H --sim-key 10,I --sim-key 50,Right --sim-key 55,Right --sim-key 80,Down"
          -DFRAMES=130 "-DEXPECT_OUTPUT=bind: two-way name=hi agree=true volume=7 color=green" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_bind_row_click
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_row_click.png"
          "-DEXTRA_ARGS=${bind_args} --gui-click pick_41" -DFRAMES=30
          "-DEXPECT_OUTPUT=bind: picked pick_41 in ui/bind.rml" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_bind_clone_click
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_bind_clone_click.png"
          "-DEXTRA_ARGS=${bind_args} --gui-click clone_2_pick" -DFRAMES=30
          "-DEXPECT_OUTPUT=bind: picked clone_2_pick in ui/bind.rml" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The built game (game_projects/ui, startScene ui.scene, see KAST_GAMES) loads the GUI from game.pak; a click there
# reaches Lua too.
add_test(NAME player_ui_click
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/ui" "-DEXE=UI Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/ui_click.run"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_ui_click.png" "-DEXTRA_ARGS=--gui-click start_button"
          -DEXPECT=0 "-DEXPECT_OUTPUT=Menu start clicked: start_button in ui/main_menu.rml"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_ui_click PROPERTIES FIXTURES_REQUIRED game_ui_built TIMEOUT 90)
# The built game "bind" (ui_bind.scene from game.pak): the data models load without an RmlUi warning in the player too,
# and a click on a data-for row's button reaches Lua.
add_test(NAME player_bind_click
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/bind" "-DEXE=Bind Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/bind_click.run"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_bind_click.png" "-DEXTRA_ARGS=--gui-click pick_41"
          -DEXPECT=0 "-DEXPECT_OUTPUT=bind: made true true true true, BIND50.*bind: picked pick_41 in ui/bind.rml"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_bind_click PROPERTIES FIXTURES_REQUIRED game_bind_built TIMEOUT 90)

# Form widgets (ui_widgets.scene: ui/widgets.rml driven by scripts/ui_widgets.lua; the C++ side is self_check's
# checkGuiWidgets). widgets.rcss has no widget rules: the engine's default widget style draws them.
# - ui_widgets_screenshot: Play; the values read back typed (VALUES), setValue snapping, clamping and validating without
#   publishing (SETVALUE), the progress bar driven by setValue (PROGRESS), the bound values (BOUND); no RmlUi warning,
#   no change event without input.
#   ui_widgets_probe reads the default style's #ffaa00 on the toggle, the progress fill and a spinbox button.
# - ui_widgets_restyled_screenshot: the same GUI in health_project, whose widgets.rcss overrides those colours (#3060ff):
#   it differs, and ui_widgets_restyled_probe reads the project's colour where the default's was.
# - ui_widgets_toggle / _spinbox: a --gui-click on the toggle and on the spinbox's + publish their change events.
# - ui_widgets_keys: Up, Up, Down on the focused spinbox, Down on the dropdown, Right on the slider.
# - ui_widgets_drag: a mouse drag on the slider publishes on the press and again during the drag.
set(widgets_args "--scene ${KAST_TEST_PROJECT}/ui_widgets.scene --play --size 1600x1000")
set(widgets_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error|widgets\\.loading")
add_test(NAME ui_widgets_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets.png"
          "-DEXTRA_ARGS=${widgets_args}"
          "-DEXPECT_OUTPUT=widgets: VALUES sound=true.boolean. volume=20.number. quality=medium.string. count=3.number. scale=1.number. loading=0.025.number. copy=3.number. made=5.number. true true.*widgets: SETVALUE volume=100.number. count=7.number. scale=0.25.number. quality=high.string. sound=false.boolean. loading=1.number. raised=true.*widgets: PROGRESS loading=0.75.number..*widgets: BOUND level=40.number. amount=6.number. loaded=0.6.number. music=true.boolean."
          "-DFORBID_OUTPUT=${widgets_forbid}|widgets: widgets\\." -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_widgets_screenshot PROPERTIES FIXTURES_SETUP ui_widgets_shot)
add_test(NAME ui_widgets_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_widgets.png" 0.335,0.152,255,170,0,12 0.35,0.322,255,170,0,12 0.39,0.256,255,170,0,12)
set_tests_properties(ui_widgets_probe PROPERTIES FIXTURES_REQUIRED ui_widgets_shot)
file(READ "${KAST_TEST_PROJECT}/ui/widgets.rcss" widgets_rcss)
file(WRITE "${KAST_HEALTH_PROJECT}/ui/widgets.rcss" "${widgets_rcss}
input.toggle:checked, progress fill, .spinbox button { background-color: #3060ff; }
")
add_test(NAME ui_widgets_restyled_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_restyled.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_widgets.scene --play --size 1600x1000"
          "-DFORBID_OUTPUT=${widgets_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_widgets_restyled_screenshot PROPERTIES FIXTURES_SETUP ui_widgets_restyled_shot)
add_test(NAME ui_widgets_restyled_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_restyled.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_widgets_restyled_differs PROPERTIES FIXTURES_REQUIRED "ui_widgets_shot;ui_widgets_restyled_shot")
add_test(NAME ui_widgets_restyled_probe
  COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_restyled.png" 0.335,0.152,48,96,255,12 0.35,0.322,48,96,255,12 0.39,0.256,48,96,255,12)
set_tests_properties(ui_widgets_restyled_probe PROPERTIES FIXTURES_REQUIRED ui_widgets_restyled_shot)
add_test(NAME ui_widgets_toggle
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_toggle.png"
          "-DEXTRA_ARGS=${widgets_args} --gui-click sound" -DFRAMES=30
          "-DEXPECT_OUTPUT=widgets: widgets.sound sound = false.boolean. in ui/widgets.rml" "-DFORBID_OUTPUT=${widgets_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_widgets_spinbox
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_spinbox.png"
          "-DEXTRA_ARGS=${widgets_args} --gui-click count_inc" -DFRAMES=30
          "-DEXPECT_OUTPUT=widgets: widgets.count count = 4.number. in ui/widgets.rml" -DEXPECT_COUNT=1
          "-DFORBID_OUTPUT=${widgets_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_widgets_keys
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_keys.png"
          "-DEXTRA_ARGS=${widgets_args} --sim-on-log KEYS --sim-key 5,Up --sim-key 10,Up --sim-key 15,Down --sim-key 45,Down --sim-key 65,Right"
          -DFRAMES=130
          "-DEXPECT_OUTPUT=widgets.count count = 4.number.*widgets.count count = 5.number.*widgets.count count = 4.number.*widgets.quality quality = high.string.*widgets.volume volume = 21.number."
          "-DFORBID_OUTPUT=${widgets_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_widgets_drag
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_widgets_drag.png"
          "-DEXTRA_ARGS=${widgets_args} --sim-mouse 80,left,60,0,0,-200,-185" -DFRAMES=110
          "-DEXPECT_OUTPUT=widgets.volume volume = [0-9]+.number.*widgets.volume volume = [0-9]+.number." "-DFORBID_OUTPUT=${widgets_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The built game "widgets" (ui_widgets.scene from game.pak): the default widget style is the player's own (no project
# sheet styles the widgets), the GUI loads with no RmlUi warning, and the spinbox's + reaches Lua.
add_test(NAME player_widgets_click
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/widgets" "-DEXE=Widgets Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/widgets_click.run"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_widgets_click.png" "-DEXTRA_ARGS=--gui-click count_inc"
          -DEXPECT=0 "-DEXPECT_OUTPUT=widgets: VALUES sound=true.*widgets: widgets.count count = 4.number. in ui/widgets.rml"
          "-DFORBID_OUTPUT=${widgets_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_widgets_click PROPERTIES FIXTURES_REQUIRED game_widgets_built TIMEOUT 90)
add_test(NAME player_widgets_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_widgets.png" 0.1875,0.157,255,170,0,12 0.203,0.393,255,170,0,12 0.2563,0.3014,255,170,0,12)
set_tests_properties(player_widgets_probe PROPERTIES FIXTURES_REQUIRED player_widgets_shot)

# Lists (ui_lists.scene: ui/lists.rml driven by scripts/ui_lists.lua; the C++ side is self_check's checkGuiLists, which
# also counts the Virtual List's live rows and times its scrolling). lists.rcss has no list rules: the engine's default
# style draws them.
# - ui_lists_screenshot: Play; the values read back (VALUES), setValue by value, key and index, its refusals logged
#   without the value and bad argument types raising (SETVALUE, CLEARED), scrollTo's refusals (SCROLLTO); no event
#   without input, no RmlUi warning. The shot shows the Virtual List scrolled down to item 50000, selected, at its bottom.
#   ui_lists_probe reads the default style's #ffaa00 on the selected rows (Rogue, Item 50000) and #313131 on a list.
# - ui_lists_restyled_screenshot: the same GUI in health_project, whose lists.rcss overrides the selected rows' colour
#   (#3060ff): it differs, and ui_lists_restyled_probe reads the project's colour where the default's was.
# - ui_lists_click / _click_virtual / _click_bound: a --gui-click on an authored row, a Virtual List row and a data-for
#   row publish their values.
# - ui_lists_keys: Down, End, Home, PageDown on the focused Virtual List.
# - ui_lists_wheel: a wheel turn over the Virtual List scrolls it: the shot differs from ui_lists_screenshot.
# - ui_lists_follow: scrollToEnd, then three appends it follows (the shot ends on Item 100003); ui_lists_follow_kept:
#   scrolled back up, the fourth append leaves it there.
set(lists_args "--scene ${KAST_TEST_PROJECT}/ui_lists.scene --play --size 1600x1000")
set(lists_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error|paladin")
add_test(NAME ui_lists_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists.png"
          "-DEXTRA_ARGS=${lists_args}" -DFRAMES=60
          "-DEXPECT_OUTPUT=lists: VALUES classes=mage.string. log=nil.nil. big=nil.nil..*kast.gui.setValue..classes..: the list has no row with that value: unchanged.*kast.gui.setValue..big..: the list has no row at that index: unchanged.*lists: SETVALUE classes=rogue.string. log=2.string. big=50000.string. 50000.string. raised=true true true true.*lists: CLEARED log=nil.nil..*kast.gui.scrollTo..big., 100000.: the list has no row at that index: unchanged.*kast.gui.scrollTo..nowhere., 1.: no loaded GUI has it.*lists: SCROLLTO"
          "-DFORBID_OUTPUT=${lists_forbid}|lists: lists\\." -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_lists_screenshot PROPERTIES FIXTURES_SETUP ui_lists_shot)
add_test(NAME ui_lists_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_lists.png" 0.28,0.168,255,170,0,12 0.6,0.6,255,170,0,12 0.62,0.4,49,49,49,12)
set_tests_properties(ui_lists_probe PROPERTIES FIXTURES_REQUIRED ui_lists_shot)
file(READ "${KAST_TEST_PROJECT}/ui/lists.rcss" lists_rcss)
file(WRITE "${KAST_HEALTH_PROJECT}/ui/lists.rcss" "${lists_rcss}
.listbox .row.selected, .virtuallist .row.selected { background-color: #3060ff; }
")
add_test(NAME ui_lists_restyled_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_restyled.png"
          "-DEXTRA_ARGS=--scene ${KAST_HEALTH_PROJECT}/ui_lists.scene --play --size 1600x1000" -DFRAMES=60
          "-DFORBID_OUTPUT=${lists_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_lists_restyled_screenshot PROPERTIES FIXTURES_SETUP ui_lists_restyled_shot)
add_test(NAME ui_lists_restyled_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ui_lists.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_restyled.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_lists_restyled_differs PROPERTIES FIXTURES_REQUIRED "ui_lists_shot;ui_lists_restyled_shot")
add_test(NAME ui_lists_restyled_probe
  COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/ui_lists_restyled.png" 0.28,0.168,48,96,255,12 0.6,0.6,48,96,255,12)
set_tests_properties(ui_lists_restyled_probe PROPERTIES FIXTURES_REQUIRED ui_lists_restyled_shot)
foreach(click "click|class_hunter|lists.classes classes = hunter.string." "click_virtual|big_50003|lists.big big = 50003.string."
              "click_bound|line_2|lists.log log = 2.string.")
  string(REPLACE "|" ";" click "${click}")
  list(GET click 0 name)
  list(GET click 1 id)
  list(GET click 2 expect)
  add_test(NAME ui_lists_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_${name}.png"
            "-DEXTRA_ARGS=${lists_args} --gui-click ${id}" -DFRAMES=30
            "-DEXPECT_OUTPUT=lists: ${expect} in ui/lists.rml" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=${lists_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME ui_lists_keys
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_keys.png"
          "-DEXTRA_ARGS=${lists_args} --sim-on-log KEYS --sim-key 5,Down --sim-key 10,End --sim-key 15,Home --sim-key 20,PageDown"
          -DFRAMES=60
          "-DEXPECT_OUTPUT=lists.big big = 50001.string.*lists.big big = 100000.string.*lists.big big = 1.string.*lists.big big = 21.string."
          "-DFORBID_OUTPUT=${lists_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_lists_wheel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_wheel.png"
          "-DEXTRA_ARGS=${lists_args} --sim-mouse 30,none,0,0,-5,70,10" -DFRAMES=60 "-DFORBID_OUTPUT=${lists_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_lists_wheel_screenshot PROPERTIES FIXTURES_SETUP ui_lists_wheel_shot)
add_test(NAME ui_lists_wheel_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ui_lists.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_wheel.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_lists_wheel_differs PROPERTIES FIXTURES_REQUIRED "ui_lists_shot;ui_lists_wheel_shot")
add_test(NAME ui_lists_follow_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_follow.png"
          "-DEXTRA_ARGS=${lists_args}" -DFRAMES=210 "-DEXPECT_OUTPUT=lists: APPEND items=100003"
          "-DFORBID_OUTPUT=${lists_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_lists_follow_kept_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_lists_follow_kept.png"
          "-DEXTRA_ARGS=${lists_args}" -DFRAMES=250 "-DEXPECT_OUTPUT=lists: APPEND items=100004.*lists: APPEND kept"
          "-DFORBID_OUTPUT=${lists_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The built game "lists" (ui_lists.scene from game.pak): the default list style is the player's own, the GUI loads with
# no RmlUi warning, and a click on a row reaches Lua.
add_test(NAME player_lists_click
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/lists" "-DEXE=Lists Test.exe" "-DWORK_DIR=${KAST_GAME_OUT}/lists_click.run"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_lists_click.png" "-DEXTRA_ARGS=--gui-click class_hunter"
          -DEXPECT=0 "-DEXPECT_OUTPUT=lists: VALUES classes=mage.*lists: lists.classes classes = hunter.string. in ui/lists.rml"
          "-DFORBID_OUTPUT=${lists_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_lists_click PROPERTIES FIXTURES_REQUIRED game_lists_built TIMEOUT 90)
add_test(NAME player_lists_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_lists.png" 0.156,0.178,255,170,0,12 0.53,0.778,255,170,0,12 0.594,0.417,49,49,49,12)
set_tests_properties(player_lists_probe PROPERTIES FIXTURES_REQUIRED player_lists_shot)

# Builder: main_menu.rml open with start_button selected (tree, Inspector, Style, Preview with the outline), and the same
# GUI in health_project with a different .rcss (wider panel, red buttons) must render differently. --gui-pick clicks the
# preview (1280x720 preview pixels) on the Quit button, which selects it.
set(builder_args "--open-gui ui/main_menu.rml --size 1600x1000")
file(READ "${KAST_TEST_PROJECT}/ui/main_menu.rcss" ui_rcss)
string(REPLACE "width: 420px;" "width: 640px;" ui_rcss "${ui_rcss}")
string(REPLACE "#3a6fb0" "#c03030" ui_rcss "${ui_rcss}")
file(WRITE "${KAST_HEALTH_PROJECT}/ui/main_menu.rcss" "${ui_rcss}")
add_test(NAME gui_builder_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select start_button"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_restyled_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_restyled.png"
          "-DEXTRA_ARGS=--project ${KAST_HEALTH_PROJECT} ${builder_args} --gui-select start_button"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_restyle_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/gui_builder.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_restyled.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(gui_builder_screenshot PROPERTIES FIXTURES_SETUP gui_builder_shot)
set_tests_properties(gui_builder_restyled_screenshot PROPERTIES FIXTURES_SETUP gui_builder_restyled_shot)
set_tests_properties(gui_builder_restyle_differs PROPERTIES FIXTURES_REQUIRED "gui_builder_shot;gui_builder_restyled_shot")
add_test(NAME gui_builder_pick_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_pick.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-pick 640,277"
          "-DEXPECT_OUTPUT=--gui-pick 640,277 selected 'quit_button'"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_pick_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/gui_builder.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_pick.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(gui_builder_pick_screenshot PROPERTIES FIXTURES_SETUP gui_builder_pick_shot)
set_tests_properties(gui_builder_pick_differs PROPERTIES FIXTURES_REQUIRED "gui_builder_shot;gui_builder_pick_shot")
# The GUI Builder previews ui/effects.rml through the same renderer: no RmlUi or GUI renderer warning, and probes
# (preview at 1280x720 scaled into the panel) show the glow outside the box, the box keeping #313131, and the linear
# gradient's ends.
add_test(NAME gui_builder_effects_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_effects.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/effects.rml --size 1600x1000"
          "-DEXPECT_OUTPUT=GUI layer framebuffer 1 created" "-DFORBID_OUTPUT=\\[warn\\] RmlUi|GUI filter|GUI shader|GUI effects need"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(gui_builder_effects_screenshot PROPERTIES FIXTURES_SETUP gui_builder_effects_shot)
add_test(NAME gui_builder_effects_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/gui_builder_effects.png"
         0.2184,0.1675,61,41,0,12 0.2153,0.1675,38,25,0,8 0.2103,0.1675,1,1,0,8 0.2253,0.1505,49,49,49,3
         0.4734,0.1675,247,0,8,10 0.5266,0.1675,5,0,250,10)
set_tests_properties(gui_builder_effects_probe PROPERTIES FIXTURES_REQUIRED gui_builder_effects_shot)
# The Inspector's UI Document (path combo, Visible) on the Main Menu entity; ui.scene survives save/load.
add_test(NAME ui_document_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_document_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui.scene --select 5" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME ui_scene_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_PROJECT}/ui.scene"
          "-DOUT_DIR=${KAST_HEALTH_PROJECT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
# Assets > Create > GUI on a fresh copy of tests/project, in ui/ (which holds main_menu.*): writes new_gui.rml + .rcss,
# the .rml linking its .rcss, and starts the rename. The Create submenu (with GUI) via --assets-menu.
set(KAST_NEW_GUI_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/new_gui_project")
add_test(NAME new_gui_project_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KAST_NEW_GUI_PROJECT}")
add_test(NAME new_gui_project_copy COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${KAST_NEW_GUI_PROJECT}")
add_test(NAME assets_create_gui_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/assets_create_gui.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --assets-create gui:ui" "-DFOCUS_PANEL=Assets"
          "-DMUST_EXIST=${KAST_NEW_GUI_PROJECT}/ui/new_gui.rcss" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME assets_create_gui_written COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/new_gui.rml")
set_tests_properties(assets_create_gui_written PROPERTIES PASS_REGULAR_EXPRESSION "href=\"new_gui.rcss\"")
set_tests_properties(new_gui_project_clean PROPERTIES FIXTURES_SETUP new_gui_clean)
set_tests_properties(new_gui_project_copy PROPERTIES FIXTURES_REQUIRED new_gui_clean FIXTURES_SETUP new_gui_copy)
set_tests_properties(assets_create_gui_screenshot PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP new_gui_made)
set_tests_properties(assets_create_gui_written PROPERTIES FIXTURES_REQUIRED new_gui_made)
add_test(NAME assets_create_menu_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/assets_create_menu.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --assets-menu /" "-DFOCUS_PANEL=Assets"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# Tree edit -> preview -> Save, through the same deferred edit and Save as the GUI Hierarchy's buttons: --gui-add Button
# into Panel #buttons shows a third button and the dirty marker (nothing saved); on the new_gui_project copy, --gui-save
# then writes it, keeping the comment, the unknown <span> and the unedited title attribute.
add_test(NAME gui_builder_add_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_add.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select buttons --gui-add Button"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# GUI Builder undo (docs/editor/undo-redo.html): the --gui-add step undone and redone through the builder's history.
add_test(NAME gui_builder_undo_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_undo.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select buttons --gui-add Button --gui-undo 1 --gui-redo 1"
          "-DEXPECT_OUTPUT=Undo Add Button.*Redo Add Button" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_save
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_save.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} ${builder_args} --gui-select buttons --gui-add Button --gui-save"
          "-DEXPECT_OUTPUT=Saved GUI" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/main_menu.rml")
set_tests_properties(gui_builder_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "<!-- The game's main menu.*<span class=\"accent\">awaits</span>.*title=\"Begin a new game\".*<button>Button</button>")
set_tests_properties(gui_builder_save PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP gui_saved)
set_tests_properties(gui_builder_saved_file PROPERTIES FIXTURES_REQUIRED gui_saved)

# Data binding in the builder: ui/bind.rml (data-model, data-for, two-way inputs) previews with no RmlUi warning (empty
# models, rows not shown); on the new_gui_project copy the Inspector's data-for of row_template is edited
# (--gui-set-attr, one step as typing it does), undone, redone and saved.
add_test(NAME gui_builder_bind_preview_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_bind_preview.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/bind.rml --size 1600x1000 --gui-select row_template"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_bind_edit
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_bind_edit.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/bind.rml --size 1600x1000 --gui-select row_template --gui-set-attr data-for=row:shown --gui-undo 1 --gui-redo 1 --gui-save"
          "-DEXPECT_OUTPUT=--gui-set-attr data-for=.row:shown. on 'row_template'.*Undo Edit Panel data-for.*Redo Edit Panel data-for.*Saved GUI"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_bind_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/bind.rml")
set_tests_properties(gui_builder_bind_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "id=\"row_template\" class=\"bind_row\" data-for=\"row:shown\" data-attr-id=")
set_tests_properties(gui_builder_bind_edit PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP gui_bind_saved)
set_tests_properties(gui_builder_bind_saved_file PROPERTIES FIXTURES_REQUIRED gui_bind_saved)

# Form widgets in the builder (ui/widgets.rml, no widget rules in its sheet: the preview wears the default widget
# style): each kind selected (one tree node, its Inspector fields, the preview outline), with no RmlUi warning; each kind
# added to main_menu.rml's #buttons Panel through the Hierarchy's deferred add, undone and redone; on the new_gui_project
# copy, a widget property (the spinbox's value, kept on its <input>) set as the Inspector does, undone, redone, saved,
# and the saved GUI reopened with it.
foreach(widget sound volume quality count loading)
  add_test(NAME gui_builder_widget_${widget}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_widget_${widget}.png"
            "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/widgets.rml --size 1600x1000 --gui-select ${widget}"
            "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
foreach(kind Toggle Slider Dropdown Spinbox Progress)
  add_test(NAME gui_builder_add_${kind}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_add_${kind}.png"
            "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select buttons --gui-add ${kind} --gui-undo 1 --gui-redo 1"
            "-DEXPECT_OUTPUT=Undo Add ${kind}.*Redo Add ${kind}" "-DFORBID_OUTPUT=${bind_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME gui_builder_widget_edit
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_widget_edit.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/widgets.rml --size 1600x1000 --gui-select count --gui-set-prop value=7 --gui-undo 1 --gui-redo 1 --gui-save"
          "-DEXPECT_OUTPUT=--gui-set-prop value=.7. on 'count'.*Undo Edit Spinbox value.*Redo Edit Spinbox value.*Saved GUI"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_widget_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/widgets.rml")
set_tests_properties(gui_builder_widget_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "<!-- Form widgets.*<input type=\"text\" id=\"count_input\" value=\"7\" />.*data-attr-id=\"'option_' [+] it_index\"")
add_test(NAME gui_builder_widget_reopen_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_widget_reopen.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/widgets.rml --size 1600x1000 --gui-select count"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(gui_builder_widget_edit PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP gui_widget_saved)
set_tests_properties(gui_builder_widget_saved_file gui_builder_widget_reopen_screenshot PROPERTIES FIXTURES_REQUIRED gui_widget_saved)

# Lists in the builder (ui/lists.rml, no list rules in its sheet: the preview wears the default style; the Virtual List
# shows 20 placeholder rows made from its template): each list selected (one tree node; its Inspector fields, rows
# editor and the preview outline), with no RmlUi warning; each kind added to main_menu.rml's #buttons Panel through the
# Hierarchy's deferred add, undone and redone; on the new_gui_project copy, the Virtual List's row-height set as the
# Inspector does, undone, redone, saved (the rest of the file unchanged), and the saved GUI reopened with it.
foreach(list classes log big)
  add_test(NAME gui_builder_list_${list}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_list_${list}.png"
            "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/lists.rml --size 1600x1000 --gui-select ${list}"
            "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
foreach(kind "ListBox|List Box" "VirtualList|Virtual List")
  string(REPLACE "|" ";" kind "${kind}")
  list(GET kind 0 flag)
  list(GET kind 1 label)
  add_test(NAME gui_builder_add_${flag}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_add_${flag}.png"
            "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select buttons --gui-add ${flag} --gui-undo 1 --gui-redo 1"
            "-DEXPECT_OUTPUT=Undo Add ${label}.*Redo Add ${label}" "-DFORBID_OUTPUT=${bind_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME gui_builder_list_edit
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_list_edit.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/lists.rml --size 1600x1000 --gui-select big --gui-set-prop row-height=30 --gui-undo 1 --gui-redo 1 --gui-save"
          "-DEXPECT_OUTPUT=--gui-set-prop row-height=.30. on 'big'.*Undo Edit Virtual List row-height.*Redo Edit Virtual List row-height.*Saved GUI"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_list_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/lists.rml")
set_tests_properties(gui_builder_list_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "<!-- Lists .*<div class=\"row selected\" id=\"class_mage\" data-value=\"mage\">Mage</div>.*data-for=\"line : lines\".*<div class=\"virtuallist\" id=\"big\" data-row-height=\"30\" data-key=\"id\" data-event=\"lists.big\" data-follow=\"\">.*data-for=\"item : items\" data-attr-id=\"'big_' [+] item.id\"")
add_test(NAME gui_builder_list_reopen_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_list_reopen.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/lists.rml --size 1600x1000 --gui-select big"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(gui_builder_list_edit PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP gui_list_saved)
set_tests_properties(gui_builder_list_saved_file gui_builder_list_reopen_screenshot PROPERTIES FIXTURES_REQUIRED gui_list_saved)

# Rich text (ui_richtext.scene: ui/richtext.rml driven by scripts/ui_richtext.lua; the C++ side is self_check's
# checkGuiRichText, checkGuiRichTextScene and checkGuiDocRichText, kast_self_check --richtext): every BBCode tag, images,
# the project font tests/project/fonts/LatoLatin-Regular.ttf, malformed markup, kast.gui.setRichText / setText and a
# data-rich binding, with no RmlUi warning; --sim-mouse clicks the link at the Game view's centre and the Lua script
# logs the gui.link it publishes. The Game view must match the committed reference tests/reference/ui_richtext.png (its
# 955x652 pixels at 322,44 of the 1600x1000 shot). The built game loads the project font from game.pak.
set(richtext_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error")
add_test(NAME ui_richtext_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_richtext.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_richtext.scene --play --size 1600x1000 --sim-mouse 60,left,0,0,0"
          "-DEXPECT_OUTPUT=Loaded font face 'LatoLatin' \\[regular\\].*richtext: SET.*richtext: REBOUND.*richtext: RICHLINK target=quest_log id=rich_center document=ui/richtext.rml"
          "-DFORBID_OUTPUT=${richtext_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_richtext_screenshot PROPERTIES FIXTURES_SETUP ui_richtext_shot)
add_test(NAME ui_richtext_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/ui_richtext.png"
          "${CMAKE_CURRENT_BINARY_DIR}/ui_richtext.png" 1.0 0,0,955,652,322,44)
set_tests_properties(ui_richtext_matches_reference PROPERTIES FIXTURES_REQUIRED ui_richtext_shot)
add_test(NAME player_richtext_fonts
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/richtext" "-DEXE=Rich Text Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/richtext_fonts.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_richtext_fonts.png"
          -DEXPECT=0 "-DEXPECT_OUTPUT=Loaded font face 'LatoLatin' \\[regular\\] from '[^']*fonts/LatoLatin-Regular.ttf'.*richtext: SET.*richtext: REBOUND"
          "-DFORBID_OUTPUT=${richtext_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_richtext_fonts PROPERTIES FIXTURES_REQUIRED game_richtext_built TIMEOUT 90)
# Rich Text in the builder: richtext.rml with rich_malformed selected (the markup field, its toolbar and the converter's
# warnings in the Inspector; the preview converted, with the project font and images, and no RmlUi warning); a Rich
# Text added to main_menu.rml's #buttons through the Hierarchy's deferred add, undone and redone; on the
# new_gui_project copy, the toolbar's [b] around rich_lua's first word (bytes 0..7), undone, redone and saved: the
# markup saved verbatim, the other rich texts unchanged.
add_test(NAME gui_builder_richtext_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_richtext.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/richtext.rml --size 1600x1000 --gui-select rich_malformed"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_add_RichText_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_add_RichText.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} ${builder_args} --gui-select buttons --gui-add RichText --gui-undo 1 --gui-redo 1"
          "-DEXPECT_OUTPUT=Undo Add Rich Text.*Redo Add Rich Text" "-DFORBID_OUTPUT=${bind_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_rich_edit
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_rich_edit.png"
          "-DEXTRA_ARGS=--project ${KAST_NEW_GUI_PROJECT} --open-gui ui/richtext.rml --size 1600x1000 --gui-select rich_lua --gui-rich-wrap b,0,7 --gui-undo 1 --gui-redo 1 --gui-save"
          "-DEXPECT_OUTPUT=--gui-rich-wrap \\[b\\] 0..7 on 'rich_lua'.*Undo Rich Text \\[b\\].*Redo Rich Text \\[b\\].*Saved GUI"
          "-DFORBID_OUTPUT=${bind_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_builder_rich_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_NEW_GUI_PROJECT}/ui/richtext.rml")
set_tests_properties(gui_builder_rich_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "<!-- Rich text .*\\[b\\]Bold\\[/b\\] \\[i\\]Italic\\[/i\\].*Line one.Line two: &lt;b&gt; &amp; \"quotes\" stay text</p>.*\\[blink\\]x\\[/blink\\] \\[color=nope\\]c.*<p id=\"rich_lua\" class=\"rich\">\\[b\\]waiting\\[/b\\] for Lua</p>.*data-rich=\"line\"")
set_tests_properties(gui_builder_rich_edit PROPERTIES FIXTURES_REQUIRED new_gui_copy FIXTURES_SETUP gui_rich_saved)
set_tests_properties(gui_builder_rich_saved_file PROPERTIES FIXTURES_REQUIRED gui_rich_saved)

# GUI prefabs: builder create / place / override / revert / apply / unpack / missing / cycle / locked, Play, player.
include(cmake/gui_prefab_tests.cmake)

# Crash dialogs: --crash makes the editor fail on purpose; with KAST_NO_CRASH_DIALOGS=1 it must exit, not wait on a
# "Debug Error!" dialog. Debug builds also report the failed assert on stderr (release compiles assert() out).
add_test(NAME crash_abort_no_dialog
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" -DCRASH=abort
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/crash_exits_test.cmake")
add_test(NAME crash_assert_no_dialog
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" -DCRASH=assert
          "-DEXPECT_OUTPUT=$<$<CONFIG:Debug>:Assertion failed>"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/crash_exits_test.cmake")

# --frames runs (editor, Project Manager, built game) show their window without taking focus from the user's app.
set(KAST_NO_FOCUS_DIR "${CMAKE_CURRENT_BINARY_DIR}/no_focus.run")
file(MAKE_DIRECTORY "${KAST_NO_FOCUS_DIR}")
add_test(NAME editor_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_editor>" --frames 120 --projects-file "${KAST_NO_FOCUS_DIR}/projects.json"
  WORKING_DIRECTORY "${KAST_NO_FOCUS_DIR}")
add_test(NAME launcher_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_editor>" --launcher --frames 120 --projects-file "${KAST_NO_FOCUS_DIR}/projects.json"
  WORKING_DIRECTORY "${KAST_NO_FOCUS_DIR}")
add_test(NAME player_no_focus
  COMMAND kast_focus_probe "${KAST_GAME_OUT}/switch/Switch Test.exe" --frames 120
  WORKING_DIRECTORY "${KAST_GAME_OUT}/switch")
add_test(NAME editor_audio_play_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_editor>" --frames 120 --projects-file "${KAST_NO_FOCUS_DIR}/projects.json"
          --scene "${KAST_AUDIO}" --play --no-audio-device --focus-panel Mixer
  WORKING_DIRECTORY "${KAST_NO_FOCUS_DIR}")
set_tests_properties(editor_no_focus launcher_no_focus editor_audio_play_no_focus PROPERTIES TIMEOUT 120 RESOURCE_LOCK no_focus_dir)
set_tests_properties(player_no_focus PROPERTIES FIXTURES_REQUIRED game_switch_built TIMEOUT 90)

include(cmake/zone_tests.cmake)

# samples/starter-mmo: the editor + services sample project.
include(cmake/starter_mmo_tests.cmake)

# GPU particles: screenshots read back at known pixels, and kast_self_check --particles.
include(cmake/particle_tests.cmake)

# Shader Graph panels: --open-graph / --graph-* screenshots, Save, Assets > Create > Shader Graph, reload on disk change;
# the graph samples (shader_graphs.scene) against the hand-written ones, in the editor and the player.
include(cmake/shader_graph_tests.cmake)

# Image-based lighting from the sky and reflection probes: scenes, variants, bake, player.
include(cmake/ibl_tests.cmake)
include(cmake/lightmap_tests.cmake)
include(cmake/voxelgi_tests.cmake)
include(cmake/sdfgi_tests.cmake)

# Screen-space effects: G-data, SSAO, SSR, depth of field, volumetric fog (post_process.h).
include(cmake/screen_space_tests.cmake)

# Game GUI resolution and DPI scaling, display modes, kast.gui scaling / kast.display.
include(cmake/gui_scaling_tests.cmake)

# Keep last: every test above runs with crash dialogs off, so a failing test exits instead of waiting for a click.
get_property(kast_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${kast_tests} APPEND PROPERTY ENVIRONMENT KAST_NO_CRASH_DIALOGS=1)
# Windowed tests stay off the main monitor: at this desktop position ("X,Y", e.g. -1900,40), or, empty (the default),
# on the first display that isn't the primary one (KAST_WINDOW_POS=secondary).
set(KAST_TEST_WINDOW_POS "" CACHE STRING "Top-left desktop position of test windows (X,Y); empty = first non-primary display")
if(KAST_TEST_WINDOW_POS)
  set_property(TEST ${kast_tests} APPEND PROPERTY ENVIRONMENT "KAST_WINDOW_POS=${KAST_TEST_WINDOW_POS}")
else()
  set_property(TEST ${kast_tests} APPEND PROPERTY ENVIRONMENT "KAST_WINDOW_POS=secondary")
endif()
