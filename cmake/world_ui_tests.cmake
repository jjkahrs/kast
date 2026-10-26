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

# World mode (the C++ side is kast_self_check --worldui: checkWorldUIAtlas, checkWorldUIQuads, checkWorldGuiAtlas).
# tests/project/world_ui_world.scene, camera A at (0, 1.5, 6) looking down -Z: the Sign (ui/sign.rml, 512x128 at 256
# px/m: 2 x 0.5 m, Billboard) 5 m ahead; Behind Wall (ui/nameplate.rml 256x128 at 128 px/m: 2 x 1 m, Billboard) with
# a Wall in front of its right half; Yaw Only and Fixed (the same, 2 x 1 m) to the left. scripts/world_ui_world.lua
# turns Fixed 60 degrees around Y at 1 s, moves the camera to B (45 degrees to the Sign's right) at 2 s and to C (45
# degrees above it) at 3 s, both 5 m from it. Shots (Play time): a 0.5 s, rotated 1.5 s, side 2.5 s, above 3.5 s.
# Probes (from the camera and quad math, 4 px inside / outside the edges): the Sign is 249x62 px (2 m at 5 m:
# 2 / (2 * 5 * tan 30) * 720) and centered from all three cameras; the Wall hides Behind Wall's right half; Fixed
# narrows when turned; from above, Yaw Only stays upright (a trapezoid: its slanted left edge leaves the ground showing
# where a camera-facing rectangle would cover it). The Sign's inside matches between the three cameras and, at 5 m, the
# committed tests/reference/world_ui_sign.png (its 266x78 pixels at 507,321 of the player shot).
set(world_ui_world_forbid "world_ui_world: FAIL|Lua error|\\[warn\\] RmlUi|\\[error\\]|World UI atlas full")
kast_game_project(world_ui_world "World UI World Test" world_ui_world.scene FALSE)
add_test(NAME build_game_world_ui_world
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/world_ui_world"
          "-DOUT=${KAST_GAME_OUT}/world_ui_world" "-DEXE=World UI World Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_world_ui_world PROPERTIES FIXTURES_SETUP game_world_ui_world_built)
set(world_ui_white 255,255,255,8)
set(world_ui_ground 233,228,224,8)
set(world_ui_editor_clear 43,52,64,8) # the Game view's
# The Sign 4 px inside its corners (white), then 4 px outside its left, right, top and bottom edges (what is behind it:
# the clear color, the ground, or a farther panel).
set(world_ui_player_sign 0.4057,0.4622,${world_ui_white} 0.5943,0.4622,${world_ui_white} 0.4057,0.5378,${world_ui_white}
                         0.5943,0.5378,${world_ui_white})
set(world_ui_editor_sign 0.4396,0.3497,${world_ui_white} 0.5611,0.3497,${world_ui_white} 0.4396,0.3970,${world_ui_white}
                         0.5611,0.3970,${world_ui_white})
set(world_ui_player_sign_a ${world_ui_player_sign} 0.3995,0.5000,${world_ui_clear} 0.6005,0.5000,${world_ui_magenta}
                           0.5000,0.4511,${world_ui_clear} 0.4104,0.5489,${world_ui_clear})
set(world_ui_editor_sign_a ${world_ui_editor_sign} 0.4346,0.3733,${world_ui_editor_clear} 0.5661,0.3733,${world_ui_magenta}
                           0.5003,0.3408,${world_ui_editor_clear} 0.4433,0.4059,${world_ui_editor_clear})
# Camera A: Behind Wall shows left of the Wall and not right of it (the Wall's grey); Yaw Only drawn; Fixed drawn near
# its right edge, which turned 60 degrees no longer reaches there; inside it either way.
set(world_ui_world_player_a ${world_ui_player_sign_a} 0.6172,0.5000,${world_ui_magenta} 0.6797,0.5000,${world_ui_wall}
                            0.3052,0.5881,${world_ui_magenta} 0.3685,0.4026,${world_ui_magenta} 0.2817,0.4026,${world_ui_magenta})
set(world_ui_world_player_rotated ${world_ui_player_sign_a} 0.6172,0.5000,${world_ui_magenta} 0.6797,0.5000,${world_ui_wall}
                                  0.3052,0.5881,${world_ui_magenta} 0.3685,0.4026,${world_ui_clear} 0.2743,0.4026,${world_ui_magenta})
set(world_ui_world_editor_a ${world_ui_editor_sign_a} 0.5704,0.3733,${world_ui_magenta} 0.6078,0.3733,${world_ui_wall}
                            0.3738,0.4305,${world_ui_magenta} 0.4153,0.3101,${world_ui_magenta} 0.3598,0.3101,${world_ui_magenta})
set(world_ui_world_editor_rotated ${world_ui_editor_sign_a} 0.5704,0.3733,${world_ui_magenta} 0.6078,0.3733,${world_ui_wall}
                                  0.3738,0.4305,${world_ui_magenta} 0.4153,0.3101,${world_ui_editor_clear} 0.3550,0.3101,${world_ui_magenta})
# Camera B: the same Sign; Yaw Only and Fixed behind it above and below.
set(world_ui_world_player_side ${world_ui_player_sign} 0.3995,0.5000,${world_ui_clear} 0.6005,0.5000,${world_ui_clear}
                               0.5000,0.4511,${world_ui_magenta} 0.4104,0.5489,${world_ui_magenta})
set(world_ui_world_editor_side ${world_ui_editor_sign} 0.4346,0.3733,${world_ui_editor_clear} 0.5661,0.3733,${world_ui_editor_clear}
                               0.5003,0.3408,${world_ui_magenta} 0.4433,0.4059,${world_ui_magenta})
# Camera C: the same Sign over the ground; Yaw Only upright: inside near its top-left corner and its right edge, and the
# ground left of its slanted left edge near the bottom.
set(world_ui_world_player_above ${world_ui_player_sign} 0.3995,0.5000,${world_ui_ground} 0.6005,0.5000,${world_ui_ground}
                                0.5000,0.4511,${world_ui_ground} 0.4104,0.5489,${world_ui_ground}
                                0.2248,0.3411,${world_ui_magenta} 0.2335,0.4131,${world_ui_ground} 0.3720,0.4131,${world_ui_magenta})
set(world_ui_world_editor_above ${world_ui_editor_sign} 0.4346,0.3733,${world_ui_ground} 0.5661,0.3733,${world_ui_ground}
                                0.5003,0.3408,${world_ui_ground} 0.4433,0.4059,${world_ui_ground}
                                0.3215,0.2702,${world_ui_magenta} 0.3277,0.3169,${world_ui_ground} 0.4175,0.3169,${world_ui_magenta})
foreach(shot "a|30" "rotated|90" "side|150" "above|210")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 frames)
  add_test(NAME player_world_ui_world_${name}
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/world_ui_world" "-DEXE=World UI World Test.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/world_ui_world_${name}.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_world_${name}.png"
            "-DEXTRA_ARGS=--frames ${frames} --fixed-dt 0.0166667 --size 1280x720" -DEXPECT=0
            "-DEXPECT_OUTPUT=world_ui_world: PASS" "-DFORBID_OUTPUT=${world_ui_world_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  set_tests_properties(player_world_ui_world_${name} PROPERTIES FIXTURES_REQUIRED game_world_ui_world_built
                       FIXTURES_SETUP player_world_ui_world_${name}_shot TIMEOUT 90)
  add_test(NAME player_world_ui_world_${name}_probe
    COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_world_${name}.png" ${world_ui_world_player_${name}})
  set_tests_properties(player_world_ui_world_${name}_probe PROPERTIES FIXTURES_REQUIRED player_world_ui_world_${name}_shot)
  # The editor's Game view (957x584 at 322,44 of the 1600x900 window).
  add_test(NAME editor_world_ui_world_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_world_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/world_ui_world.scene --size 1600x900 --fixed-dt 0.0166667 --play"
            -DFRAMES=${frames} -DFOCUS_PANEL=Game "-DEXPECT_OUTPUT=world_ui_world: PASS"
            "-DFORBID_OUTPUT=${world_ui_world_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(editor_world_ui_world_${name} PROPERTIES FIXTURES_SETUP editor_world_ui_world_${name}_shot)
  add_test(NAME editor_world_ui_world_${name}_probe
    COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_world_${name}.png" ${world_ui_world_editor_${name}})
  set_tests_properties(editor_world_ui_world_${name}_probe PROPERTIES FIXTURES_REQUIRED editor_world_ui_world_${name}_shot)
endforeach()
foreach(shot side above) # Billboard: the Sign's inside looks the same from every camera
  add_test(NAME player_world_ui_world_billboard_${shot}
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_world_a.png"
            "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_world_${shot}.png" 4 519,333,242,54)
  set_tests_properties(player_world_ui_world_billboard_${shot} PROPERTIES
                       FIXTURES_REQUIRED "player_world_ui_world_a_shot;player_world_ui_world_${shot}_shot")
endforeach()
add_test(NAME player_world_ui_world_sign_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_world_a.png"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/world_ui_sign.png" 2 507,321,266,78,0,0)
set_tests_properties(player_world_ui_world_sign_matches_reference PROPERTIES FIXTURES_REQUIRED player_world_ui_world_a_shot)

# A full atlas: tests/project/world_ui_overflow.scene holds six 1000x1000 panels (1 m at 1000 px/m), left to right
# 1 m farther each from camera A, of which the 2048x2048 atlas fits four: the nearest four are drawn (magenta at their
# centers), the two farthest not (the clear color), logged once.
set(world_ui_overflow_expect "World UI atlas full \\(2048x2048\\): the 2 farthest World panel\\(s\\) not drawn")
kast_game_project(world_ui_overflow "World UI Overflow Test" world_ui_overflow.scene TRUE)
add_test(NAME build_game_world_ui_overflow
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/world_ui_overflow"
          "-DOUT=${KAST_GAME_OUT}/world_ui_overflow" "-DEXE=World UI Overflow Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_world_ui_overflow PROPERTIES FIXTURES_SETUP game_world_ui_overflow_built)
add_test(NAME player_world_ui_overflow
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/world_ui_overflow" "-DEXE=World UI Overflow Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/world_ui_overflow.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_overflow.png"
          "-DEXTRA_ARGS=--frames 30 --fixed-dt 0.0166667 --size 1280x720" -DEXPECT=0 "-DEXPECT_OUTPUT=${world_ui_overflow_expect}"
          "-DFORBID_OUTPUT=World UI atlas full.*World UI atlas full|\\[warn\\] RmlUi|\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_world_ui_overflow PROPERTIES FIXTURES_REQUIRED game_world_ui_overflow_built
                     FIXTURES_SETUP player_world_ui_overflow_shot TIMEOUT 90)
add_test(NAME player_world_ui_overflow_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_world_ui_overflow.png"
         0.2564,0.5000,${world_ui_magenta} 0.3721,0.5000,${world_ui_magenta} 0.4621,0.5000,${world_ui_magenta}
         0.5341,0.5000,${world_ui_magenta} 0.5930,0.5000,${world_ui_clear} 0.6421,0.5000,${world_ui_clear})
set_tests_properties(player_world_ui_overflow_probe PROPERTIES FIXTURES_REQUIRED player_world_ui_overflow_shot)
add_test(NAME editor_world_ui_overflow
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_overflow.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/world_ui_overflow.scene --size 1600x900 --fixed-dt 0.0166667 --play"
          -DFRAMES=30 -DFOCUS_PANEL=Game "-DEXPECT_OUTPUT=${world_ui_overflow_expect}" -DEXPECT_COUNT=1
          "-DFORBID_OUTPUT=\\[warn\\] RmlUi|\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(editor_world_ui_overflow PROPERTIES FIXTURES_SETUP editor_world_ui_overflow_shot)
add_test(NAME editor_world_ui_overflow_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/editor_world_ui_overflow.png"
         0.3423,0.3733,${world_ui_magenta} 0.4173,0.3733,${world_ui_magenta} 0.4757,0.3733,${world_ui_magenta}
         0.5224,0.3733,${world_ui_magenta} 0.5607,0.3733,${world_ui_editor_clear} 0.5925,0.3733,${world_ui_editor_clear})
set_tests_properties(editor_world_ui_overflow_probe PROPERTIES FIXTURES_REQUIRED editor_world_ui_overflow_shot)
