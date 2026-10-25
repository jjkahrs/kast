# Baked global illumination (docs/engine/lightmaps.html), included from CMakeLists.txt. lightmap_room.scene: a room of
# Static meshes (red and green walls, a normal-mapped back wall, a ceiling over its back half, a box, a pillar, a ball, a
# crate model, a step), a directional, a point and a spot light (all Indirect), a procedural sky and a Dynamic ball,
# baked into the committed lightmap_room_lightmaps/. Configure-time copies in the shadow project change one thing each.
# Shots as the IBL tests take them (cmake/ibl_tests.cmake): 1600x900, fixed dt, the Assets tab over the Console.
set(KAST_LM_ROOM "${KAST_TEST_PROJECT}/lightmap_room.scene")
set(KAST_LM_TERRAIN "${KAST_TEST_PROJECT}/terrain.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_LM_ROOM}"
             "${KAST_TEST_PROJECT}/lightmap_room_lightmaps/lightmaps.json")
file(READ "${KAST_LM_ROOM}" lm_room)
kast_plain_asset_refs(lm_room)
file(READ "${KAST_LM_TERRAIN}" lm_terrain)
kast_plain_asset_refs(lm_terrain)
file(COPY_FILE "${KAST_LM_ROOM}" "${KAST_SHADOW_PROJECT}/lightmap_room.scene")
set(lm_baked "\"bakedPath\": \"[^\"]*\"")
set(lm_ball "\"position\": \\[-2.9, 1.2, -5.5\\]")
kast_shadow_variant(lightmap_room_unbaked "${lm_room}" "${lm_baked}" "\"bakedPath\": \"\"")
kast_shadow_variant(lightmap_room_disabled "${lm_room}" "\"enabled\": true" "\"enabled\": false")
kast_shadow_variant(lightmap_room_flat "${lm_room}" "\"directional\": true" "\"directional\": false")
# The Dynamic ball: by the red wall (lit by the probe grid there) or under the ceiling at the back (in its shade), and
# the same as Off (lit by the sky, unoccluded, untinted).
kast_shadow_variant(lightmap_room_ball_off "${lm_room}" "\"giMode\": \"dynamic\"" "\"giMode\": \"off\"")
kast_shadow_variant(lightmap_room_ball_back "${lm_room}" "${lm_ball}" "\"position\": [0.6, 1.2, -9.2]")
string(REPLACE "\"giMode\": \"dynamic\"" "\"giMode\": \"off\"" lm_room_off "${lm_room}")
kast_shadow_variant(lightmap_room_ball_back_off "${lm_room_off}" "${lm_ball}" "\"position\": [0.6, 1.2, -9.2]")
# The Box moved after the bake: Out of date (and still lit by it).
kast_shadow_variant(lightmap_room_moved "${lm_room}" "\"position\": \\[-1.6, 0.6, -7.2\\]" "\"position\": [-1.0, 0.6, -6.5]")
# Lua halves the baked light and turns the directional term off (scripts/lightmap_set.lua); the scene saved that way.
kast_shadow_variant(lightmap_room_lua "${lm_room}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/lightmap_set.lua\"]")
string(REPLACE "\"intensity\": 1, \"directional\": true" "\"intensity\": 0.5, \"directional\": false" lm_room_lua_ref "${lm_room}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_lua_ref.scene" "${lm_room_lua_ref}")
# Every light Full (baked direct light and shadows too) and Realtime (not baked), baked by lightmap_*_bake; and every
# light off (only the sky lights the room: its occlusion shows), baked and not.
string(REGEX REPLACE "\"intensity\": (0.9|3|8), \"" "\"intensity\": 0, \"" lm_room_sky "${lm_room}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_sky.scene" "${lm_room_sky}")
kast_shadow_variant(lightmap_room_sky_unbaked "${lm_room_sky}" "${lm_baked}" "\"bakedPath\": \"\"")
kast_shadow_variant(lightmap_room_full "${lm_room}" "\"bakeMode\": \"indirect\"" "\"bakeMode\": \"full\"")
kast_shadow_variant(lightmap_room_realtime "${lm_room}" "\"bakeMode\": \"indirect\"" "\"bakeMode\": \"realtime\"")
# Files that can't be used: missing, corrupt (an atlas page replaced), another version.
kast_shadow_variant(lightmap_room_missing "${lm_room}" "${lm_baked}"
                    "\"bakedPath\": \"lightmap_room_missing_lightmaps/lightmaps.json\"")
kast_shadow_variant(lightmap_room_corrupt "${lm_room}" "${lm_baked}"
                    "\"bakedPath\": \"lightmap_room_corrupt_lightmaps/lightmaps.json\"")
kast_shadow_variant(lightmap_room_version "${lm_room}" "${lm_baked}"
                    "\"bakedPath\": \"lightmap_room_version_lightmaps/lightmaps.json\"")
file(REMOVE_RECURSE "${KAST_SHADOW_PROJECT}/lightmap_room_corrupt_lightmaps" "${KAST_SHADOW_PROJECT}/lightmap_room_version_lightmaps")
file(COPY "${KAST_TEST_PROJECT}/lightmap_room_lightmaps/" DESTINATION "${KAST_SHADOW_PROJECT}/lightmap_room_corrupt_lightmaps"
     PATTERN "*.meta" EXCLUDE) # the copy gets ids of its own, not duplicates
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_corrupt_lightmaps/atlas0.ktx" "not a KTX")
file(COPY "${KAST_TEST_PROJECT}/lightmap_room_lightmaps/" DESTINATION "${KAST_SHADOW_PROJECT}/lightmap_room_version_lightmaps"
     PATTERN "*.meta" EXCLUDE) # the copy gets ids of its own, not duplicates
file(READ "${KAST_TEST_PROJECT}/lightmap_room_lightmaps/lightmaps.json" lm_sidecar)
string(REPLACE "{\"version\":1," "{\"version\":99," lm_sidecar "${lm_sidecar}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_version_lightmaps/lightmaps.json" "${lm_sidecar}")
# Terrain: the hills map's tiles baked with the sun Full, and the same with the baked light at 0 (dark: proof it's the
# lightmaps, not the realtime sun, that light it).
string(REPLACE "\"Terrain\": { \"map\": 3, \"radius\": 1 }" "\"Terrain\": { \"map\": 3, \"radius\": 1, \"lightmap\": true }"
       lm_terrain_baked "${lm_terrain}")
kast_shadow_variant(lightmap_terrain "${lm_terrain_baked}" "(\"intensity\": 1) }" "\\1, \"bakeMode\": \"full\" }")
string(REPLACE "\"ambientIntensity\": 0.25 }" "\"ambientIntensity\": 0.25, \"lightmaps\": { \"intensity\": 0 } }"
       lm_terrain_dark "${lm_terrain_baked}")
kast_shadow_variant(lightmap_terrain_dark "${lm_terrain_dark}" "(\"intensity\": 1) }" "\\1, \"bakeMode\": \"full\" }")
file(COPY_FILE "${KAST_LM_TERRAIN}" "${KAST_SHADOW_PROJECT}/terrain.scene")

set(lm_scene_rect 322,76,955,551)
set(no_errors "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
# kast_lm_shot(<shot> <scene> <editor flags> [PLAY] [smoke_test args...]): lm_<shot>.png of the shadow project's scene.
function(kast_lm_shot name scene flags)
  set(play "")
  if(PLAY IN_LIST ARGN)
    list(REMOVE_ITEM ARGN PLAY)
    set(play " --play")
  endif()
  add_test(NAME lm_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/lm_${name}.png"
            "-DEXTRA_ARGS=--project ${KAST_SHADOW_PROJECT} --scene ${KAST_SHADOW_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016${play} ${flags}"
            -DFOCUS_PANEL=Assets ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(lm_${name}_screenshot PROPERTIES FIXTURES_SETUP lm_${name}_shot)
endfunction()
foreach(shot lightmap_room lightmap_room_unbaked lightmap_room_disabled lightmap_room_flat lightmap_room_ball_off
             lightmap_room_ball_back lightmap_room_ball_back_off lightmap_room_sky_unbaked)
  kast_lm_shot(${shot} ${shot} "" "${no_errors}")
endforeach()
foreach(shot lightmap_room lightmap_room_lua_ref)
  kast_lm_shot(${shot}_play ${shot} "" PLAY "${no_errors}")
endforeach()
kast_lm_shot(lightmap_room_lua_play lightmap_room_lua "" PLAY "-DEXPECT_OUTPUT=lightmap_set.lua: lightmaps set" -DEXPECT_COUNT=1)
set(lm_warning "Lightmaps '")
kast_lm_shot(lightmap_room_missing lightmap_room_missing ""
  "-DEXPECT_OUTPUT=${lm_warning}lightmap_room_missing_lightmaps/lightmaps.json': cannot read the file. the scene renders unbaked"
  -DEXPECT_COUNT=1)
kast_lm_shot(lightmap_room_corrupt lightmap_room_corrupt ""
  "-DEXPECT_OUTPUT=${lm_warning}lightmap_room_corrupt_lightmaps/lightmaps.json': 'atlas0.ktx' doesn't match lightmaps.json \\(bake again\\). the scene renders unbaked"
  -DEXPECT_COUNT=1)
kast_lm_shot(lightmap_room_version lightmap_room_version ""
  "-DEXPECT_OUTPUT=${lm_warning}lightmap_room_version_lightmaps/lightmaps.json': lightmaps.json is version 99, expected 1 \\(bake again\\). the scene renders unbaked"
  -DEXPECT_COUNT=1)
# A skinned model set Static loads, warns once and is lit as Dynamic; the Inspector shows a model's GI fields.
file(READ "${KAST_TEST_PROJECT}/models_anim.scene" lm_skinned)
kast_plain_asset_refs(lm_skinned)
kast_shadow_variant(lightmap_skinned_static "${lm_skinned}" "(\"model\": \"models/female-128-anim.glb\",)"
                    "\\1 \"giMode\": \"static\",")
kast_lm_shot(lightmap_skinned_static lightmap_skinned_static ""
  "-DEXPECT_OUTPUT=a skinned model can't be Static. lit as Dynamic" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]")
kast_lm_shot(lightmap_room_inspector lightmap_room "--select 9" "${no_errors}")
# The Lighting panel's status: Baked, and Out of date once a Static mesh moved.
kast_lm_shot(lightmap_room_lighting lightmap_room "" -DFOCUS_PANEL=Lighting "-DEXPECT_OUTPUT=Lightmaps: Baked")
kast_lm_shot(lightmap_room_moved_lighting lightmap_room_moved "" -DFOCUS_PANEL=Lighting "-DEXPECT_OUTPUT=Lightmaps: Out of date")

# kast_lm_compare(<test> <a> <b> <same: ON / OFF>): lm_<a>.png against lm_<b>.png, the whole window.
function(kast_lm_compare test a b same)
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/lm_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/lm_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "lm_${a}_shot;lm_${b}_shot")
endfunction()
# kast_lm_diff(<test> <a png> <a fixture> <b png> <b fixture> <max mean>): the Scene views within max mean difference.
function(kast_lm_diff test a fixture_a b fixture_b limit)
  add_test(NAME ${test} COMMAND kast_self_check --diff-png "${a}" "${b}" ${limit} ${lm_scene_rect})
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "${fixture_a};${fixture_b}")
endfunction()
# kast_lm_region(<test> <a> <a rect> <b> <b rect> <num> <den> <factor>): kast_self_check --region-compare of two shots.
function(kast_lm_region test a rect_a b rect_b num den factor)
  add_test(NAME ${test}
    COMMAND kast_self_check --region-compare "${CMAKE_CURRENT_BINARY_DIR}/lm_${a}.png" ${rect_a}
            "${CMAKE_CURRENT_BINARY_DIR}/lm_${b}.png" ${rect_b} ${num} ${den} ${factor})
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "lm_${a}_shot;lm_${b}_shot")
endfunction()

# Baked differs from unbaked; the directional term and the probe grid show; off at run time is exactly unbaked, and so is
# every bake that can't be used.
foreach(pair "lightmap_room|lightmap_room_unbaked" "lightmap_room|lightmap_room_flat" "lightmap_room|lightmap_room_ball_off"
             "lightmap_room_ball_back|lightmap_room_ball_back_off" "lightmap_room|lightmap_room_ball_back"
             "lightmap_room_play|lightmap_room_lua_ref_play")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  kast_lm_compare(lm_${a}_vs_${b}_differs ${a} ${b} OFF)
endforeach()
foreach(pair "lightmap_room_disabled|lightmap_room_unbaked" "lightmap_room_missing|lightmap_room_unbaked"
             "lightmap_room_corrupt|lightmap_room_unbaked" "lightmap_room_version|lightmap_room_unbaked"
             "lightmap_room_lua_play|lightmap_room_lua_ref_play")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  kast_lm_diff(lm_${a}_matches_${b} "${CMAKE_CURRENT_BINARY_DIR}/lm_${a}.png" lm_${a}_shot
               "${CMAKE_CURRENT_BINARY_DIR}/lm_${b}.png" lm_${b}_shot 0)
endforeach()
# The bake against unbaked: color bleed (the floor by the red wall redder, by the green wall greener); the Dynamic ball
# from the probe grid: tinted red by the red wall, warm (the lamp's bounce) in the back under the ceiling where the sky's
# light is bluish, and darker there than by the sunlit wall. The committed reference shot, within tolerance.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/lightmap_regions.cmake")
kast_lm_region(lm_bleed_red lightmap_room ${lm_red_floor} lightmap_room_unbaked ${lm_red_floor} r g 1.05)
kast_lm_region(lm_bleed_green lightmap_room ${lm_green_floor} lightmap_room_unbaked ${lm_green_floor} g r 1.03)
kast_lm_region(lm_ball_red lightmap_room ${lm_ball_front} lightmap_room_ball_off ${lm_ball_front} r g 1.05)
kast_lm_region(lm_ball_back_warm lightmap_room_ball_back ${lm_ball_back} lightmap_room_ball_back_off ${lm_ball_back} r b 1.15)
kast_lm_region(lm_ball_bright_vs_dark lightmap_room ${lm_ball_front} lightmap_room_ball_back ${lm_ball_back} l 1 1.2)
add_test(NAME lm_lightmap_room_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/lightmap_room.png"
          "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" 1.5 0,0,955,551,322,76) # the reference: the Scene view only
set_tests_properties(lm_lightmap_room_matches_reference PROPERTIES FIXTURES_REQUIRED lm_lightmap_room_shot)

# Bake from the command line on project copies: the room (at the default settings, under a minute), its lights Full and
# Realtime, and the terrain's tiles.
# Six identical Static cubes (one unwrap, one atlas page): they still draw as one instanced submit.
set(lm_row "")
foreach(i RANGE 0 5)
  math(EXPR id "20 + ${i}")
  math(EXPR x "${i} - 3")
  string(APPEND lm_row ",\n    {\n      \"id\": ${id},\n      \"name\": \"Row ${i}\",\n      \"parent\": null,\n      \"components\": {\n"
         "        \"Transform\": { \"position\": [${x}.4, 0.25, -2.6], \"rotation\": [0, 0, 0, 1], \"scale\": [0.5, 0.5, 0.5] },\n"
         "        \"MeshRenderer\": { \"primitive\": \"cube\", \"material\": \"materials/lm_white.mat\", \"giMode\": \"static\" }\n      }\n    }")
endforeach()
string(REGEX REPLACE "([\r\n]+  \\],[\r\n]+  \"systems\")" "${lm_row}\\1" lm_instanced "${lm_room}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_instanced.scene" "${lm_instanced}")
foreach(bake "room|lightmap_room.scene|60||" "full|lightmap_room_full.scene|60||" "realtime|lightmap_room_realtime.scene|60||"
             "sky|lightmap_room_sky.scene|60||" "instanced|lightmap_instanced.scene|60||"
             "terrain|lightmap_terrain.scene|300|4|lightmap_terrain.scene,lightmap_terrain_dark.scene,terrain.scene")
  string(REPLACE "|" ";" bake "${bake}")
  list(GET bake 0 name)
  list(GET bake 1 scene)
  list(GET bake 2 seconds)
  list(GET bake 3 tiles)
  list(GET bake 4 play)
  set(terrain "")
  if(tiles)
    set(terrain -DTERRAIN_TILES=${tiles} -DMAP_FOLDER=maps/hills)
  endif()
  if(name STREQUAL "instanced")
    set(terrain -DINSTANCING_CHECK=ON)
  endif()
  add_test(NAME lightmap_${name}_bake
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSELF_CHECK=$<TARGET_FILE:kast_self_check>"
            "-DPROJECT=${KAST_SHADOW_PROJECT}" -DSCENE=${scene} -DMAX_SECONDS=${seconds} ${terrain} "-DPLAY_SCENES=${play}"
            "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/lightmap_${name}_bake" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lightmap_bake_test.cmake")
  set_tests_properties(lightmap_${name}_bake PROPERTIES FIXTURES_SETUP lightmap_${name}_baked TIMEOUT 600)
endforeach()
set(lm_bakes "${CMAKE_CURRENT_BINARY_DIR}")
# Full lights: Static meshes lit as by the realtime lights (Indirect: same bounces, direct realtime), within tolerance;
# Realtime lights bounce nothing: differs.
kast_lm_diff(lm_full_matches_indirect "${lm_bakes}/lightmap_full_bake/loaded.png" lightmap_full_baked
             "${lm_bakes}/lightmap_room_bake/loaded.png" lightmap_room_baked 2.0)
add_test(NAME lm_realtime_vs_indirect_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${lm_bakes}/lightmap_realtime_bake/loaded.png" "-DB=${lm_bakes}/lightmap_room_bake/loaded.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(lm_realtime_vs_indirect_differs PROPERTIES FIXTURES_REQUIRED "lightmap_realtime_baked;lightmap_room_baked")
# Contact darkening, the sky alone lighting the room: baked, the floor at the static Ball's foot is darker against the
# open floor than unbaked (where the sky's light reaches everything alike).
add_test(NAME lm_contact_darkening
  COMMAND kast_self_check --region-compare "${lm_bakes}/lightmap_sky_bake/loaded.png" ${lm_contact}
          "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room_sky_unbaked.png" ${lm_contact} l l 1.2)
set_tests_properties(lm_contact_darkening PROPERTIES FIXTURES_REQUIRED "lightmap_sky_baked;lm_lightmap_room_sky_unbaked_shot")
# A fresh bake renders as the committed one does (the bake is deterministic).
kast_lm_diff(lm_fresh_bake_matches_committed "${lm_bakes}/lightmap_room_bake/loaded.png" lightmap_room_baked
             "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" lm_lightmap_room_shot 0.5)
# Terrain in Play: lit by its baked sun as by the realtime one (within tolerance), dark with the baked light at 0.
kast_lm_diff(lm_terrain_matches_realtime "${lm_bakes}/lightmap_terrain_bake/lightmap_terrain_play.png" lightmap_terrain_baked
             "${lm_bakes}/lightmap_terrain_bake/terrain_play.png" lightmap_terrain_baked 4.0)
add_test(NAME lm_terrain_vs_dark_differs
  COMMAND kast_self_check --region-compare "${lm_bakes}/lightmap_terrain_bake/lightmap_terrain_play.png" ${lm_scene_rect}
          "${lm_bakes}/lightmap_terrain_bake/lightmap_terrain_dark_play.png" ${lm_scene_rect} l 1 1.5)
set_tests_properties(lm_terrain_vs_dark_differs PROPERTIES FIXTURES_REQUIRED lightmap_terrain_baked)

# The editor bakes in the background: it keeps drawing (the progress is logged from several frames) and the bake lands;
# Cancel stops it and keeps the previous bake's files as they were.
string(REPLACE "\"samplesPerTexel\": 256" "\"samplesPerTexel\": 1024" lm_room_slow "${lm_room}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_slow.scene" "${lm_room_slow}")
add_test(NAME lightmap_background_bake
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${KAST_SHADOW_PROJECT}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/lightmap_background_bake" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lightmap_job_test.cmake")
set_tests_properties(lightmap_background_bake PROPERTIES TIMEOUT 300)

# Round trip; a scene breaking a lightmap rule fails to load, naming it.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lightmap_room_roundtrip")
add_test(NAME lightmap_room_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/lightmap_room.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/lightmap_room_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
foreach(bad "scale|\"lightmapScale\": 2|\"lightmapScale\": 0|.lightmapScale. must be > 0"
            "bounces|\"bounces\": 2|\"bounces\": 2.5|.bounces. must be a whole number"
            "mode|\"giMode\": \"dynamic\"|\"giMode\": \"baked\"|giMode")
  string(REPLACE "|" ";" bad "${bad}")
  list(GET bad 0 field)
  list(GET bad 1 regex)
  list(GET bad 2 replacement)
  list(GET bad 3 message)
  kast_shadow_variant(lightmap_room_bad_${field} "${lm_room}" "${regex}" "${replacement}")
  add_test(NAME lightmap_room_bad_${field}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCENE=${KAST_SHADOW_PROJECT}/lightmap_room_bad_${field}.scene" "-DEXPECT_OUTPUT=${message}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()
add_test(NAME lightmap_self_check COMMAND kast_self_check --lightmaps)

# The player: a game built from lightmap_room.scene (its bake ships in game.pak as it is) renders as the editor's Play
# does: its 957x584 screenshot matches the Game view.
kast_game_project(lightmaps "Lightmaps Test" lightmap_room.scene FALSE "")
add_test(NAME build_game_lightmaps
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/lightmaps"
          "-DOUT=${KAST_GAME_OUT}/lightmaps" "-DEXE=Lightmaps Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
add_test(NAME player_lightmaps_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/lightmaps" "-DEXE=Lightmaps Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/lightmaps.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_lightmaps.png"
          "-DEXTRA_ARGS=--fixed-dt 0.016 --size 957x584" -DEXPECT=0 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
add_test(NAME player_lightmaps_matches_editor
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room_play.png"
          "${CMAKE_CURRENT_BINARY_DIR}/player_lightmaps.png" 0.00392 322,44,955,583,1,0)
set_tests_properties(build_game_lightmaps PROPERTIES FIXTURES_SETUP game_lightmaps_built)
set_tests_properties(player_lightmaps_screenshot PROPERTIES FIXTURES_REQUIRED game_lightmaps_built
                     FIXTURES_SETUP player_lightmaps_shot TIMEOUT 90)
set_tests_properties(player_lightmaps_matches_editor PROPERTIES FIXTURES_REQUIRED "player_lightmaps_shot;lm_lightmap_room_play_shot")
add_test(NAME game_lightmaps_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/lightmaps/game.pak"
          "-DMUST_CONTAIN=lightmap_room_lightmaps/lightmaps.json;lightmap_room_lightmaps/atlas0.ktx;lightmap_room_lightmaps/atlas0_dir.ktx;lightmap_room_lightmaps/meshes.klmm"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_lightmaps_pak PROPERTIES FIXTURES_REQUIRED game_lightmaps_built)
