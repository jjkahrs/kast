# World-space UI, Overlay mode (docs/engine/world-ui.html; the C++ side is kast_self_check --worldui:
# checkWorldUIComponent, checkWorldToScreen, checkWorldGuiScene). tests/project/world_ui.scene: a camera at (0, 1.5, 6)
# looking down -Z; Near and Far on ui/nameplate.rml (256x64, magenta), overlapping, Near in front; a box-collider Wall
# with one 96x32 panel behind it occluded and one with occlude off; a panel 66 m away culled by its maxDistance 50 and
# one as far with no limit; a Mover; two entities on a missing .rml. scripts/world_ui.lua names Near "Alice", gives
# Far the "far" class (cyan) through kast.gui calls with an entity, logs "world_ui: PASS", and from 1 s of Play time
# moves the Mover 1.5 m right. Probes are at the pixels the panels' anchors project to (see the scene's numbers).
set(world_ui_forbid "world_ui: FAIL|Lua error|\\[warn\\] RmlUi|missing_plate.rml' not found.*missing_plate.rml' not found")

add_test(NAME world_ui_self_check COMMAND kast_self_check --worldui)

# The built player, 1280x720 at 60 fixed steps a second: frame 30 (Mover not moved yet) and frame 120 (moved).
kast_game_project(world_ui "World UI Test" world_ui.scene FALSE)
add_test(NAME build_game_world_ui
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/world_ui"
          "-DOUT=${KAST_GAME_OUT}/world_ui" "-DEXE=World UI Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_world_ui PROPERTIES FIXTURES_SETUP game_world_ui_built)
foreach(shot "early|30" "late|120")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 frames)
  add_test(NAME player_world_ui_${name}
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/world_ui" "-DEXE=World UI Test.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/world_ui_${name}.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_${name}.png"
            "-DEXTRA_ARGS=--frames ${frames} --fixed-dt 0.0166667 --size 1280x720" -DEXPECT=0
            "-DEXPECT_OUTPUT=world_ui: PASS" "-DFORBID_OUTPUT=${world_ui_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  set_tests_properties(player_world_ui_${name} PROPERTIES FIXTURES_REQUIRED game_world_ui_built
                       FIXTURES_SETUP player_world_ui_${name}_shot TIMEOUT 90)
endforeach()
add_test(NAME player_world_ui_bad_path
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/world_ui" "-DEXE=World UI Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/world_ui_bad_path.run" "-DEXTRA_ARGS=--frames 10" -DEXPECT=0
          "-DEXPECT_OUTPUT=World UI document 'ui/missing_plate.rml' not found" "-DFORBID_OUTPUT=${world_ui_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_world_ui_bad_path PROPERTIES FIXTURES_REQUIRED game_world_ui_built TIMEOUT 90)
# Panels (x,y at 1280x720): Near only 420,340 and where it overlaps Far 600,360 magenta (the nearer on top), Far only
# 720,345 cyan; behind the wall: occluded 790,362 the wall's grey, occlude off 895,362 magenta; 66 m away: limited
# 315,166 the clear color, unlimited 881,166 magenta. The Mover's panel at 598,146 (early) then 754,146 (late).
set(world_ui_magenta 255,0,255,8)
set(world_ui_clear 32,32,32,8) # kast_player's
set(world_ui_wall 209,205,202,12)
set(world_ui_common 0.3281,0.4722,${world_ui_magenta} 0.4688,0.5,${world_ui_magenta} 0.5625,0.4792,0,255,255,8
                    0.6172,0.5028,${world_ui_wall} 0.6992,0.5028,${world_ui_magenta}
                    0.2461,0.2306,${world_ui_clear} 0.6883,0.2306,${world_ui_magenta})
add_test(NAME player_world_ui_early_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_early.png"
         ${world_ui_common} 0.4672,0.2028,${world_ui_magenta} 0.5891,0.2028,${world_ui_clear})
set_tests_properties(player_world_ui_early_probe PROPERTIES FIXTURES_REQUIRED player_world_ui_early_shot)
add_test(NAME player_world_ui_late_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_late.png"
         ${world_ui_common} 0.4672,0.2028,${world_ui_clear} 0.5891,0.2028,${world_ui_magenta})
set_tests_properties(player_world_ui_late_probe PROPERTIES FIXTURES_REQUIRED player_world_ui_late_shot)
add_test(NAME player_world_ui_tracks
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_early.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_late.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_world_ui_tracks PROPERTIES FIXTURES_REQUIRED "player_world_ui_early_shot;player_world_ui_late_shot")

# The editor's Game view (1600x900 window: the Game image is 957x584 at 322,44): panels in Play, none when Stopped.
foreach(shot "play|--play" "stopped|")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(LENGTH shot fields)
  set(play "")
  if(fields GREATER 1)
    list(GET shot 1 play)
  endif()
  set(expect "")
  if(play)
    set(expect "-DEXPECT_OUTPUT=World UI document 'ui/missing_plate.rml' not found" -DEXPECT_COUNT=1)
  endif()
  add_test(NAME editor_world_ui_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/world_ui.scene --size 1600x900 --fixed-dt 0.0166667 ${play}"
            -DFOCUS_PANEL=Game ${expect} "-DFORBID_OUTPUT=${world_ui_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(editor_world_ui_${name} PROPERTIES FIXTURES_SETUP editor_world_ui_${name}_shot)
endforeach()
# Near only 600,330 and the overlap 760,340 magenta, Far only 880,320 cyan in Play; the editor's clear color there when
# Stopped.
add_test(NAME editor_world_ui_play_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_play.png"
         0.375,0.3667,${world_ui_magenta} 0.475,0.3778,${world_ui_magenta} 0.55,0.3556,0,255,255,8)
set_tests_properties(editor_world_ui_play_probe PROPERTIES FIXTURES_REQUIRED editor_world_ui_play_shot)
add_test(NAME editor_world_ui_stopped_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_stopped.png"
         0.375,0.3667,43,52,64,8 0.475,0.3778,43,52,64,8 0.55,0.3556,43,52,64,8)
set_tests_properties(editor_world_ui_stopped_probe PROPERTIES FIXTURES_REQUIRED editor_world_ui_stopped_shot)

# The Inspector's World UI on Bad Path A: the .rml combo shows the path missing with a warning; every field is shown,
# Facing and Pixels Per Meter greyed (Overlay mode).
add_test(NAME world_ui_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/world_ui_inspector.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/world_ui.scene --size 1600x900 --select 12"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME world_ui_scene_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_TEST_PROJECT}/world_ui.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
