# ctest for samples/starter-mmo (its README "Tests"), included by the root CMakeLists.txt.
#
# Always run (no docker): the editor opens the sample with no errors; a test-time copy whose CA file is the test
# certificate (the real one is copied out of the world container by setup_realm.cmake and not committed) plays the
# login screen for a screenshot, builds, and the built game starts with Online Services on.
#
# Live (the services/world compose stack): starter_mmo_live_setup restarts the stack with the sample's compose override,
# adds "Starter Realm" and readies the test account from KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD (the environment ctest
# runs in; unset = starter_mmo_live_env.cmake's defaults). Three stages then play test-time copies through the sample's screens (character list, the
# Meadow with the Guard, an idle kast_bot, the HUD and a chat line, both portals into the Cavern, the zone scenes loaded
# on each portal), a secrets check
# follows, and starter_mmo_live_teardown puts the default world mounts back. Each prints "SKIP: ..." and is skipped
# without docker or a running engine.
set(KAST_STARTER "${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo")
set(KAST_STARTER_OUT "${CMAKE_CURRENT_BINARY_DIR}/starter_mmo")
set(KAST_STARTER_COPY "${KAST_STARTER_OUT}/offline")
set(KAST_STARTER_STATE "${KAST_STARTER_OUT}/live")
set(KAST_WORLD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/services/world")

add_test(NAME starter_mmo_load
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_load.png"
          "-DEXTRA_ARGS=--project ${KAST_STARTER}" -DFRAMES=60 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|Lua error"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME starter_mmo_copy
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_STARTER}" "${KAST_STARTER_COPY}")
add_test(NAME starter_mmo_copy_ca
  COMMAND "${CMAKE_COMMAND}" -E copy "${KAST_TEST_PROJECT}/certs/net_test_ca.pem" "${KAST_STARTER_COPY}/certs/world_dev_ca.pem")
set_tests_properties(starter_mmo_copy PROPERTIES FIXTURES_SETUP starter_mmo_copied)
set_tests_properties(starter_mmo_copy_ca PROPERTIES FIXTURES_SETUP starter_mmo_copied DEPENDS starter_mmo_copy)
add_test(NAME starter_mmo_login_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_login.png"
          "-DEXTRA_ARGS=--project ${KAST_STARTER_COPY} --play --no-audio-device" -DFRAMES=120
          "-DFORBID_OUTPUT=\\[error\\]|Lua error|\\[warn\\] RmlUi" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME starter_mmo_build
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${KAST_STARTER_COPY}"
          "-DOUT=${KAST_STARTER_OUT}/game" "-DEXE=Starter MMO.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
add_test(NAME starter_mmo_player
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_STARTER_OUT}/game" "-DEXE=Starter MMO.exe"
          "-DWORK_DIR=${KAST_STARTER_OUT}/game.run" -DEXPECT=0 "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_player.png"
          "-DEXPECT_OUTPUT=Online Services enabled: auth http://localhost:3100, CA file 'certs/world_dev_ca.pem'"
          "-DEXTRA_ARGS=--no-audio-device" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
# GUI glows (ui/starter.rcss box-shadows): the mouse held over Log in (50 px below the Game view's centre) glows it, 3 and
# 11 px left of its edge fading over the card, while Create an account doesn't; the HUD's ready powers glow in the GUI
# Builder preview (scaled to the panel), above, below and not far off (the buttons are empty there: label and wipe only).
add_test(NAME starter_mmo_login_hover_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_login_hover.png"
          "-DEXTRA_ARGS=--project ${KAST_STARTER_COPY} --play --no-audio-device --sim-mouse 60,none,0,0,0,0,50" -DFRAMES=120
          "-DEXPECT_OUTPUT=GUI layer framebuffer 1 created" "-DFORBID_OUTPUT=\\[error\\]|Lua error|\\[warn\\] RmlUi|GUI filter|GUI effects need"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME starter_mmo_login_hover_probe COMMAND kast_self_check --probe-png "${KAST_STARTER_OUT}/starter_mmo_login_hover.png"
         0.3910,0.4493,255,204,0,6 0.3262,0.4493,160,133,23,14 0.3199,0.4493,76,69,43,12 0.3262,0.5215,49,49,49,4)
add_test(NAME starter_mmo_hud_glow_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_hud_glow.png"
          "-DEXTRA_ARGS=--project ${KAST_STARTER_COPY} --open-gui ui/hud.rml --size 1600x1000" -DFRAMES=60
          "-DEXPECT_OUTPUT=GUI layer framebuffer 1 created" "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\] RmlUi|GUI filter|GUI effects need"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME starter_mmo_hud_glow_probe COMMAND kast_self_check --probe-png "${KAST_STARTER_OUT}/starter_mmo_hud_glow.png"
         0.4334,0.6255,255,170,0,6 0.4334,0.6155,84,75,51,14 0.4334,0.6375,105,82,37,14 0.4334,0.6455,43,52,64,6)
set_tests_properties(starter_mmo_login_hover_screenshot PROPERTIES FIXTURES_SETUP starter_mmo_hover_shot)
set_tests_properties(starter_mmo_login_hover_probe PROPERTIES FIXTURES_REQUIRED starter_mmo_hover_shot)
set_tests_properties(starter_mmo_hud_glow_screenshot PROPERTIES FIXTURES_SETUP starter_mmo_hud_shot)
set_tests_properties(starter_mmo_hud_glow_probe PROPERTIES FIXTURES_REQUIRED starter_mmo_hud_shot)
set_tests_properties(starter_mmo_login_screenshot starter_mmo_build starter_mmo_login_hover_screenshot starter_mmo_hud_glow_screenshot
                     PROPERTIES FIXTURES_REQUIRED starter_mmo_copied)
set_tests_properties(starter_mmo_build PROPERTIES FIXTURES_SETUP starter_mmo_built)
set_tests_properties(starter_mmo_player PROPERTIES FIXTURES_REQUIRED starter_mmo_built)
# The quest definitions (.server/quests/) stay out of the client's game.pak.
add_test(NAME starter_mmo_pak_server_only
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_STARTER_OUT}/game/game.pak" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_pak_check.cmake")
set_tests_properties(starter_mmo_pak_server_only PROPERTIES FIXTURES_REQUIRED starter_mmo_built)
# Hit sparks (scripts/combat.lua): offline, a test-only driver publishes the server's hit; the burst's entity goes once
# kast.particles.alive is 0.
add_test(NAME starter_mmo_hit_sparks
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSAMPLE=${KAST_STARTER}" "-DOUT=${KAST_STARTER_OUT}/hit_sparks"
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_hit_sparks.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_hit_sparks.cmake")
# Chat and quest log lists (bound with data-for, no slot cap): offline, a test-only driver fakes 15 chat lines and 25
# quests (cmake/starter_mmo_lists.cmake).
add_test(NAME starter_mmo_lists
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSAMPLE=${KAST_STARTER}" "-DOUT=${KAST_STARTER_OUT}/lists"
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_lists.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_lists.cmake")
# HUD masks (ui/hud.rml, ui/chat.rml, ui/starter.rcss), offline: a test-only driver selects the Guard, fills the chat and
# starts Firebolt's 8 s cooldown under a 4 s GCD (cmake/starter_mmo_hud_masks.cmake). The shot (Play, 1600x1000: the
# Game view's GUI at 321,44) is probed at GUI pixels:
# - the round portrait (border-radius 28px + overflow: hidden, the base clip mask): the NPC placeholder's head and
#   background inside the circle, the sky (not the image's corner) inside its box but outside the circle, its gold ring;
# - the chat's top fade (mask-image linear-gradient over 60px): one glyph stem of "Chat line" on lines 1, 2 and 19, 133,
#   186 and 240 (unmasked);
# - the cooldown wipes (conic-gradient masks, about 94deg cleared on Firebolt, 172deg on Frost Nova): #454545 where the
#   sweep has passed (from 12 o'clock clockwise), 21 under the wipe; Firebolt above-right light, above-left and
#   below-right dark, Frost Nova above-right and below-right light, above-left dark.
set(KAST_STARTER_HUD_MASKS_PROBES
    342,27,240,200,160,8 326,40,160,60,40,8 319,17,175,200,235,10 316,40,255,170,0,8
    32,276,133,135,132,10 32,293,186,186,185,10 32,576,240,240,240,10
    499,605,69,69,69,6 419,605,21,21,21,6 499,631,21,21,21,6 639,605,69,69,69,6 639,631,69,69,69,6 559,605,21,21,21,6)
add_test(NAME starter_mmo_hud_masks
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSAMPLE=${KAST_STARTER}" "-DOUT=${KAST_STARTER_OUT}/hud_masks"
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_hud_masks.png" "-DFORBID=${masks_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_hud_masks.cmake")
kast_effects_probes(starter_hud_masks_probes 1600 1000 321 44 PROBES KAST_STARTER_HUD_MASKS_PROBES)
add_test(NAME starter_mmo_hud_masks_probe COMMAND kast_self_check --probe-png "${KAST_STARTER_OUT}/starter_mmo_hud_masks.png"
         ${starter_hud_masks_probes})
set_tests_properties(starter_mmo_hud_masks PROPERTIES FIXTURES_SETUP starter_mmo_hud_masks_shot)
set_tests_properties(starter_mmo_hud_masks_probe PROPERTIES FIXTURES_REQUIRED starter_mmo_hud_masks_shot)
# Nameplates, damage numbers and the notice board (scripts/nameplates.lua, scripts/notice_board.lua), offline: a
# test-only driver plays the world client's part on the bus (cmake/starter_mmo_nameplates.cmake). The shot (the Guard
# hit for 12, the Bat dead): Tester's bar full, the Guard's 38/50 red then grey, no bar under the friendly Elder's name,
# Rival's 100/140; the Bat's plate hidden; two floating damage numbers. Then the notice board's Next notice button
# (World mode, interactive): hovered (highlighted) and clicked (the next page), with real --sim-mouse events.
set(nameplates_args "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSAMPLE=${KAST_STARTER}" "-DCA=${KAST_TEST_PROJECT}/certs/net_test_ca.pem")
set(nameplates_expect "test nameplates: Guard .npc. plate ui/nameplate.rml, Health yes"
    "test nameplates: Elder .npc. plate ui/nameplate.rml, Health no" "test nameplates: Rival .player. plate ui/nameplate.rml"
    "test nameplates: hit: 2 floating text.s., Bat plate visible false, Guard plate visible true")
string(REPLACE ";" "$<SEMICOLON>" nameplates_expect "${nameplates_expect}")
add_test(NAME starter_mmo_nameplates
  COMMAND "${CMAKE_COMMAND}" ${nameplates_args} "-DOUT=${KAST_STARTER_OUT}/nameplates"
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_nameplates.png" "-DEDITOR_ARGS=--screenshot-on-log 4,SHOT"
          "-DEXPECT_ALL=${nameplates_expect}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_nameplates.cmake")
add_test(NAME starter_mmo_nameplates_probe COMMAND kast_self_check --probe-png "${KAST_STARTER_OUT}/starter_mmo_nameplates.png"
         0.5312,0.3250,224,60,60,12 0.5094,0.2470,224,60,60,12 0.5763,0.2470,49,49,49,12 0.6106,0.2470,109,114,94,14
         0.4031,0.2210,224,60,60,12 0.4656,0.2210,49,49,49,12 0.6875,0.5270,255,170,0,12)
# The board's button 286,147 px from the Game view's centre: held over it from frame 60 (highlighted #ffcc00; the
# floating texts gone a second after the hit), or clicked there (page 2).
foreach(sign "hover|none|test nameplates: mouseOver true$<SEMICOLON>test nameplates: after 70 frames: 0 floating text"
             "click|left|test nameplates: mouseOver true$<SEMICOLON>notice board: page 2")
  string(REPLACE "|" ";" sign "${sign}")
  list(GET sign 0 name)
  list(GET sign 1 button)
  list(GET sign 2 expect)
  add_test(NAME starter_mmo_notice_board_${name}
    COMMAND "${CMAKE_COMMAND}" ${nameplates_args} "-DOUT=${KAST_STARTER_OUT}/notice_board_${name}"
            "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_notice_board_${name}.png" -DFRAMES=150
            "-DEDITOR_ARGS=--sim-mouse 60,${button},0,0,0,286,147" "-DEXPECT_ALL=${expect}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_nameplates.cmake")
  set_tests_properties(starter_mmo_notice_board_${name} PROPERTIES FIXTURES_SETUP starter_mmo_notice_board_${name}_shot)
endforeach()
set_tests_properties(starter_mmo_nameplates PROPERTIES FIXTURES_SETUP starter_mmo_nameplates_shot)
set_tests_properties(starter_mmo_nameplates_probe PROPERTIES FIXTURES_REQUIRED starter_mmo_nameplates_shot)
add_test(NAME starter_mmo_notice_board_hover_probe COMMAND kast_self_check --probe-png
         "${KAST_STARTER_OUT}/starter_mmo_notice_board_hover.png" 0.6875,0.5270,255,204,0,12)
set_tests_properties(starter_mmo_notice_board_hover_probe PROPERTIES FIXTURES_REQUIRED starter_mmo_notice_board_hover_shot)
add_test(NAME starter_mmo_notice_board_page # the click turned the page: the board differs from the hover's
  COMMAND "${CMAKE_COMMAND}" "-DA=${KAST_STARTER_OUT}/starter_mmo_notice_board_hover.png"
          "-DB=${KAST_STARTER_OUT}/starter_mmo_notice_board_click.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(starter_mmo_notice_board_page PROPERTIES
                     FIXTURES_REQUIRED "starter_mmo_notice_board_hover_shot;starter_mmo_notice_board_click_shot")
# 100 nameplates (the driver's grid of Guards) in the built player, without vsync: the average frame time is logged and
# must hold 60 fps (16.7 ms) in an optimized build.
set(nameplates_budget "")
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(nameplates_budget -DMAX_MS=16.7)
endif()
add_test(NAME starter_mmo_nameplates_perf
  COMMAND "${CMAKE_COMMAND}" ${nameplates_args} "-DOUT=${KAST_STARTER_OUT}/nameplates_perf"
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_nameplates_perf.png" -DPERF=ON ${nameplates_budget}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_nameplates.cmake")
set_tests_properties(starter_mmo_nameplates_perf PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE # alone: a frame time
                     LABELS perf) # ctest --preset perf, not the debug / release presets

# Live. RUN_SERIAL: they change which content the shared compose stack serves.
set(live_args "-DWORLD_DIR=${KAST_WORLD_DIR}" "-DSAMPLE=${KAST_STARTER}" "-DSTATE=${KAST_STARTER_STATE}")
add_test(NAME starter_mmo_live_setup
  COMMAND "${CMAKE_COMMAND}" ${live_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_setup.cmake")
add_test(NAME starter_mmo_live_teardown
  COMMAND "${CMAKE_COMMAND}" ${live_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_teardown.cmake")
# After world_live_teardown: it recreates the world server, which would undo this setup's override mid-run.
set_tests_properties(starter_mmo_live_setup PROPERTIES FIXTURES_SETUP starter_mmo_live TIMEOUT 1200
                     DEPENDS world_live_teardown)
set_tests_properties(starter_mmo_live_teardown PROPERTIES FIXTURES_CLEANUP starter_mmo_live TIMEOUT 900)
set(live_shots)
# The equip_drag stage's mouse drag: from bag slot 1's centre to Main Hand's (ui/starter.rcss: #character, #equip_6).
set(equip_drag_dx -212)
set(equip_drag_dy -89)
# The rock stage's W key, pressed every 3 frames for 2 (--sim-key holds a key 2 frames): 200 presses, metres more than
# the 3.35 m to the rock at any frame rate.
set(rock_keys "")
foreach(frame RANGE 10 610 3)
  string(APPEND rock_keys " --sim-key ${frame},W")
endforeach()
# name | stage | character | frames | bot | expected log lines (regexes, "&" separated) | extra editor flags
foreach(stage
    "starter_mmo_live_chars|chars|Starter|420|OFF|picking realm Starter Realm&test driver: characters: .*Starter|"
    # The loading screen (ui/loading.rml): up from the click (the shot) to the zone's first quick frame; gone in the Meadow,
    # whose chat works; a refused enterWorld (the driver's list request still in flight) hides it and says why, and a
    # second click enters:
    "starter_mmo_live_loading|loading|Starter|3000|OFF|test driver: entering as Starter[a-j]+&loading: shown for Starter[a-j]+|--screenshot-on-log 2,SHOT"
    "starter_mmo_live_loaded|loaded|Starter|3000|OFF|loading: shown for Starter[a-j]+.*Switched to scene 'scenes/meadow.scene'.*test driver: loading machine: entering.*loading: hidden after zone rendered&test driver: loading screen hidden after [0-9]+ frame&test driver: chat Starter[a-j]+: hello after loading|--screenshot-on-log 10,SHOT"
    "starter_mmo_live_enter_error|enter_error|Starter|3000|OFF|test driver: EnterWorld failed: rate_limited&loading: shown for Starter[a-j]+.*loading: hidden, One request at a time, please[.].*loading: shown for Starter[a-j]+.*loading: hidden after zone rendered&test driver: chat Starter[a-j]+: hello after loading|--screenshot-on-log 10,SHOT"
    "starter_mmo_live_world|world|Starter|900|ON|test driver: entered map 10&spawn npc Guard template 'Guard'&spawn player [^ ]+ template ''&test driver: Health [0-9]+ / 140 on&test driver: Wallet [0-9]+ gold&test driver: chat Starter[a-j]+: hello from the starter sample|"
    "starter_mmo_live_cavern|cavern|Explorer|1800|OFF|test driver: entered map 1[01]&crossed to map 10&crossed to map 11&spawn npc Bat template 'Bat'&world: map changed to 11|"
    # One scene per zone (scripts/world_flow.lua): meadow.scene on entering, cavern.scene through the portal (map 11,
    # where the server put the player, HUD and chat shown, the Bat), meadow.scene again on the way back (the Guard, the bot),
    # with no loading screen after a portal (the "loading" game machine idle):
    "starter_mmo_live_travel|travel|Traveler|3000|ON|test driver: entered map 10&Switched to scene 'scenes/characters.scene'.*Switched to scene 'scenes/meadow.scene'.*Switched to scene 'scenes/cavern.scene'.*Switched to scene 'scenes/meadow.scene'&test driver: scene scenes/meadow.scene: terrain map 10&test driver: scene scenes/cavern.scene: terrain map 11, player at 128.0, 128.0.*cloned Bat.*test driver: scene scenes/meadow.scene: terrain map 10, player at 140.0, 128.0.*cloned Guard.*test driver: back in the meadow: Guard and [1-9][0-9]* player&test driver: documents shown: ui/bag.rml ui/character.rml ui/chat.rml ui/hud.rml ui/loading.rml ui/loot.rml ui/quest.rml ui/quest_log.rml&test driver: loading machine: idle|--screenshot-on-log 30,SHOT"
    # scripts/player.lua's camera: behind the character at 10 m, zoomed all the way out, orbited by a left drag
    "starter_mmo_live_camera|camera|Starter|900|OFF|test driver: entered map 10|"
    "starter_mmo_live_camera_zoom|camera|Starter|900|OFF|test driver: entered map 10|--sim-mouse 800,none,0,0,-10"
    "starter_mmo_live_camera_orbit|camera|Starter|900|OFF|test driver: entered map 10|--sim-mouse 800,left,480,-60,0"
    # Zone objects (maps/meadow/objects): W walks the character into a rock, which stops it 1.65 m from its centre at any frame rate
    # (its 1.2 m radius + scripts/player.lua's 0.45 m body); E at the Meadow Shrine: the server's blessing in the chat.
    "starter_mmo_live_rock|rock|Starter|3000|OFF|test driver: KEYS&test driver: stopped at 14[01][.][0-9]+, 106[.][0-9]+|${rock_keys} --sim-on-log KEYS --screenshot-on-log 30,SHOT"
    "starter_mmo_live_shrine|shrine|Starter|3000|OFF|test driver: PRESS&test driver: combat log: .E. Use the Meadow Shrine&test driver: combat log: [*] The shrine hums softly. You feel refreshed.|--sim-key 10,E --sim-on-log PRESS --screenshot-on-log 30,SHOT"
    # Combat (scripts/combat.lua, the server's systems/combat.lua). The screenshot is taken a moment after the driver's
    # "SHOT" line (--screenshot-on-log); FRAMES is only the upper bound. A left click (no drag) selects the Bat:
    "starter_mmo_live_combat_target|combat_target|Fighter|6000|OFF|test driver: target selected Bat .[0-9]+., 1 marker|--sim-mouse 2400,left,0,0,0 --screenshot-on-log 30,SHOT"
    # Tab, Escape, Tab, then 1 until the Bat dies; it stays as Fighter's corpse:
    "starter_mmo_live_combat|combat|Fighter|6000|OFF|test driver: target selected Bat&test driver: target cleared, 0 marker&test driver: combat log: You cast Strike on Bat for 12&test driver: combat log: Bat dies&test driver: corpse [0-9]+ owner [0-9]+|--screenshot-on-log 45,SHOT"
    # Mid-fight with the Guard: three Strikes, and it hits back:
    "starter_mmo_live_combat_guard|combat_guard|Fighter|6000|OFF|test driver: target selected Guard&test driver: combat log: You cast Strike on Guard for 12&test driver: Health 14 / 50 on&test driver: combat log: Guard hits you for 5|--screenshot-on-log 45,SHOT"
    # Nameplates (scripts/nameplates.lua) over the idle bot and the Guard; Strikes on the Guard shrink its bar and show
    # floating damage numbers, which go after a second; the shot when one shows again:
    "starter_mmo_live_nameplates|nameplates|Plater|6000|ON|test driver: nameplate on npc Guard: ui/nameplate.rml&test driver: nameplate on player [^ ]+: ui/nameplate.rml&test driver: combat log: You cast Strike on Guard for 12&test driver: Health [0-9]+ / 50 on&test driver: damage number shown.*test driver: damage numbers gone.*test driver: damage number shown&test driver: SHOT damage number|--screenshot-on-log 2,SHOT"
    # The Guard kills a Warrior that hit it once (140 Health, 5 every 2 s): dead, then revived at the spawn:
    "starter_mmo_live_death|death|Fallen|12000|OFF|test driver: combat log: Guard hits you for 5&test driver: combat log: You die&test driver: Health 0 / 140 on|--screenshot-on-log 45,SHOT"
    "starter_mmo_live_revive|revive|Fallen|12000|OFF|test driver: combat log: You die&test driver: combat log: You revive&test driver: Health 140 / 140 on&world: map changed to 10 at 128.0, 128.0|--screenshot-on-log 60,SHOT"
    # Loot (scripts/loot.lua, systems/inventory.lua; the test-only server system seeds the roll: 1 gold, Bat Wing, Linen
    # Cloth). Each waits for a living Bat (up to 90 s after an earlier stage's kill). A real right click (--sim-mouse
    # on the driver's CLICK line) on Looter's corpse opens the loot window:
    "starter_mmo_live_loot_window|loot_window|Looter|15000|OFF|test driver: corpse [0-9]+ owner [0-9]+ .me., 1 sparkle&test driver: loot window: 1 gold, bat_wing x1, linen_cloth x1|--sim-mouse 30,right,0,0,0 --sim-on-log CLICK --screenshot-on-log 30,SHOT"
    # Rival: Looter's corpse has no sparkle and refuses Rival; bad slots are refused; Rival's own corpse refuses it from
    # 8.5 m, then Rival takes it all from 2.5 m:
    "starter_mmo_live_loot_rejects|loot_rejects|Rival|15000|OFF|test driver: foreign corpse [0-9]+ .owner [0-9]+., 0 sparkle&test driver: combat log: [*] You can't loot that&test driver: rejected not_owner&test driver: rejected slot&test driver: corpse [0-9]+ owner [0-9]+ .me., 1 sparkle&test driver: combat log: [*] Too far away&test driver: rejected range&test driver: loot closed|--screenshot-on-log 30,SHOT"
    # Take All: gold to the Wallet, the items into the empty bag, the corpse goes; the bag open:
    "starter_mmo_live_loot|loot|Looter|15000|OFF|test driver: loot window: 1 gold, bat_wing x1, linen_cloth x1&test driver: gold [+]1&test driver: loot closed&test driver: bag 1=bat_wing:1 2=linen_cloth:1|--screenshot-on-log 30,SHOT"
    # Relog: the bag is as loot left it; hovering slot 1 shows its tooltip:
    "starter_mmo_live_bag_persist|bag_persist|Looter|6000|OFF|test driver: bag after relog: 1=bat_wing:1 2=linen_cloth:1|--sim-mouse 20,none,255,104,0 --sim-on-log HOVER --screenshot-on-log 30,SHOT"
    # A full bag (16 x 20 Linen Cloth): the gold is taken, the items stay in the corpse, "Inventory is full":
    "starter_mmo_live_bag_full|bag_full|Packer|15000|OFF|test driver: loot window: 1 gold, bat_wing x1, linen_cloth x1&test driver: rejected full&test driver: combat log: [*] Inventory is full&test driver: loot window: 0 gold, bat_wing x1, linen_cloth x1|--screenshot-on-log 30,SHOT"
    # The bag's drags (bag.slot.drop, as RmlUi publishes it), each stage one step further from the same known bag:
    "starter_mmo_live_bag_move|bag_move|Sorter|6000|OFF|test driver: bag 2=linen_cloth:3 3=bat_wing:5 4=bat_wing:18 5=worn_shortsword:1 7=minor_healing_potion:4|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_bag_swap|bag_swap|Sorter|6000|OFF|test driver: bag 2=bat_wing:5 3=linen_cloth:3 4=bat_wing:18|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_bag_merge|bag_merge|Sorter|6000|OFF|test driver: bag 2=bat_wing:3 3=linen_cloth:3 4=bat_wing:20|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_bag_split|bag_split|Sorter|6000|OFF|test driver: bag 2=bat_wing:3 3=linen_cloth:2 4=bat_wing:20 5=worn_shortsword:1 6=linen_cloth:1 7=minor_healing_potion:4|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_bag_destroy_confirm|bag_destroy_confirm|Sorter|6000|OFF|test driver: drop slot_6 -> nowhere|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_bag_destroy|bag_destroy|Sorter|6000|OFF|test driver: bag 2=bat_wing:3 3=linen_cloth:2 4=bat_wing:20 5=worn_shortsword:1 7=minor_healing_potion:4|--screenshot-on-log 30,SHOT"
    # A real mouse drag (--sim-mouse from slot 1, 254 / 103 px from the Game view's centre, 116 px right onto slot 3):
    "starter_mmo_live_bag_drag_mouse|bag_drag_mouse|Sorter|6000|OFF|test driver: bag 2=linen_cloth:3 3=bat_wing:5 4=bat_wing:18 5=worn_shortsword:1 7=minor_healing_potion:4&test driver: camera turned by the drag: false|--sim-mouse 20,left,116,0,0,254,103 --sim-on-log DRAG --screenshot-on-log 30,SHOT"
    # Real keys (--sim-key): B opens the bag; with the chat field focused B types "b" and the bag stays open:
    "starter_mmo_live_bag_keys|bag_keys|Sorter|6000|OFF|test driver: bag open: true&test driver: chat focused: true, focusedField chat_input&test driver: after B, chat, B: 1 bag toggle.s., chat field 'b'|--sim-key 30,B --sim-key 150,B --sim-on-log KEYS --screenshot-on-log 30,SHOT"
    # Classes and powers (scripts/characters.lua, scripts/powers.lua; the server's systems/classes.lua and combat.lua).
    # Create with no class sends nothing; then Mage picked (highlighted, its description):
    "starter_mmo_live_class_pick|class_pick|Picker|3000|OFF|test driver: pressed Create with no class&test driver: replies to Create with no class: 0&test driver: picked mage|--screenshot-on-log 20,SHOT"
    # One character per class against the Meadow's Guard, keys pressed by the action bar's event. Warrior: out of range,
    # the GCD and Strike's cooldown refused; Shield Wall halves the Guard's hits:
    "starter_mmo_live_class_warrior|warrior|Warrior|9000|OFF|test driver: Class warrior on&test driver: Health [0-9]+ / 140 on&test driver: powers of warrior: 1 Strike, 2 Heroic Slam, 3 Shield Wall&test driver: combat log: [*] Out of range&test driver: combat log: You cast Shield Wall&test driver: combat log: [*] Not ready yet .global cooldown.&test driver: combat log: You cast Strike on Guard for 12&test driver: combat log: [*] Not ready yet[^ ]&test driver: combat log: You cast Heroic Slam on Guard for 25&test driver: combat log: Guard hits you for 2|--screenshot-on-log 20,SHOT"
    # Mage: a Firebolt in flight; then Firebolt, Arcane Blast and Frost Nova rooting the Guard as it comes:
    "starter_mmo_live_class_mage_bolt|mage_bolt|Mage|9000|OFF|test driver: Class mage on&test driver: Health [0-9]+ / 80 on&test driver: powers of mage: 1 Firebolt, 2 Frost Nova, 3 Arcane Blast&test driver: combat log: You cast Firebolt on Guard for 14|--screenshot-on-log 5,SHOT"
    "starter_mmo_live_class_mage|mage|Mage|9000|OFF|test driver: combat log: You cast Firebolt on Guard for 14&test driver: combat log: You cast Arcane Blast on Guard for 30&test driver: combat log: You cast Frost Nova: Guard rooted|--screenshot-on-log 8,SHOT"
    # Priest: Power Shield soaks the Guard's hits; Smite; Heal:
    "starter_mmo_live_class_priest|priest|Priest|9000|OFF|test driver: Class priest on&test driver: Health [0-9]+ / 90 on&test driver: powers of priest: 1 Smite, 2 Heal, 3 Power Shield&test driver: combat log: You cast Power Shield&test driver: combat log: You cast Smite on Guard for 10&test driver: combat log: Power Shield absorbs 5&test driver: combat log: You cast Heal for [0-9]+|--screenshot-on-log 20,SHOT"
    # Rogue: Evasion dodges the Guard (test.seed makes the rolls known); Sinister Strike; then Backstab on the Cavern's
    # Bat, refused in front of it, deadly behind it:
    "starter_mmo_live_class_rogue|rogue|Rogue|9000|OFF|test driver: Class rogue on&test driver: Health [0-9]+ / 110 on&test driver: powers of rogue: 1 Sinister Strike, 2 Backstab, 3 Evasion&test driver: combat log: You cast Evasion&test driver: combat log: You cast Sinister Strike on Guard for 11&test driver: combat log: You dodge Guard's attack|--screenshot-on-log 20,SHOT"
    "starter_mmo_live_class_backstab|rogue_backstab|Rogue|15000|OFF|test driver: combat log: [*] Not behind target&test driver: combat log: You cast Backstab on Bat for 30&test driver: combat log: Bat dies|--screenshot-on-log 20,SHOT"
    # The character list with classes:
    "starter_mmo_live_class_list|chars|Starter|420|OFF|test driver: characters: .*Warrior[a-j]+ .warrior.&test driver: characters: .*Mage[a-j]+ .mage.&test driver: characters: .*Priest[a-j]+ .priest.&test driver: characters: .*Rogue[a-j]+ .rogue.|"
    # Equipment (scripts/character.lua; the server's systems/inventory.lua, combat.lua, classes.lua). Real keys: C opens
    # the character window; with the chat field focused C types "c" and the window stays open:
    "starter_mmo_live_equip_open|equip_open|Outfitter|6000|OFF|test driver: character open: true&test driver: character document visible: true&test driver: chat focused: true, focusedField chat_input&test driver: after C, chat, C: 1 character toggle.s., chat field 'c'|--sim-key 30,C --sim-key 150,C --sim-on-log KEYS --screenshot-on-log 30,SHOT"
    # A real mouse drag (--sim-mouse from bag slot 1, 254 / 103 px from the Game view's centre, onto Main Hand):
    "starter_mmo_live_equip_drag|equip_drag|Outfitter|6000|OFF|test driver: equipment 6=worn_shortsword&test driver: bag 2=leather_cap:1&test driver: stats: max health 140, armor 0, damage [+]3&test driver: camera turned by the drag: false|--sim-mouse 20,left,${equip_drag_dx},${equip_drag_dy},0,254,103 --sim-on-log DRAG --screenshot-on-log 30,SHOT"
    # Hovering the Leather Cap that stage left in bag slot 2 (--sim-mouse 58 px right of slot 1): slot and stats:
    "starter_mmo_live_equip_tooltip|equip_tooltip|Outfitter|6000|OFF|test driver: bag 2=leather_cap:1&test driver: hovered slot_2|--sim-mouse 20,none,312,104,0 --sim-on-log HOVER --screenshot-on-log 30,SHOT"
    # Refusals (drops replayed as RmlUi publishes them): the wrong slot (client and server), a full bag, in combat:
    "starter_mmo_live_equip_rules|equip_rules|Ruler|9000|OFF|test driver: combat log: [*] Can't equip that there&test driver: rejected wrong_slot&test driver: equipment 6=worn_shortsword&test driver: rejected full&test driver: combat log: [*] Inventory is full&test driver: combat log: You cast Strike on Guard for 15&test driver: rejected combat&test driver: combat log: [*] Can't do that in combat|--screenshot-on-log 20,SHOT"
    # Sword, tunic and shield: Strike 15, 155 max Health, the Guard's 5 down to 1 by 4 Armor; then the same after a relog:
    "starter_mmo_live_equip_combat|equip_combat|Brawler|9000|OFF|test driver: equipment 2=linen_tunic 6=worn_shortsword 7=wooden_shield&test driver: Health [0-9]+ / 155 on&test driver: combat log: You cast Strike on Guard for 15&test driver: combat log: Guard hits you for 1|--screenshot-on-log 20,SHOT"
    "starter_mmo_live_equip_persist|equip_persist|Brawler|6000|OFF|test driver: equipment after relog: 2=linen_tunic 6=worn_shortsword 7=wooden_shield&test driver: Health [0-9]+ / 155 on&test driver: stats: max health 155, armor 4, damage [+]3|--screenshot-on-log 30,SHOT"
    # Quests (scripts/quest.lua, scripts/quest_log.lua; the server's systems/quests.lua, .server/quests/). Quester's
    # chain, one stage further each: the Elder's yellow !, "Too far away" from 6.4 m, then a real right click
    # (--sim-mouse 40 px below the view's centre) opens its dialog:
    "starter_mmo_live_quest_dialog|quest_dialog|Quester|6000|OFF|test driver: quest markers: Elder available&test driver: marker Elder available: 2 yellow, 0 grey part.s.&test driver: combat log: [*] Too far away&test driver: CLICK.*test driver: quest dialog: Elder: bat_trouble available|--sim-mouse 30,right,0,0,0,0,40 --sim-on-log CLICK --screenshot-on-log 30,SHOT"
    # Accept: 0/3 and the grey ?; real keys: L opens the log, L typed into the focused chat field is an "l":
    "starter_mmo_live_quest_log|quest_log|Quester|6000|OFF|test driver: quest accepted: Bat Trouble&test driver: combat log: Quest accepted: Bat Trouble&test driver: quests: bat_trouble 0/3. completed -&test driver: marker Elder progress: 0 yellow, 6 grey part.s.&test driver: quest log open: true&test driver: quest log sync: Bat Trouble&test driver: chat focused: true, focusedField chat_input&test driver: after L, chat, L: 1 log toggle.s., chat field 'l'|--sim-key 30,L --sim-key 150,L --sim-on-log KEYS --screenshot-on-log 30,SHOT"
    # Three Bats in the Cavern (looted, seeded), the yellow ? back in the Meadow, the turn-in (+10 gold), Wings for the
    # Elder taken at 3/5 (the 3 looted wings count), Bat Trouble refused as completed:
    "starter_mmo_live_quest_kills|quest_kills|Quester|400000|OFF|test driver: Bats slain: bat_trouble 1/3&test driver: Bats slain: bat_trouble 2/3&test driver: Bats slain: bat_trouble 3/3&test driver: marker Elder ready: 6 yellow, 0 grey part.s.&test driver: quest completed: Bat Trouble .10 gold.&test driver: gold [+]10&test driver: quest dialog: Elder: wings_for_the_elder available&test driver: quests: wings_for_the_elder 3/5. completed bat_trouble&test driver: rejected quest completed&test driver: combat log: [*] You have already completed that quest|--screenshot-on-log 30,SHOT"
    # 5 Bat Wings (test-only), the log at 5/5, the turn-in: wings gone, +15 gold, the Leather Cap in the bag:
    "starter_mmo_live_quest|quest|Quester|40000|OFF|test driver: log at wings_for_the_elder 5/5&test driver: quest completed: Wings for the Elder .15 gold.&test driver: gold [+]15&test driver: after the turn-in: bag 1=leather_cap:1. none. completed bat_trouble, wings_for_the_elder&test driver: quest dialog: Elder: nothing|--screenshot-on-log 30,SHOT"
    # Every illegal request refused with its reason; abandon resets; a full bag refuses the turn-in; then Rejecter's
    # state after a relog:
    "starter_mmo_live_quest_rejects|quest_rejects|Rejecter|100000|OFF|test driver: rejected quest unknown_quest&test driver: rejected quest requires&test driver: rejected quest not_active.*test driver: rejected quest not_active&test driver: rejected quest active&test driver: rejected quest incomplete&test driver: rejected quest log_full&test driver: rejected quest dead.*test driver: rejected quest dead&test driver: before abandon: bat_trouble 2/3&test driver: after abandon: none&test driver: re-accepted: bat_trouble 0/3&test driver: rejected quest range&test driver: rejected quest completed&test driver: rejected quest full&test driver: combat log: [*] Inventory is full&test driver: full turn-in changed nothing: bag true, gold true, wings_for_the_elder 5/5. completed bat_trouble|--screenshot-on-log 30,SHOT"
    "starter_mmo_live_quest_persist|quest_persist|Rejecter|6000|OFF|test driver: quests after relog: wings_for_the_elder 5/5. completed bat_trouble&test driver: marker Elder ready|--screenshot-on-log 30,SHOT"
    # Chat (scripts/chat.lua, services/chat): slash commands typed into the chat box (yell, emote, a whisper to itself,
    # General, /join Trade, Trade, zone, a whisper to nobody), then real Enter (--sim-key Return on the driver's KEYS
    # line) sends the field. The chat service stopped (STOP_CHAT, chat_down): say still works, the box says it's down.
    "starter_mmo_live_chat|chat|Chatter|3000|OFF|test driver: chat status: connected&test driver: chat channel General 1 joined&test driver: chat yell Chatter[a-j]+: Hello Meadow!&test driver: chat emote Chatter[a-j]+: waves&test driver: chat message whisper_sent Chatter[a-j]+: psst&test driver: chat message whisper Chatter[a-j]+: psst&test driver: chat message channel General Chatter[a-j]+: Hello General&test driver: chat channel Trade 2 joined&test driver: chat message channel Trade Chatter[a-j]+: WTS sword&test driver: chat message zone Chatter[a-j]+: Anyone in the Meadow.&test driver: chat error not_online Nobodyhere&test driver: chat focused: true&test driver: chat Chatter[a-j]+: sent with Enter&test driver: chat field after Enter: ''|--sim-key 10,Return --sim-on-log KEYS --screenshot-on-log 30,SHOT"
    "starter_mmo_live_chat_down|chat_down|Chatter|3000|OFF|test driver: chat status: unavailable&test driver: chat Chatter[a-j]+: Say still works|--screenshot-on-log 30,SHOT")
  string(REPLACE "|" ";" stage "${stage}")
  list(GET stage 0 name)
  list(GET stage 1 stage_name)
  list(GET stage 2 character)
  list(GET stage 3 frames)
  list(GET stage 4 bot)
  list(GET stage 5 expected)
  list(GET stage 6 editor_args)
  string(REPLACE "&" "$<SEMICOLON>" expected "${expected}")
  set(shot "${KAST_STARTER_OUT}/${name}.png")
  set(stop_chat OFF)
  if(stage_name STREQUAL "chat_down")
    set(stop_chat ON)
  endif()
  list(APPEND live_shots "${shot}")
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" ${live_args} "-DSTAGE=${stage_name}"
            "-DCHARACTER=${character}" "-DSCREENSHOT=${shot}" "-DFRAMES=${frames}" "-DBOT=${bot}"
            "-DEXPECT_ALL=${expected}" "-DEDITOR_ARGS=${editor_args}" "-DSTOP_CHAT=${stop_chat}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_play.cmake")
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED starter_mmo_live DEPENDS starter_mmo_live_setup
                       SKIP_REGULAR_EXPRESSION "SKIP: " TIMEOUT 300)
endforeach()
# A character from before classes enters as a Warrior and is saved as one (MySQL edited between two plays):
add_test(NAME starter_mmo_live_legacy
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" ${live_args} -DCHARACTER=Veteran
          "-DSCREENSHOT=${KAST_STARTER_OUT}/starter_mmo_live_legacy.png" -DFRAMES=3000
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_legacy.cmake")
list(APPEND live_shots "${KAST_STARTER_OUT}/starter_mmo_live_legacy.png")
set_tests_properties(starter_mmo_live_legacy PROPERTIES FIXTURES_REQUIRED starter_mmo_live SKIP_REGULAR_EXPRESSION "SKIP: ")
# The loot, bag, class and equipment stages run in this order (each loot stage, and the Backstab one, may wait for the Bat an
# earlier one killed).
set(inventory_stages starter_mmo_live_loot_window starter_mmo_live_loot_rejects starter_mmo_live_loot
    starter_mmo_live_bag_persist starter_mmo_live_bag_full starter_mmo_live_bag_move starter_mmo_live_bag_swap
    starter_mmo_live_bag_merge starter_mmo_live_bag_split starter_mmo_live_bag_destroy_confirm starter_mmo_live_bag_destroy
    starter_mmo_live_bag_drag_mouse starter_mmo_live_bag_keys)
set(class_stages starter_mmo_live_class_pick starter_mmo_live_class_warrior starter_mmo_live_class_mage_bolt
    starter_mmo_live_class_mage starter_mmo_live_class_priest starter_mmo_live_class_rogue starter_mmo_live_class_backstab
    starter_mmo_live_class_list starter_mmo_live_legacy)
set(equip_stages starter_mmo_live_equip_open starter_mmo_live_equip_drag starter_mmo_live_equip_tooltip starter_mmo_live_equip_rules
    starter_mmo_live_equip_combat starter_mmo_live_equip_persist)
set(quest_stages starter_mmo_live_quest_dialog starter_mmo_live_quest_log starter_mmo_live_quest_kills starter_mmo_live_quest
    starter_mmo_live_quest_rejects starter_mmo_live_quest_persist)
set(previous starter_mmo_live_revive)
foreach(name IN LISTS inventory_stages class_stages equip_stages quest_stages)
  set_tests_properties(${name} PROPERTIES DEPENDS "starter_mmo_live_setup;${previous}" TIMEOUT 420)
  set(previous ${name})
endforeach()
string(REPLACE ";" "$<SEMICOLON>" live_shots "${live_shots}")
add_test(NAME starter_mmo_live_secrets
  COMMAND "${CMAKE_COMMAND}" "-DSAMPLE=${KAST_STARTER}" "-DSTATE=${KAST_STARTER_STATE}" "-DSHOTS=${live_shots}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_secrets.cmake")
set_tests_properties(starter_mmo_live_secrets PROPERTIES FIXTURES_REQUIRED starter_mmo_live
                     DEPENDS "starter_mmo_live_chars;starter_mmo_live_loading;starter_mmo_live_loaded;starter_mmo_live_enter_error;starter_mmo_live_world;starter_mmo_live_chat;starter_mmo_live_chat_down;starter_mmo_live_cavern;starter_mmo_live_travel;starter_mmo_live_camera;starter_mmo_live_camera_zoom;starter_mmo_live_camera_orbit;starter_mmo_live_combat_target;starter_mmo_live_combat;starter_mmo_live_combat_guard;starter_mmo_live_nameplates;starter_mmo_live_death;starter_mmo_live_revive;${inventory_stages};${class_stages};${equip_stages};${quest_stages}"
                     SKIP_REGULAR_EXPRESSION "SKIP: ")
set_tests_properties(starter_mmo_live_setup starter_mmo_live_teardown starter_mmo_live_chars starter_mmo_live_loading
                     starter_mmo_live_loaded starter_mmo_live_enter_error starter_mmo_live_world
                     starter_mmo_live_chat starter_mmo_live_chat_down
                     starter_mmo_live_cavern starter_mmo_live_travel starter_mmo_live_camera starter_mmo_live_camera_zoom
                     starter_mmo_live_camera_orbit starter_mmo_live_combat_target starter_mmo_live_combat
                     starter_mmo_live_combat_guard starter_mmo_live_nameplates starter_mmo_live_death starter_mmo_live_revive
                     starter_mmo_live_secrets
                     ${inventory_stages} ${class_stages} ${equip_stages} ${quest_stages} PROPERTIES RUN_SERIAL TRUE)
# The Bat kills wait for respawns (30 s after each looted corpse goes).
set_tests_properties(starter_mmo_live_quest_kills PROPERTIES TIMEOUT 900)
set_tests_properties(starter_mmo_live_setup PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: ")
