# The Map tab and tile scenes (docs/editor/map-builder.html, docs/engine/terrain.html#tile-scenes) on tests/project's
# scenes/tilegrid.scene: a 3 x 3 map (maps/tilegrid, id 7, Terrain radius 0) with tile scenes on 0_0, 1_0, 0_1 (its
# "Stray Rock" root stands in tile 2_1) and 1_1, and an orphan on the undeclared 3_1. Tests that write use a copy
# (map_test.cmake, @PROJECT@). Included from CMakeLists.txt.
set(KAST_TILEGRID "${KAST_TEST_PROJECT}/scenes/tilegrid.scene")
set(map_smoke "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set(map_copy "${CMAKE_CURRENT_SOURCE_DIR}/cmake/map_test.cmake")
set(map_orphan "Map: maps/tilegrid/scenes/3_1.scene is an orphan: tile 3_1 is not a zone tile")
set(map_bounds "Map: maps/tilegrid/scenes/0_1.scene: root entity 'Stray Rock' at \\(600, 400\\) is in tile 2_1, outside tile 0_1")

# The tab docked beside Hierarchy and Zone (default layout, and a layout saved before the tab existed: tests/layouts/
# pre_map.ini, the default layout of the editor before it), the 3 x 3 grid in its 8 x 8 bounds.
add_test(NAME editor_map_tab
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map "-DLAYOUT_SAME_DOCK=Hierarchy;Zone;Map"
          "-DEXPECT_OUTPUT=Map: maps/tilegrid \\(9 tiles, 5 tile scenes\\)" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_tab_old_layout
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab_old_layout.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/pre_map.ini" "-DLAYOUT_SAME_DOCK=Hierarchy;Zone;Map"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_tab_pre_zone_layout
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab_pre_zone_layout.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/pre_zone.ini" "-DLAYOUT_SAME_DOCK=Hierarchy;Zone;Map;Systems"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
# No Terrain entity, no project: the tab says so (as the Zone tab), and nothing is logged.
add_test(NAME editor_map_no_terrain
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_no_terrain.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/scene_b.scene --map-click 0,0" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=Map: |\\[error\\]|New scene added" -P "${map_smoke}")
add_test(NAME editor_map_no_project
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_no_project.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/no_project_basic.scene" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=Map: |\\[error\\]" -P "${map_smoke}")

# A click on 1_0 (it has a scene) opens it additively, not active (the Hierarchy's new header); a second click closes it.
add_test(NAME editor_map_click_open
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_open.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 1,0" -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=Opened scene '[^']*maps/tilegrid/scenes/1_0.scene' additively.*Map: opened tile 1_0 \\(maps/tilegrid/scenes/1_0.scene\\)"
          "-DFORBID_OUTPUT=\\[error\\]|Active scene" -P "${map_smoke}")
add_test(NAME editor_map_click_open_hierarchy
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_open_hierarchy.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 1,0" -DFOCUS_PANEL=Hierarchy
          "-DEXPECT_OUTPUT=Map: opened tile 1_0" "-DFORBID_OUTPUT=\\[error\\]|Active scene" -P "${map_smoke}")
add_test(NAME editor_map_click_close
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_close.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 1,0 --map-click 1,0" -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=Map: opened tile 1_0.*Map: close tile 1_0.*Removed scene '1_0'" "-DFORBID_OUTPUT=\\[error\\]"
          -P "${map_smoke}")
# Undeclared cells ignore clicks: 5_5 is empty, 3_1 holds the orphan.
add_test(NAME editor_map_click_undeclared
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_undeclared.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 5,5 --map-click 3,1" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=\\[error\\]|Map: (opened|close|created)|Opened scene '[^']*maps/|New scene added" -P "${map_smoke}")

# A click on 2_0 (declared, no scene) creates an empty, unsaved scene for maps/tilegrid/scenes/2_0.scene (the Map tab
# marks it "*"); Ctrl+S writes it, making scenes/ if needed. A second click on the unsaved one asks before closing it.
add_test(NAME editor_map_click_create
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_create.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-click 2,0" -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=New scene added.*Map: created maps/tilegrid/scenes/2_0.scene for tile 2_0 \\(unsaved"
          "-DMUST_NOT_EXIST=@PROJECT@/maps/tilegrid/scenes/2_0.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
add_test(NAME editor_map_click_create_save
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_create_save.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-click 2,0 --sim-key 30,ctrl+S"
          -DFOCUS_PANEL=Scene "-DEXPECT_OUTPUT=Map: created maps/tilegrid/scenes/2_0.scene.*Saved scene '[^']*maps/tilegrid/scenes/2_0.scene'"
          "-DMUST_EXIST=@PROJECT@/maps/tilegrid/scenes/2_0.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
add_test(NAME editor_map_click_create_close_prompt
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_create_close_prompt.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-click 2,0 --map-click 2,0"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: created maps/tilegrid/scenes/2_0.scene.*Map: close tile 2_0"
          "-DFORBID_OUTPUT=\\[error\\]|Removed scene" -P "${map_copy}")

# Validation: the orphan and the out-of-bounds root each warned once (300 frames: the tab reads the files every half
# second), and File > Save still writes the scene.
add_test(NAME editor_map_warnings
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_warnings.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --file-save" -DFOCUS_PANEL=Map -DFRAMES=300
          "-DEXPECT_ONCE=${map_orphan};${map_bounds}" "-DEXPECT_OUTPUT=Saved scene '[^']*scenes/tilegrid.scene'"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
# Removing tile 1_1 in the zone (saved) leaves its scene on disk, now an orphan.
add_test(NAME editor_map_orphan_after_remove
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_orphan_after_remove.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map tilegrid --remove-tile 1,1 --save-zone --no-recycle"
          -DFOCUS_PANEL=Map "-DEXPECT_ONCE=Map: maps/tilegrid/scenes/1_1.scene is an orphan"
          "-DMUST_EXIST=@PROJECT@/maps/tilegrid/scenes/1_1.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")

# Play (scripts/map_stream.lua moves the camera 0_0 -> 1_0 -> 1_1 -> 2_0 -> 1_0): the open 1_1 unloads (out of range),
# tile scenes stream in and out with their events and kast.loadedScenes(); Ctrl+P stops and the open set comes back.
set(map_stream_head "map stream: loaded map 7 tile 0,0 maps/tilegrid/scenes/0_0.scene.*map stream: unloaded map 7 tile 0,0 maps/tilegrid/scenes/0_0.scene.*map stream: loaded map 7 tile 1,0 maps/tilegrid/scenes/1_0.scene.*map stream: unloaded map 7 tile 1,0.*map stream: loaded map 7 tile 1,1.*map stream: scenes scenes/tilegrid.scene,maps/tilegrid/scenes/1_1.scene")
set(map_stream_tail "map stream: unloaded map 7 tile 1,1.*map stream: loaded map 7 tile 1,0.*map stream: scenes scenes/tilegrid.scene,maps/tilegrid/scenes/1_0.scene")
add_test(NAME editor_map_play_stream
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_play_stream.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --open-additive ${KAST_TEST_PROJECT}/maps/tilegrid/scenes/1_1.scene --play --sim-key 170,ctrl+P --size 1600x1000"
          -DFOCUS_PANEL=Map -DFRAMES=230
          "-DEXPECT_OUTPUT=Unloaded scene 'maps/tilegrid/scenes/1_1.scene'.*map stream: unloaded map 7 tile 1,1 .*${map_stream_head}.*${map_stream_tail}.*Stop"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
set_tests_properties(editor_map_tab editor_map_tab_old_layout editor_map_tab_pre_zone_layout editor_map_no_terrain
                     editor_map_no_project editor_map_click_open
                     editor_map_click_open_hierarchy editor_map_click_close editor_map_click_undeclared
                     editor_map_click_create editor_map_click_create_save editor_map_click_create_close_prompt
                     editor_map_warnings editor_map_orphan_after_remove editor_map_play_stream PROPERTIES LABELS smoke)

# The built player (game "tilegrid" in KAST_GAMES, its copy's 2_0.scene broken): the same streaming path; the broken
# scene logs one error and streaming goes on (1_0 loads again after it).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/tilegrid/maps/tilegrid/scenes/2_0.scene" "{ not a scene\n")
add_test(NAME player_tilegrid_stream
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/tilegrid" "-DEXE=Tile Grid Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/tilegrid_stream.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_tilegrid_stream.png"
          -DEXPECT=0 "-DEXPECT_OUTPUT=${map_stream_head}.*Map: tile scene 'maps/tilegrid/scenes/2_0.scene' did not load.*${map_stream_tail}"
          "-DEXPECT_ONCE=Map: tile scene 'maps/tilegrid/scenes/2_0.scene' did not load" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_tilegrid_stream PROPERTIES FIXTURES_REQUIRED game_tilegrid_built TIMEOUT 90 LABELS integration)
