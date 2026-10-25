# VoxelGI volumes (docs/engine/voxelgi.html), included from CMakeLists.txt. voxelgi_room.scene: a Cornell-style room of
# Static boxes (white floor, ceiling, back wall and tall box, a red and a green side wall, a white rough ball), a chrome
# ball (GI off: not voxelized), a Dynamic cube, a shadowed point light circling under the ceiling (Rotator, 90 degrees a
# second: by the red wall at Play start, by the green one 2 s in) and a VoxelGI volume over the room, baked into the
# committed voxelgi_room_voxelgi/. Configure-time copies in the shadow project change one thing each. Shots as the
# lightmap tests take them (cmake/lightmap_tests.cmake): 1600x900, fixed dt, the Assets tab over the Console.
set(KAST_VGI_ROOM "${KAST_TEST_PROJECT}/voxelgi_room.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_VGI_ROOM}"
             "${KAST_TEST_PROJECT}/voxelgi_room_voxelgi/13.json")
file(READ "${KAST_VGI_ROOM}" vgi_room)
kast_plain_asset_refs(vgi_room)
file(COPY_FILE "${KAST_VGI_ROOM}" "${KAST_SHADOW_PROJECT}/voxelgi_room.scene")
set(vgi_baked "\"bakedPath\": \"[^\"]*\"")
set(vgi_entity "[\r\n]+    {[\r\n]+      \"id\": 13,[^{]*{[\r\n]+[^\r\n]*[\r\n]+[^\r\n]*VoxelGI[^\r\n]*[\r\n]+      }[\r\n]+    },")
# Without the volume (the light without VoxelGI: the flat ambient), off, unbaked; the presets.
kast_shadow_variant(voxelgi_room_none "${vgi_room}" "${vgi_entity}" "")
kast_shadow_variant(voxelgi_room_disabled "${vgi_room}" "\"enabled\": true" "\"enabled\": false")
kast_shadow_variant(voxelgi_room_unbaked "${vgi_room}" "${vgi_baked}" "\"bakedPath\": \"\"")
kast_shadow_variant(voxelgi_room_low "${vgi_room}" "\"quality\": \"medium\"" "\"quality\": \"low\"")
kast_shadow_variant(voxelgi_room_high "${vgi_room}" "\"quality\": \"medium\"" "\"quality\": \"high\"")
# Files that can't be used: missing, corrupt (the voxels replaced), another version, baked at another resolution.
kast_shadow_variant(voxelgi_room_missing "${vgi_room}" "${vgi_baked}" "\"bakedPath\": \"voxelgi_room_missing_voxelgi/13.json\"")
kast_shadow_variant(voxelgi_room_corrupt "${vgi_room}" "${vgi_baked}" "\"bakedPath\": \"voxelgi_room_corrupt_voxelgi/13.json\"")
kast_shadow_variant(voxelgi_room_version "${vgi_room}" "${vgi_baked}" "\"bakedPath\": \"voxelgi_room_version_voxelgi/13.json\"")
kast_shadow_variant(voxelgi_room_mismatch "${vgi_room}" "\"resolution\": 128" "\"resolution\": 64")
file(REMOVE_RECURSE "${KAST_SHADOW_PROJECT}/voxelgi_room_corrupt_voxelgi" "${KAST_SHADOW_PROJECT}/voxelgi_room_version_voxelgi")
file(COPY "${KAST_TEST_PROJECT}/voxelgi_room_voxelgi/" DESTINATION "${KAST_SHADOW_PROJECT}/voxelgi_room_corrupt_voxelgi"
     PATTERN "*.meta" EXCLUDE) # the copy gets ids of its own, not duplicates
file(WRITE "${KAST_SHADOW_PROJECT}/voxelgi_room_corrupt_voxelgi/13.kvox" "not a KVOX")
file(COPY "${KAST_TEST_PROJECT}/voxelgi_room_voxelgi/" DESTINATION "${KAST_SHADOW_PROJECT}/voxelgi_room_version_voxelgi"
     PATTERN "*.meta" EXCLUDE) # the copy gets ids of its own, not duplicates
file(READ "${KAST_TEST_PROJECT}/voxelgi_room_voxelgi/13.json" vgi_sidecar)
string(REPLACE "\"version\": 1," "\"version\": 99," vgi_sidecar "${vgi_sidecar}")
file(WRITE "${KAST_SHADOW_PROJECT}/voxelgi_room_version_voxelgi/13.json" "${vgi_sidecar}")
# A Static mesh moved after the bake: Out of date (and still lit by it).
kast_shadow_variant(voxelgi_room_moved "${vgi_room}" "\"position\": \\[-1.4, 1.2, -7.4\\]" "\"position\": [-1.0, 1.2, -7.0]")
# Lua halves the volume's light (scripts/voxelgi_set.lua) and turns it off (scripts/voxelgi_toggle.lua); the scene
# saved that way.
kast_shadow_variant(voxelgi_room_lua "${vgi_room}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/voxelgi_set.lua\"]")
kast_shadow_variant(voxelgi_room_lua_ref "${vgi_room}" "\"intensity\": 1, \"quality\"" "\"intensity\": 0.5, \"quality\"")
kast_shadow_variant(voxelgi_room_toggle "${vgi_room}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/voxelgi_toggle.lua\"]")
# The lamp held still (the player's start frame isn't the editor's Play's).
kast_shadow_variant(voxelgi_room_still "${vgi_room}" "\"degreesPerSecond\": 90" "\"degreesPerSecond\": 0")
# Precedence: lightmap_room (lightmaps baked, a Dynamic ball on the probe grid) with a VoxelGI volume over the room,
# baked by voxelgi_lightmap_room_bake.
file(READ "${KAST_TEST_PROJECT}/lightmap_room.scene" vgi_lm_room)
kast_plain_asset_refs(vgi_lm_room)
string(REGEX REPLACE "([\r\n]+  \\],[\r\n]+  \"systems\")"
       ",\n    {\n      \"id\": 30,\n      \"name\": \"Room VoxelGI\",\n      \"parent\": null,\n      \"components\": {\n        \"Transform\": { \"position\": [0, 2.1, -6], \"rotation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n        \"VoxelGI\": { \"boxSize\": [8.8, 4.6, 8.8], \"resolution\": 128, \"intensity\": 1, \"bakedPath\": \"\" }\n      }\n    }\\1"
       vgi_lm_voxel "${vgi_lm_room}")
file(WRITE "${KAST_SHADOW_PROJECT}/lightmap_room_voxelgi.scene" "${vgi_lm_voxel}")

set(vgi_scene_rect 322,76,955,551)
set(vgi_no_errors "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
# kast_vgi_shot(<shot> <scene> <editor flags> [PLAY] [smoke_test args...]): vgi_<shot>.png of the shadow project's scene.
function(kast_vgi_shot name scene flags)
  set(play "")
  if(PLAY IN_LIST ARGN)
    list(REMOVE_ITEM ARGN PLAY)
    set(play " --play")
  endif()
  add_test(NAME vgi_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/vgi_${name}.png"
            "-DEXTRA_ARGS=--project ${KAST_SHADOW_PROJECT} --scene ${KAST_SHADOW_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016${play} ${flags}"
            -DFOCUS_PANEL=Assets ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(vgi_${name}_screenshot PROPERTIES FIXTURES_SETUP vgi_${name}_shot)
endfunction()
foreach(shot voxelgi_room voxelgi_room_none voxelgi_room_disabled voxelgi_room_low voxelgi_room_high)
  kast_vgi_shot(${shot} ${shot} "" "${vgi_no_errors}")
endforeach()
# Play: the lamp by the red wall (30 frames in) and by the green wall (120), with the volume and without.
foreach(shot voxelgi_room voxelgi_room_disabled)
  kast_vgi_shot(${shot}_play_early ${shot} "" PLAY -DFRAMES=30 "${vgi_no_errors}")
  kast_vgi_shot(${shot}_play ${shot} "" PLAY "${vgi_no_errors}")
endforeach()
kast_vgi_shot(voxelgi_room_lua_ref_play voxelgi_room_lua_ref "" PLAY "${vgi_no_errors}")
kast_vgi_shot(voxelgi_room_still_play voxelgi_room_still "" PLAY "${vgi_no_errors}")
kast_vgi_shot(voxelgi_room_lua_play voxelgi_room_lua "" PLAY "-DEXPECT_OUTPUT=voxelgi_set.lua: voxelgi set" -DEXPECT_COUNT=1)
kast_vgi_shot(voxelgi_room_toggle_play voxelgi_room_toggle "" PLAY "-DEXPECT_OUTPUT=voxelgi_toggle.lua: voxelgi toggled"
              -DEXPECT_COUNT=1)
set(vgi_warning "VoxelGI 'VoxelGI Volume': '")
kast_vgi_shot(voxelgi_room_unbaked voxelgi_room_unbaked ""
  "-DEXPECT_OUTPUT=VoxelGI 'VoxelGI Volume': not baked. meshes in its box are lit as outside it" -DEXPECT_COUNT=1)
kast_vgi_shot(voxelgi_room_missing voxelgi_room_missing ""
  "-DEXPECT_OUTPUT=${vgi_warning}voxelgi_room_missing_voxelgi/13.json': cannot read the file. meshes in its box are lit as outside it"
  -DEXPECT_COUNT=1)
kast_vgi_shot(voxelgi_room_corrupt voxelgi_room_corrupt ""
  "-DEXPECT_OUTPUT=${vgi_warning}voxelgi_room_corrupt_voxelgi/13.json': '13.kvox' doesn't match its .json \\(bake again\\). meshes"
  -DEXPECT_COUNT=1)
kast_vgi_shot(voxelgi_room_version voxelgi_room_version ""
  "-DEXPECT_OUTPUT=${vgi_warning}voxelgi_room_version_voxelgi/13.json': the .json is version 99, expected 1 \\(bake again\\). meshes"
  -DEXPECT_COUNT=1)
kast_vgi_shot(voxelgi_room_mismatch voxelgi_room_mismatch ""
  "-DEXPECT_OUTPUT=${vgi_warning}voxelgi_room_voxelgi/13.json': baked at resolution 128, the volume is 64 \\(bake again\\). meshes"
  -DEXPECT_COUNT=1)
# The Inspector's status and box gizmo (Baked), and Out of date once a Static mesh moved.
kast_vgi_shot(voxelgi_room_inspector voxelgi_room "--select 13" "-DEXPECT_OUTPUT=VoxelGI 'VoxelGI Volume': Baked")
kast_vgi_shot(voxelgi_room_moved_inspector voxelgi_room_moved "--select 13"
  "-DEXPECT_OUTPUT=VoxelGI 'VoxelGI Volume': Out of date")
kast_vgi_shot(voxelgi_room_lighting voxelgi_room "" -DFOCUS_PANEL=Lighting "-DEXPECT_OUTPUT=VoxelGI 'VoxelGI Volume': Baked")

# kast_vgi_compare(<test> <a> <b> <same: ON / OFF>): vgi_<a>.png against vgi_<b>.png, the whole window.
function(kast_vgi_compare test a b same)
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/vgi_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/vgi_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "vgi_${a}_shot;vgi_${b}_shot")
endfunction()
# kast_vgi_diff(<test> <a png> <a fixture> <b png> <b fixture> <max mean> [rect] [DIFFERS]): kast_self_check --diff-png of
# two shots (the Scene view, or rect) within max mean difference; DIFFERS: more than it.
function(kast_vgi_diff test a fixture_a b fixture_b limit)
  set(rect ${vgi_scene_rect})
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
# kast_vgi_region(<test> <a> <a rect> <b> <b rect> <num> <den> <factor>): kast_self_check --region-compare of two shots.
function(kast_vgi_region test a rect_a b rect_b num den factor)
  add_test(NAME ${test}
    COMMAND kast_self_check --region-compare "${CMAKE_CURRENT_BINARY_DIR}/vgi_${a}.png" ${rect_a}
            "${CMAKE_CURRENT_BINARY_DIR}/vgi_${b}.png" ${rect_b} ${num} ${den} ${factor})
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "vgi_${a}_shot;vgi_${b}_shot")
endfunction()
set(vgi "${CMAKE_CURRENT_BINARY_DIR}/vgi_")

# The volume lights the room; off, unbaked, or with files that can't be used it lights nothing: exactly the room without
# it. The presets differ from Medium. Lua's halved light and its toggle match the scenes saved that way.
kast_vgi_compare(vgi_voxelgi_room_vs_none_differs voxelgi_room voxelgi_room_none OFF)
foreach(shot voxelgi_room_low voxelgi_room_high)
  kast_vgi_compare(vgi_${shot}_vs_medium_differs ${shot} voxelgi_room OFF)
endforeach()
foreach(shot voxelgi_room_disabled voxelgi_room_unbaked voxelgi_room_missing voxelgi_room_corrupt voxelgi_room_version
             voxelgi_room_mismatch)
  kast_vgi_diff(vgi_${shot}_matches_none "${vgi}${shot}.png" vgi_${shot}_shot "${vgi}voxelgi_room_none.png"
                vgi_voxelgi_room_none_shot 0)
endforeach()
kast_vgi_diff(vgi_lua_matches_ref "${vgi}voxelgi_room_lua_play.png" vgi_voxelgi_room_lua_play_shot
              "${vgi}voxelgi_room_lua_ref_play.png" vgi_voxelgi_room_lua_ref_play_shot 0)
kast_vgi_diff(vgi_toggle_matches_disabled "${vgi}voxelgi_room_toggle_play.png" vgi_voxelgi_room_toggle_play_shot
              "${vgi}voxelgi_room_disabled_play.png" vgi_voxelgi_room_disabled_play_shot 0)
kast_vgi_compare(vgi_lua_vs_play_differs voxelgi_room_lua_play voxelgi_room_play OFF)
# The committed reference shot, within tolerance (the Scene view only).
add_test(NAME vgi_voxelgi_room_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/voxelgi_room.png"
          "${vgi}voxelgi_room.png" 1.5 0,0,955,551,322,76)
set_tests_properties(vgi_voxelgi_room_matches_reference PROPERTIES FIXTURES_REQUIRED vgi_voxelgi_room_shot)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/voxelgi_regions.cmake")

# Bake from the command line on project copies: the room, and lightmap_room with its volume (whose shot the
# precedence checks use).
foreach(bake "room|voxelgi_room.scene" "lightmap_room|lightmap_room_voxelgi.scene")
  string(REPLACE "|" ";" bake "${bake}")
  list(GET bake 0 name)
  list(GET bake 1 scene)
  add_test(NAME voxelgi_${name}_bake
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSELF_CHECK=$<TARGET_FILE:kast_self_check>"
            "-DPROJECT=${KAST_SHADOW_PROJECT}" -DSCENE=${scene} -DVOLUMES=1
            "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/voxelgi_${name}_bake" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/voxelgi_bake_test.cmake")
  set_tests_properties(voxelgi_${name}_bake PROPERTIES FIXTURES_SETUP voxelgi_${name}_baked TIMEOUT 300)
endforeach()
set(vgi_bakes "${CMAKE_CURRENT_BINARY_DIR}")
# A fresh bake renders as the committed one does (the voxelizer is deterministic).
kast_vgi_diff(vgi_fresh_bake_matches_committed "${vgi_bakes}/voxelgi_room_bake/loaded.png" voxelgi_room_baked
              "${vgi}voxelgi_room.png" vgi_voxelgi_room_shot 0)

# Round trip; a scene breaking a VoxelGI rule fails to load, naming it.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/voxelgi_room_roundtrip")
add_test(NAME voxelgi_room_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/voxelgi_room.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/voxelgi_room_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
foreach(bad "resolution|\"resolution\": 128|\"resolution\": 100|.resolution. must be 64, 128 or 256"
            "intensity|\"intensity\": 1, \"quality\"|\"intensity\": -1, \"quality\"|.intensity. must be >= 0"
            "quality|\"quality\": \"medium\"|\"quality\": \"ultra\"|quality")
  string(REPLACE "|" ";" bad "${bad}")
  list(GET bad 0 field)
  list(GET bad 1 regex)
  list(GET bad 2 replacement)
  list(GET bad 3 message)
  kast_shadow_variant(voxelgi_room_bad_${field} "${vgi_room}" "${regex}" "${replacement}")
  add_test(NAME voxelgi_room_bad_${field}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCENE=${KAST_SHADOW_PROJECT}/voxelgi_room_bad_${field}.scene" "-DEXPECT_OUTPUT=${message}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()
add_test(NAME voxelgi_self_check COMMAND kast_self_check --voxelgi)

# The player: a game built from voxelgi_room.scene with the lamp still (its bake ships in game.pak as it is) renders as
# the editor's Play does: its 957x584 screenshot matches the Game view. At 1600x900 (Medium) its GPU frame time,
# VoxelGI included, is logged and must stay under 3 ms.
kast_game_project(voxelgi "VoxelGI Test" voxelgi_room.scene FALSE "")
string(REPLACE "\"degreesPerSecond\": 90" "\"degreesPerSecond\": 0" vgi_still "${vgi_room}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/voxelgi/voxelgi_room.scene" "${vgi_still}")
add_test(NAME build_game_voxelgi
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/voxelgi"
          "-DOUT=${KAST_GAME_OUT}/voxelgi" "-DEXE=VoxelGI Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
add_test(NAME player_voxelgi_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/voxelgi" "-DEXE=VoxelGI Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/voxelgi.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_voxelgi.png"
          "-DEXTRA_ARGS=--fixed-dt 0.016 --size 957x584" -DEXPECT=0 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
add_test(NAME player_voxelgi_matches_editor
  COMMAND kast_self_check --diff-png "${vgi}voxelgi_room_still_play.png" "${CMAKE_CURRENT_BINARY_DIR}/player_voxelgi.png"
          0.00392 521,114,550,370,200,70) # inside the room (the hosts clear the view to different backgrounds)
add_test(NAME player_voxelgi_gpu_time
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/voxelgi" "-DEXE=VoxelGI Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/voxelgi.gpu" "-DEXTRA_ARGS=--fixed-dt 0.016 --size 1600x900 --gpu-stats" -DEXPECT=0
          "-DEXPECT_OUTPUT=GPU frame time: [0-2]\\.[0-9]+ ms average over [0-9]+ frames \\(VoxelGI view [0-9.]+ ms\\)"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(build_game_voxelgi PROPERTIES FIXTURES_SETUP game_voxelgi_built)
set_tests_properties(player_voxelgi_screenshot PROPERTIES FIXTURES_REQUIRED game_voxelgi_built
                     FIXTURES_SETUP player_voxelgi_shot TIMEOUT 90)
set_tests_properties(player_voxelgi_gpu_time PROPERTIES FIXTURES_REQUIRED game_voxelgi_built TIMEOUT 90 RUN_SERIAL TRUE)
set_tests_properties(player_voxelgi_matches_editor PROPERTIES FIXTURES_REQUIRED "player_voxelgi_shot;vgi_voxelgi_room_still_play_shot")
add_test(NAME game_voxelgi_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/voxelgi/game.pak"
          "-DMUST_CONTAIN=voxelgi_room_voxelgi/13.json;voxelgi_room_voxelgi/13.kvox"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_voxelgi_pak PROPERTIES FIXTURES_REQUIRED game_voxelgi_built)

# Color bleed: the floor by the red wall redder than without the volume, by the green wall greener, the ceiling by the
# red wall redder. The chrome ball reflects the walls (its sides tinted red and green; without the volume it reflects the
# flat ambient).
kast_vgi_region(vgi_bleed_red voxelgi_room ${vgi_red_floor} voxelgi_room_none ${vgi_red_floor} r g 1.08)
kast_vgi_region(vgi_bleed_green voxelgi_room ${vgi_green_floor} voxelgi_room_none ${vgi_green_floor} g r 1.08)
kast_vgi_region(vgi_bleed_ceiling voxelgi_room ${vgi_red_ceiling} voxelgi_room_none ${vgi_red_ceiling} r g 1.08)
kast_vgi_region(vgi_reflects_red voxelgi_room ${vgi_chrome_left} voxelgi_room_none ${vgi_chrome_left} r g 1.15)
kast_vgi_region(vgi_reflects_green voxelgi_room ${vgi_chrome_right} voxelgi_room_none ${vgi_chrome_right} g r 1.3)
# The moving lamp relights the volume live: where neither of its positions lights directly (exactly the same shot without
# the volume), and in the chrome ball, the two Play shots differ with it.
kast_vgi_diff(vgi_moving_light_direct_same "${vgi}voxelgi_room_disabled_play_early.png" vgi_voxelgi_room_disabled_play_early_shot
              "${vgi}voxelgi_room_disabled_play.png" vgi_voxelgi_room_disabled_play_shot 0 ${vgi_play_unlit})
kast_vgi_diff(vgi_moving_light_relights "${vgi}voxelgi_room_play_early.png" vgi_voxelgi_room_play_early_shot
              "${vgi}voxelgi_room_play.png" vgi_voxelgi_room_play_shot 10 ${vgi_play_unlit} DIFFERS)
kast_vgi_diff(vgi_moving_reflection_off_same "${vgi}voxelgi_room_disabled_play_early.png" vgi_voxelgi_room_disabled_play_early_shot
              "${vgi}voxelgi_room_disabled_play.png" vgi_voxelgi_room_disabled_play_shot 0.5 ${vgi_play_chrome})
kast_vgi_diff(vgi_moving_reflection "${vgi}voxelgi_room_play_early.png" vgi_voxelgi_room_play_early_shot
              "${vgi}voxelgi_room_play.png" vgi_voxelgi_room_play_shot 20 ${vgi_play_chrome} DIFFERS)
# Precedence in lightmap_room with a volume over it: its lightmapped Static meshes keep their lightmaps' diffuse (only
# their faint specular changes), its Dynamic ball takes VoxelGI instead of the probe grid.
foreach(rect ${vgi_lm_floor} ${vgi_lm_wall})
  string(REPLACE "," "_" name "${rect}")
  kast_vgi_diff(vgi_lightmap_kept_${name} "${vgi_bakes}/voxelgi_lightmap_room_bake/loaded.png" voxelgi_lightmap_room_baked
                "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" lm_lightmap_room_shot 1.0 ${rect})
endforeach()
kast_vgi_diff(vgi_dynamic_takes_voxelgi "${vgi_bakes}/voxelgi_lightmap_room_bake/loaded.png" voxelgi_lightmap_room_baked
              "${CMAKE_CURRENT_BINARY_DIR}/lm_lightmap_room.png" lm_lightmap_room_shot 4 ${vgi_lm_ball} DIFFERS)
