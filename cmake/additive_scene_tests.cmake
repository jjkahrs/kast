# Additive scenes (docs/engine/additive-scenes.html, docs/editor/multi-scene.html): several open scenes in the editor
# (Hierarchy headers, active scene, Remove Scene's prompt, the open set reopening after a restart) and Lua's additive
# loads in the built player. tests/project/additive_a.scene and additive_b.scene both use entity id 1, with different
# skies and gravity. Included from CMakeLists.txt.
set(KAST_ADDITIVE_A "${KAST_TEST_PROJECT}/additive_a.scene")
set(KAST_ADDITIVE_B "${KAST_TEST_PROJECT}/additive_b.scene")
set(KAST_ADDITIVE_ARGS "--scene ${KAST_ADDITIVE_A} --open-additive ${KAST_ADDITIVE_B} --size 1600x900")

# The engine, Lua and editor checks alone (they also run in engine_self_check).
add_test(NAME additive_scenes_self_check COMMAND kast_self_check --additive)
set_tests_properties(additive_scenes_self_check PROPERTIES LABELS unit)

# The Hierarchy: one header per open scene, a's marked active, b's "*" after an edit of its entity 1
# (id 4294967297 in memory: b's slot in the high bits).
add_test(NAME editor_additive_hierarchy
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_hierarchy.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --set-field 4294967297,Transform.position,[1.5,2,0]"
          "-DEXPECT_OUTPUT=Opened scene '[^']*additive_b.scene' additively" "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The Scene view with both scenes, before and after Set Active Scene: a's blue sky, then b's orange one.
add_test(NAME editor_additive_active_a
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_active_a.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS}" "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_additive_active_b
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_active_b.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --active-scene 1" "-DEXPECT_OUTPUT=Active scene 'additive_b'"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_additive_active_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/additive_active_a.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/additive_active_b.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(editor_additive_active_a PROPERTIES FIXTURES_SETUP additive_active_a_shot)
set_tests_properties(editor_additive_active_b PROPERTIES FIXTURES_SETUP additive_active_b_shot)
set_tests_properties(editor_additive_active_differs PROPERTIES FIXTURES_REQUIRED "additive_active_a_shot;additive_active_b_shot")
# A scene header's context menu (Set Active Scene, Save Scene, Remove Scene, New Scene, Open Scene Additive..., Create),
# a's cube moved into b (the Hierarchy drop), and Remove Scene on b with unsaved changes asking first.
add_test(NAME editor_additive_scene_menu
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_scene_menu.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --scene-menu 1" "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_additive_move
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_move.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --move-to-scene 2,1" "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_additive_remove_prompt
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_remove_prompt.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --set-field 4294967297,Transform.position,[1.5,2,0] --remove-scene 1"
          "-DFORBID_OUTPUT=\\[error\\]|Removed scene" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# The open set outlives the editor: the first run remembers a + b (b active) in its editor settings; a second run that
# opens the project with them reopens both and makes b active.
add_test(NAME editor_additive_remember
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_remember.png"
          "-DEXTRA_ARGS=${KAST_ADDITIVE_ARGS} --active-scene 1" -DFRAMES=30 "-DFORBID_OUTPUT=\\[error\\]"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME editor_additive_restore
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/additive_restore.png"
          "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --size 1600x900"
          "-DSETTINGS_FROM=${CMAKE_CURRENT_BINARY_DIR}/additive_remember.png.run/editor_settings.json"
          "-DEXPECT_OUTPUT=Opened scene '[^']*additive_a.scene'.*Opened scene '[^']*additive_b.scene' additively.*Active scene 'additive_b'"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(editor_additive_remember PROPERTIES FIXTURES_SETUP additive_remembered)
set_tests_properties(editor_additive_restore PROPERTIES FIXTURES_REQUIRED additive_remembered)
set_tests_properties(editor_additive_hierarchy editor_additive_active_a editor_additive_active_b editor_additive_active_differs
                     editor_additive_scene_menu editor_additive_move editor_additive_remove_prompt editor_additive_remember
                     editor_additive_restore PROPERTIES LABELS smoke)

# The built player (the "additive" game, KAST_GAMES): its system loads additive_b.scene additively, makes it active
# and unloads it again.
add_test(NAME player_additive_scenes
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/additive" "-DEXE=Additive Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/additive_scenes.run" -DEXPECT=0
          "-DEXPECT_OUTPUT=additive player: start additive_player.scene active additive_player.scene.*additive player: loaded additive_player.scene,additive_b.scene active additive_player.scene.*additive player: activated additive_player.scene,additive_b.scene active additive_b.scene.*additive player: unloaded additive_player.scene active additive_player.scene"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_additive_scenes PROPERTIES FIXTURES_REQUIRED game_additive_built TIMEOUT 90 LABELS integration)
