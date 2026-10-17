# ctest for samples/starter-mmo (its README "Tests"), included by the root CMakeLists.txt.
#
# Always run (no docker): the editor opens the sample with no errors; a test-time copy whose CA file is the test
# certificate (the real one is copied out of the world container by setup_realm.cmake and not committed) plays the
# login screen for a screenshot, builds, and the built game starts with Online Services on.
#
# Live (the services/world compose stack): starter_mmo_live_setup restarts the stack with the sample's compose override,
# adds "Starter Realm" and readies the test account from KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD (the environment ctest
# runs in; unset = skip). Three stages then play test-time copies through the sample's screens (character list, the
# Meadow with the Guard, an idle kast_bot, the HUD and a chat line, both portals into the Cavern), a secrets check
# follows, and starter_mmo_live_teardown puts the default world mounts back. Each prints "SKIP: ..." and is skipped
# without docker, a running engine or the credentials.
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
set_tests_properties(starter_mmo_login_screenshot starter_mmo_build PROPERTIES FIXTURES_REQUIRED starter_mmo_copied)
set_tests_properties(starter_mmo_build PROPERTIES FIXTURES_SETUP starter_mmo_built)
set_tests_properties(starter_mmo_player PROPERTIES FIXTURES_REQUIRED starter_mmo_built)

# Live. RUN_SERIAL: they change which content the shared compose stack serves.
set(live_args "-DWORLD_DIR=${KAST_WORLD_DIR}" "-DSAMPLE=${KAST_STARTER}" "-DSTATE=${KAST_STARTER_STATE}")
add_test(NAME starter_mmo_live_setup
  COMMAND "${CMAKE_COMMAND}" ${live_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_setup.cmake")
add_test(NAME starter_mmo_live_teardown
  COMMAND "${CMAKE_COMMAND}" ${live_args} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_teardown.cmake")
set_tests_properties(starter_mmo_live_setup PROPERTIES FIXTURES_SETUP starter_mmo_live TIMEOUT 1200)
set_tests_properties(starter_mmo_live_teardown PROPERTIES FIXTURES_CLEANUP starter_mmo_live TIMEOUT 900)
set(live_shots)
# name | stage | character | frames | bot | expected log lines (regexes, "&" separated)
foreach(stage
    "starter_mmo_live_chars|chars|Starter|420|OFF|picking realm Starter Realm&test driver: characters: .*Starter"
    "starter_mmo_live_world|world|Starter|900|ON|test driver: entered map 10&spawn npc Guard template 'Guard'&spawn player [^ ]+ template ''&test driver: Health [0-9]+ / 100 on&test driver: Wallet [0-9]+ gold&test driver: chat Starter[a-j]+: hello from the starter sample"
    "starter_mmo_live_cavern|cavern|Explorer|1800|OFF|test driver: entered map 1[01]&crossed to map 10&crossed to map 11&spawn npc Bat template 'Bat'&world: map changed to 11")
  string(REPLACE "|" ";" stage "${stage}")
  list(GET stage 0 name)
  list(GET stage 1 stage_name)
  list(GET stage 2 character)
  list(GET stage 3 frames)
  list(GET stage 4 bot)
  list(GET stage 5 expected)
  string(REPLACE "&" "$<SEMICOLON>" expected "${expected}")
  set(shot "${KAST_STARTER_OUT}/${name}.png")
  list(APPEND live_shots "${shot}")
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" ${live_args} "-DSTAGE=${stage_name}"
            "-DCHARACTER=${character}" "-DSCREENSHOT=${shot}" "-DFRAMES=${frames}" "-DBOT=${bot}"
            "-DEXPECT_ALL=${expected}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_play.cmake")
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED starter_mmo_live DEPENDS starter_mmo_live_setup
                       SKIP_REGULAR_EXPRESSION "SKIP: " TIMEOUT 300)
endforeach()
string(REPLACE ";" "$<SEMICOLON>" live_shots "${live_shots}")
add_test(NAME starter_mmo_live_secrets
  COMMAND "${CMAKE_COMMAND}" "-DSAMPLE=${KAST_STARTER}" "-DSTATE=${KAST_STARTER_STATE}" "-DSHOTS=${live_shots}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/starter_mmo_live_secrets.cmake")
set_tests_properties(starter_mmo_live_secrets PROPERTIES FIXTURES_REQUIRED starter_mmo_live
                     DEPENDS "starter_mmo_live_chars;starter_mmo_live_world;starter_mmo_live_cavern"
                     SKIP_REGULAR_EXPRESSION "SKIP: ")
set_tests_properties(starter_mmo_live_setup starter_mmo_live_teardown starter_mmo_live_chars starter_mmo_live_world
                     starter_mmo_live_cavern starter_mmo_live_secrets PROPERTIES RUN_SERIAL TRUE)
set_tests_properties(starter_mmo_live_setup PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP: ")
