# SDFGI (docs/engine/sdfgi.html), included from CMakeLists.txt. sdfgi_outdoor.scene: the hills map's terrain (map 3,
# radius 1) around a plinth of Static boxes (white floor, back wall and an overhang roofing its back half, a red and a
# green side wall, a white pillar), a Dynamic ball, the sun, the procedural sky and SDFGI on (Medium), the Main Camera
# flown along a path in Play (scripts/sdfgi_camera_path.lua). Configure-time copies in the shadow project change one
# thing each. Shots of the Game view in Play (the terrain streams in Play only) as the VoxelGI tests take theirs
# (cmake/voxelgi_tests.cmake): 1600x900, fixed dt, the Assets tab over the Console.
set(KAST_SG_SCENE "${KAST_TEST_PROJECT}/sdfgi_outdoor.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_SG_SCENE}")
file(READ "${KAST_SG_SCENE}" sg_scene)
kast_plain_asset_refs(sg_scene)
file(COPY_FILE "${KAST_SG_SCENE}" "${KAST_SHADOW_PROJECT}/sdfgi_outdoor.scene")
set(sg_path "\"systems\": \\[\"scripts/sdfgi_camera_path.lua\"\\]")
set(sg_on "\"sdfgi\": { \"enabled\": true")
set(sg_sun "\"intensity\": 1.1, \"maxDistance\"")
# The camera held still (most checks), SDFGI off, the presets; lit by the sky alone (sky occlusion); without the Dynamic
# ball; SDFGI off along the path.
string(REGEX REPLACE "${sg_path}" "\"systems\": []" sg_still "${sg_scene}")
kast_shadow_variant(sdfgi_still "${sg_scene}" "${sg_path}" "\"systems\": []")
kast_shadow_variant(sdfgi_still_off "${sg_still}" "${sg_on}" "\"sdfgi\": { \"enabled\": false")
kast_shadow_variant(sdfgi_still_low "${sg_still}" "\"quality\": \"medium\"" "\"quality\": \"low\"")
kast_shadow_variant(sdfgi_still_high "${sg_still}" "\"quality\": \"medium\"" "\"quality\": \"high\"")
string(REGEX REPLACE "${sg_sun}" "\"intensity\": 0, \"maxDistance\"" sg_sky "${sg_still}")
file(WRITE "${KAST_SHADOW_PROJECT}/sdfgi_sky.scene" "${sg_sky}")
kast_shadow_variant(sdfgi_sky_off "${sg_sky}" "${sg_on}" "\"sdfgi\": { \"enabled\": false")
kast_shadow_variant(sdfgi_no_ball "${sg_still}" "[\r\n]+    {[\r\n]+      \"id\": 9,[^{]*{[^{]*{[^}]*},[^}]*}[\r\n]+      }[\r\n]+    }," "")
kast_shadow_variant(sdfgi_path_off "${sg_scene}" "${sg_on}" "\"sdfgi\": { \"enabled\": false")
# Lua halves its energy (scripts/sdfgi_set.lua) and turns it off (sdfgi_toggle.lua); the scene saved that way. Lua
# swings the sun (sdfgi_sun.lua), with SDFGI and without.
kast_shadow_variant(sdfgi_lua "${sg_still}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/sdfgi_set.lua\"]")
kast_shadow_variant(sdfgi_lua_ref "${sg_still}" "\"energy\": 1," "\"energy\": 0.5,")
kast_shadow_variant(sdfgi_toggle "${sg_still}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/sdfgi_toggle.lua\"]")
string(REGEX REPLACE "\"systems\": \\[\\]" "\"systems\": [\"scripts/sdfgi_sun.lua\"]" sg_sun_scene "${sg_still}")
file(WRITE "${KAST_SHADOW_PROJECT}/sdfgi_sun.scene" "${sg_sun_scene}")
kast_shadow_variant(sdfgi_sun_off "${sg_sun_scene}" "${sg_on}" "\"sdfgi\": { \"enabled\": false")
# Streaming: only the camera's tile at first (radius 0); Lua widens the radius 0.5 s in (sdfgi_stream.lua), the camera
# still, so the tiles around stream in.
string(REGEX REPLACE "\"radius\": 1" "\"radius\": 0" sg_stream "${sg_still}")
string(REGEX REPLACE "\"systems\": \\[\\]" "\"systems\": [\"scripts/sdfgi_stream.lua\"]" sg_stream "${sg_stream}")
file(WRITE "${KAST_SHADOW_PROJECT}/sdfgi_stream.scene" "${sg_stream}")
kast_shadow_variant(sdfgi_stream_off "${sg_stream}" "${sg_on}" "\"sdfgi\": { \"enabled\": false")
# A setting out of range: the scene loads with SDFGI off and a warning.
kast_shadow_variant(sdfgi_bad_cell "${sg_still}" "\"cellSize\": 0.5" "\"cellSize\": 99")
# Precedence: lightmap_room (lightmapped Static meshes, a Dynamic ball on the probe grid) and voxelgi_room (every mesh in
# its VoxelGI volume) with SDFGI on.
set(sg_settings "\"sdfgi\": { \"enabled\": true, \"cascades\": 4, \"cellSize\": 0.25, \"energy\": 1, \"bounceFeedback\": 0.5, \"quality\": \"medium\", \"yScale\": 1 }")
file(READ "${KAST_TEST_PROJECT}/lightmap_room.scene" sg_lm_room)
kast_plain_asset_refs(sg_lm_room)
kast_shadow_variant(lightmap_room_sdfgi "${sg_lm_room}" "(\"bakedPath\": \"lightmap_room_lightmaps/lightmaps.json\" })"
                    "\\1,\n    ${sg_settings}")
file(READ "${KAST_TEST_PROJECT}/voxelgi_room.scene" sg_vgi_room)
kast_plain_asset_refs(sg_vgi_room)
kast_shadow_variant(voxelgi_room_sdfgi "${sg_vgi_room}" "(\"ambientIntensity\": 0.1)" "\\1,\n    ${sg_settings}")

set(sg_no_errors "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
# kast_sg_shot(<shot> <scene> <editor flags> [PLAY] [smoke_test args...]): sg_<shot>.png of the shadow project's scene.
function(kast_sg_shot name scene flags)
  set(play "")
  if(PLAY IN_LIST ARGN)
    list(REMOVE_ITEM ARGN PLAY)
    set(play " --play")
  endif()
  add_test(NAME sg_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sg_${name}.png"
            "-DEXTRA_ARGS=--project ${KAST_SHADOW_PROJECT} --scene ${KAST_SHADOW_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016${play} ${flags}"
            -DFOCUS_PANEL=Assets ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(sg_${name}_screenshot PROPERTIES FIXTURES_SETUP sg_${name}_shot)
endfunction()
foreach(shot sdfgi_still sdfgi_still_off sdfgi_still_low sdfgi_still_high sdfgi_sky sdfgi_sky_off sdfgi_no_ball
             sdfgi_lua_ref)
  kast_sg_shot(${shot} ${shot} "" PLAY "${sg_no_errors}")
endforeach()
# The edit-mode Scene view (no terrain outside Play) and the Lighting panel's SDFGI section.
kast_sg_shot(sdfgi_edit sdfgi_still "" "${sg_no_errors}")
kast_sg_shot(sdfgi_edit_off sdfgi_still_off "" "${sg_no_errors}")
kast_sg_shot(sdfgi_lighting sdfgi_still "" -DFOCUS_PANEL=Lighting "${sg_no_errors}")
# Along the path: two consecutive frames with SDFGI and without; a longer run logging each frame's rebuilt cells
# (--log-sdfgi): the cascades scroll, never rebuilding one in full.
foreach(frame 100 101)
  kast_sg_shot(sdfgi_path_${frame} sdfgi_outdoor "" PLAY -DFRAMES=${frame} "${sg_no_errors}")
  kast_sg_shot(sdfgi_path_off_${frame} sdfgi_path_off "" PLAY -DFRAMES=${frame} "${sg_no_errors}")
endforeach()
kast_sg_shot(sdfgi_path_log sdfgi_outdoor "--log-sdfgi" PLAY -DFRAMES=300 "-DFORBID_OUTPUT=rebuilt in full|\\[error\\]|\\[warn\\]"
             "-DEXPECT_OUTPUT=rebuilt cells c0 [1-9][0-9]*, c1 [0-9]+, c2 [0-9]+, c3 [0-9]+")
# Lua: energy halved, and SDFGI turned off; the sun swung (shots before it moves, 30 frames in, and long after).
kast_sg_shot(sdfgi_lua sdfgi_lua "" PLAY "-DEXPECT_OUTPUT=sdfgi_set.lua: sdfgi set" -DEXPECT_COUNT=1)
kast_sg_shot(sdfgi_toggle sdfgi_toggle "" PLAY "-DEXPECT_OUTPUT=sdfgi_toggle.lua: sdfgi toggled" -DEXPECT_COUNT=1)
foreach(shot sdfgi_sun sdfgi_sun_off)
  kast_sg_shot(${shot}_early ${shot} "" PLAY -DFRAMES=30 "${sg_no_errors}")
  kast_sg_shot(${shot} ${shot} "" PLAY -DFRAMES=150 "-DEXPECT_OUTPUT=sdfgi_sun.lua: sun moved" -DEXPECT_COUNT=1)
endforeach()
# Streaming: the radius widens 32 frames in; 10 frames later the new tiles have their GI (within
# tolerance of where it settles, 200 frames in), with SDFGI and without.
kast_sg_shot(sdfgi_stream_early sdfgi_stream "--log-sdfgi" PLAY -DFRAMES=43 "-DEXPECT_OUTPUT=SDFGI: terrain tile 1_0 streamed in")
kast_sg_shot(sdfgi_stream sdfgi_stream "" PLAY -DFRAMES=200 "-DEXPECT_OUTPUT=sdfgi_stream.lua: terrain radius 1")
kast_sg_shot(sdfgi_stream_off_early sdfgi_stream_off "" PLAY -DFRAMES=43 "-DEXPECT_OUTPUT=sdfgi_stream.lua: terrain radius 1")
kast_sg_shot(sdfgi_bad_cell sdfgi_bad_cell "" PLAY
  "-DEXPECT_OUTPUT=lighting.sdfgi: .cellSize. must be a number between 0.125 and 4. SDFGI left off" -DEXPECT_COUNT=1)
kast_sg_shot(lightmap_room_sdfgi lightmap_room_sdfgi "" "${sg_no_errors}")
kast_sg_shot(voxelgi_room_sdfgi voxelgi_room_sdfgi "" "${sg_no_errors}")

# kast_sg_compare(<test> <a> <b> <same: ON / OFF>): sg_<a>.png against sg_<b>.png, the whole window.
function(kast_sg_compare test a b same)
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/sg_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/sg_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "sg_${a}_shot;sg_${b}_shot")
endfunction()
set(sg_game_rect 322,45,955,582) # the Game view in a Play shot
# kast_sg_diff(<test> <a png> <a fixture> <b png> <b fixture> <max mean> [rect] [DIFFERS]): kast_self_check --diff-png of
# two shots (the Game view, or rect) within max mean difference; DIFFERS: more than it.
function(kast_sg_diff test a fixture_a b fixture_b limit)
  set(rect ${sg_game_rect})
  set(differs FALSE)
  foreach(arg IN LISTS ARGN)
    if(arg STREQUAL "DIFFERS")
      set(differs TRUE)
    else()
      set(rect ${arg})
    endif()
  endforeach()
  add_test(NAME ${test} COMMAND kast_self_check --diff-png "${a}" "${b}" ${limit} ${rect})
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "${fixture_a};${fixture_b}" WILL_FAIL ${differs})
endfunction()
# kast_sg_region(<test> <a> <a rect> <b> <b rect> <num> <den> <factor>): kast_self_check --region-compare of two shots.
function(kast_sg_region test a rect_a b rect_b num den factor)
  add_test(NAME ${test}
    COMMAND kast_self_check --region-compare "${CMAKE_CURRENT_BINARY_DIR}/sg_${a}.png" ${rect_a}
            "${CMAKE_CURRENT_BINARY_DIR}/sg_${b}.png" ${rect_b} ${num} ${den} ${factor})
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "sg_${a}_shot;sg_${b}_shot")
endfunction()
set(sg "${CMAKE_CURRENT_BINARY_DIR}/sg_")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdfgi_regions.cmake")

# SDFGI lights the scene (Play and edit mode); the presets differ from Medium.
kast_sg_compare(sg_still_vs_off_differs sdfgi_still sdfgi_still_off OFF)
kast_sg_compare(sg_edit_vs_off_differs sdfgi_edit sdfgi_edit_off OFF)
foreach(shot sdfgi_still_low sdfgi_still_high)
  kast_sg_compare(sg_${shot}_vs_medium_differs ${shot} sdfgi_still OFF)
endforeach()
# Bounce light: the floor by the red wall redder than without SDFGI, by the green wall greener, the shaded red wall
# lighter at the top (the sunlit floor bounces onto it). Sky occlusion, lit by the sky alone: under the overhang much darker than
# without SDFGI (whose sky light ignores what blocks it), the open terrain about the same.
kast_sg_region(sg_bleed_red sdfgi_still ${sg_red_floor} sdfgi_still_off ${sg_red_floor} r g 1.08)
kast_sg_region(sg_bleed_green sdfgi_still ${sg_green_floor} sdfgi_still_off ${sg_green_floor} g r 1.08)
kast_sg_region(sg_bounce_onto_wall sdfgi_still ${sg_red_wall} sdfgi_still_off ${sg_red_wall} l 1 1.2)
kast_sg_region(sg_sky_occluded sdfgi_sky_off ${sg_under_overhang} sdfgi_sky ${sg_under_overhang} l 1 2.0)
kast_sg_region(sg_sky_open_low sdfgi_sky ${sg_open_terrain} sdfgi_sky_off ${sg_open_terrain} l 1 0.9)
kast_sg_region(sg_sky_open_high sdfgi_sky_off ${sg_open_terrain} sdfgi_sky ${sg_open_terrain} l 1 0.9)
# The committed reference shot, within tolerance (the Game view only).
add_test(NAME sg_sdfgi_outdoor_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/sdfgi_outdoor.png"
          "${sg}sdfgi_still.png" 1.5 0,0,955,582,322,45)
set_tests_properties(sg_sdfgi_outdoor_matches_reference PROPERTIES FIXTURES_REQUIRED sg_sdfgi_still_shot)
# The Dynamic ball receives SDFGI but adds nothing to it: away from the ball and its shadow the scene without it is
# exactly the same.
kast_sg_diff(sg_dynamic_adds_nothing "${sg}sdfgi_no_ball.png" sg_sdfgi_no_ball_shot "${sg}sdfgi_still.png"
             sg_sdfgi_still_shot 0 ${sg_far_from_ball})
# Along the path: consecutive frames differ about as much with SDFGI as without (no popping).
kast_sg_diff(sg_path_no_popping "${sg}sdfgi_path_100.png" sg_sdfgi_path_100_shot "${sg}sdfgi_path_101.png"
             sg_sdfgi_path_101_shot ${sg_popping_limit})
kast_sg_diff(sg_path_off_moves "${sg}sdfgi_path_off_100.png" sg_sdfgi_path_off_100_shot "${sg}sdfgi_path_off_101.png"
             sg_sdfgi_path_off_101_shot 0 DIFFERS)
# Lua: halved energy and the toggle match the scenes saved that way.
kast_sg_diff(sg_lua_matches_ref "${sg}sdfgi_lua.png" sg_sdfgi_lua_shot "${sg}sdfgi_lua_ref.png" sg_sdfgi_lua_ref_shot 0)
kast_sg_diff(sg_toggle_matches_off "${sg}sdfgi_toggle.png" sg_sdfgi_toggle_shot "${sg}sdfgi_still_off.png"
             sg_sdfgi_still_off_shot 0)
kast_sg_compare(sg_lua_vs_still_differs sdfgi_lua sdfgi_still OFF)
# A bad setting: SDFGI off, as the scene without it.
kast_sg_diff(sg_bad_cell_matches_off "${sg}sdfgi_bad_cell.png" sg_sdfgi_bad_cell_shot "${sg}sdfgi_still_off.png"
             sg_sdfgi_still_off_shot 0)
# The sun swung from Lua relights the floor in the green wall's shade (in shadow at both sun positions) with SDFGI;
# without, it stays exactly the same.
kast_sg_diff(sg_sun_off_same "${sg}sdfgi_sun_off_early.png" sg_sdfgi_sun_off_early_shot "${sg}sdfgi_sun_off.png"
             sg_sdfgi_sun_off_shot 0 ${sg_green_shade})
kast_sg_diff(sg_sun_relights "${sg}sdfgi_sun_early.png" sg_sdfgi_sun_early_shot "${sg}sdfgi_sun.png" sg_sdfgi_sun_shot 3
             ${sg_green_shade} DIFFERS)
# Streaming: the tiles that came in 10 frames ago are lit as they settle (within tolerance), and differently than
# without SDFGI.
kast_sg_diff(sg_stream_lit_in_time "${sg}sdfgi_stream_early.png" sg_sdfgi_stream_early_shot "${sg}sdfgi_stream.png"
             sg_sdfgi_stream_shot ${sg_stream_limit} ${sg_streamed_tile})
kast_sg_diff(sg_stream_has_gi "${sg}sdfgi_stream_early.png" sg_sdfgi_stream_early_shot "${sg}sdfgi_stream_off_early.png"
             sg_sdfgi_stream_off_early_shot 2 ${sg_streamed_tile} DIFFERS)
# Precedence: lightmap_room's lightmapped Static meshes keep their lightmaps, its Dynamic ball takes SDFGI instead of the
# probe grid; every mesh in voxelgi_room's volume keeps VoxelGI (the Scene view exactly as without SDFGI).
foreach(rect ${sg_lm_floor} ${sg_lm_wall})
  string(REPLACE "," "_" name "${rect}")
  kast_sg_diff(sg_lightmap_kept_${name} "${sg}lightmap_room_sdfgi.png" sg_lightmap_room_sdfgi_shot
               "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" lm_lightmap_room_shot 0 ${rect})
endforeach()
kast_sg_diff(sg_dynamic_takes_sdfgi "${sg}lightmap_room_sdfgi.png" sg_lightmap_room_sdfgi_shot
             "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" lm_lightmap_room_shot 2 ${sg_lm_ball} DIFFERS)
kast_sg_diff(sg_voxelgi_kept "${sg}voxelgi_room_sdfgi.png" sg_voxelgi_room_sdfgi_shot
             "${CMAKE_CURRENT_BINARY_DIR}/vgi_voxelgi_room.png" vgi_voxelgi_room_shot 0 ${vgi_scene_rect})

# Round trip; SDFGI's GPU-free math.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdfgi_roundtrip")
add_test(NAME sdfgi_outdoor_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/sdfgi_outdoor.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/sdfgi_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME sdfgi_self_check COMMAND kast_self_check --sdfgi)

# The player: a game built from sdfgi_outdoor.scene with the camera still renders as the editor's Play does: its
# 957x584 screenshot matches the Game view. Along the path at 1600x900 (Medium) its GPU frame time stays under 4 ms
# (KAST_SDFGI_GPU_P99_MS) in 99% of the frames (the slowest one is logged too; a lone hiccup of the OS or another program can't fail it), with no
# cascade rebuilt in full.
kast_game_project(sdfgi "SDFGI Test" sdfgi_outdoor.scene FALSE "")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/sdfgi/sdfgi_outdoor.scene" "${sg_still}")
kast_game_project(sdfgi_path "SDFGI Test" sdfgi_outdoor.scene FALSE "")
foreach(game sdfgi sdfgi_path)
  add_test(NAME build_game_${game}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/${game}"
            "-DOUT=${KAST_GAME_OUT}/${game}" "-DEXE=SDFGI Test.exe" -DEXPECT=0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
  set_tests_properties(build_game_${game} PROPERTIES FIXTURES_SETUP game_${game}_built)
endforeach()
add_test(NAME player_sdfgi_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/sdfgi" "-DEXE=SDFGI Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/sdfgi.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_sdfgi.png"
          "-DEXTRA_ARGS=--fixed-dt 0.016 --size 957x584" -DEXPECT=0 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
add_test(NAME player_sdfgi_matches_editor
  COMMAND kast_self_check --diff-png "${sg}sdfgi_still.png" "${CMAKE_CURRENT_BINARY_DIR}/player_sdfgi.png"
          ${sg_player_limit} ${sg_player_rect})
# KAST_SDFGI_GPU_P99_MS is that limit in whole ms, for both 99th percentiles (the SDFGI view's and the frame's): 4, the
# budget, unless a machine's GPU (or what else runs on it) can't hold it; then only that machine's build cache sets more
# (docs/development/tests.html).
set(KAST_SDFGI_GPU_P99_MS 4 CACHE STRING "player_sdfgi_gpu_time: the SDFGI view and frame GPU time 99th percentile limit, whole ms")
if(NOT KAST_SDFGI_GPU_P99_MS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "KAST_SDFGI_GPU_P99_MS must be a whole number of ms, not '${KAST_SDFGI_GPU_P99_MS}'")
endif()
math(EXPR sg_p99_below "${KAST_SDFGI_GPU_P99_MS} - 1")
set(sg_p99_ms "")
foreach(ms RANGE ${sg_p99_below}) # under the limit: a whole part from 0 to limit - 1
  list(APPEND sg_p99_ms ${ms})
endforeach()
string(REPLACE ";" "|" sg_p99_ms "${sg_p99_ms}")
set(sg_p99_ms "(${sg_p99_ms})\\.[0-9]+")
add_test(NAME player_sdfgi_gpu_time
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/sdfgi_path" "-DEXE=SDFGI Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/sdfgi_path.gpu" "-DEXTRA_ARGS=--fixed-dt 0.016 --size 1600x900 --frames 310 --gpu-stats --log-sdfgi"
          -DEXPECT=0 "-DFORBID_OUTPUT=rebuilt in full"
          "-DEXPECT_OUTPUT=SDFGI view GPU time: [0-9.]+ ms average, ${sg_p99_ms} ms 99th percentile, [0-9.]+ ms at most over [0-9]+ frames \\(frame: ${sg_p99_ms} ms 99th percentile"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_sdfgi_screenshot PROPERTIES FIXTURES_REQUIRED game_sdfgi_built
                     FIXTURES_SETUP player_sdfgi_shot TIMEOUT 90)
set_tests_properties(player_sdfgi_gpu_time PROPERTIES FIXTURES_REQUIRED game_sdfgi_path_built TIMEOUT 90 RUN_SERIAL TRUE
                     LABELS perf) # ctest --preset perf, not the debug / release presets
set_tests_properties(player_sdfgi_matches_editor PROPERTIES FIXTURES_REQUIRED "player_sdfgi_shot;sg_sdfgi_still_shot")
