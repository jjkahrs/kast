# The Map tab and tile scenes (docs/editor/map-builder.html, docs/engine/terrain.html#tile-scenes) on tests/project's
# scenes/tilegrid.scene: a 3 x 3 map (maps/tilegrid, id 7, Terrain radius 0) with tile scenes on 0_0, 1_0, 0_1 (its
# "Stray Rock" root stands in tile 2_1) and 1_1, and an orphan on the undeclared 3_1. Tests that write use a copy
# (map_test.cmake, @PROJECT@). Included from CMakeLists.txt.
set(KAST_TILEGRID "${KAST_TEST_PROJECT}/scenes/tilegrid.scene")
set(map_smoke "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set(map_copy "${CMAKE_CURRENT_SOURCE_DIR}/cmake/map_test.cmake")
set(map_orphan "Map: maps/tilegrid/scenes/3_1.scene is an orphan: tile 3_1 is not a map tile")
set(map_bounds "Map: maps/tilegrid/scenes/0_1.scene: root entity 'Stray Rock' at \\(600, 400\\) is in tile 2_1, outside tile 0_1")

# The tab docked beside Entities (default layout; tests/layouts/pre_map.ini, a layout saved with the old Zone tab and no
# Map tab: Map takes the Zone tab's place; pre_mapedit.ini, saved before either), the 3 x 3 grid in its 8 x 8 bounds.
# A saved Zone window is dropped (zone_own_dock.ini: Zone alone in a dock node below Map), leaving no empty window.
add_test(NAME editor_map_tab
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map "-DLAYOUT_SAME_DOCK=Entities;Map"
          "-DEXPECT_OUTPUT=Map: maps/tilegrid \\(9 tiles, 5 tile scenes\\)" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_tab_old_layout
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab_old_layout.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/pre_map.ini" "-DLAYOUT_SAME_DOCK=Entities;Map" -DLAYOUT_FIRST_TAB=Entities
          "-DLAYOUT_MUST_NOT_CONTAIN=[Window][Zone]" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_tab_zone_own_dock
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab_zone_own_dock.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/zone_own_dock.ini" "-DLAYOUT_SAME_DOCK=Entities;Map"
          "-DLAYOUT_MUST_NOT_CONTAIN=[Window][Zone]" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_tab_pre_mapedit_layout
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_tab_pre_mapedit_layout.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000" -DFOCUS_PANEL=Map
          "-DLAYOUT_FROM=${CMAKE_CURRENT_SOURCE_DIR}/tests/layouts/pre_mapedit.ini" "-DLAYOUT_SAME_DOCK=Entities;Map;Systems"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
# No Terrain entity, no project: the tab says so, and nothing is logged.
add_test(NAME editor_map_no_terrain
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_no_terrain.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/scene_b.scene --map-click 0,0" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=Map: |\\[error\\]|New scene added" -P "${map_smoke}")
add_test(NAME editor_map_no_project
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_no_project.png"
          "-DEXTRA_ARGS=--scene ${CMAKE_CURRENT_BINARY_DIR}/no_project_basic.scene" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=Map: |\\[error\\]" -P "${map_smoke}")

# A click on 1_0 (it has a scene) opens it additively, not active (the Entities panel's new header); a second click closes it.
add_test(NAME editor_map_click_open
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_open.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 1,0" -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=Opened scene '[^']*maps/tilegrid/scenes/1_0.scene' additively.*Map: opened tile 1_0 \\(maps/tilegrid/scenes/1_0.scene\\)"
          "-DFORBID_OUTPUT=\\[error\\]|Active scene" -P "${map_smoke}")
add_test(NAME editor_map_click_open_hierarchy
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_click_open_hierarchy.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-click 1,0" -DFOCUS_PANEL=Entities
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
# Removing tile 1_1 in the map (saved) leaves its scene on disk, now an orphan.
add_test(NAME editor_map_orphan_after_remove
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_orphan_after_remove.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map tilegrid --remove-tile 1,1 --save-map --no-recycle"
          -DFOCUS_PANEL=Map "-DEXPECT_ONCE=Map: maps/tilegrid/scenes/1_1.scene is an orphan"
          "-DMUST_EXIST=@PROJECT@/maps/tilegrid/scenes/1_1.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")

# Zones (docs/editor/map-builder.html#zones): maps/tilegrid's zones.json has west (x 0..1, zones/west.scene: "West
# Gate" on tile 1_1) and east (x 2). The Zones section with west selected, its borders and names on the grid.
add_test(NAME editor_map_zones
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zones.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-section Tiles,Zones --grid-mode zones --map-select zone:west"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: maps/tilegrid \\(9 tiles, 5 tile scenes\\)"
          "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\] Map: maps/tilegrid/zones" -P "${map_smoke}")
# Zone scenes: a click on west's scene button opens it additively, a second closes it; east has none: created empty,
# unsaved until Ctrl+S writes maps/tilegrid/zones/east.scene.
add_test(NAME editor_map_zone_scene_open_close
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_scene_open_close.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-section Zones --zone-scene-click west --zone-scene-click west"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Opened scene '[^']*maps/tilegrid/zones/west.scene' additively.*Map: opened zone west \\(maps/tilegrid/zones/west.scene\\).*Map: close zone west.*Removed scene 'west'"
          "-DFORBID_OUTPUT=\\[error\\]|Active scene" -P "${map_smoke}")
add_test(NAME editor_map_zone_scene_create
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_scene_create.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-section Zones --map-select zone:east --zone-scene-click east"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=New scene added.*Map: created maps/tilegrid/zones/east.scene for zone east \\(unsaved"
          "-DMUST_NOT_EXIST=@PROJECT@/maps/tilegrid/zones/east.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
add_test(NAME editor_map_zone_scene_create_save
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_scene_create_save.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --zone-scene-click east --sim-key 30,ctrl+S"
          -DFOCUS_PANEL=Scene "-DEXPECT_OUTPUT=Map: created maps/tilegrid/zones/east.scene.*Saved scene '[^']*maps/tilegrid/zones/east.scene'"
          "-DMUST_EXIST=@PROJECT@/maps/tilegrid/zones/east.scene" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
# A zone renamed (west -> vale, unsaved) keeps its scene: vale's button opens zones/west.scene; Ctrl+S saves the map,
# renaming the file to zones/vale.scene, and the open scene follows it (else zones/west.scene would show as an orphan).
add_test(NAME editor_map_zone_rename_save
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_rename_save.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map tilegrid --rename-zone west,vale --map-section Zones --zone-scene-click vale --sim-key 30,ctrl+S --no-recycle"
          -DFOCUS_PANEL=Scene -DFRAMES=120 "-DEXPECT_OUTPUT=Map: opened zone vale \\(maps/tilegrid/zones/west.scene\\).*Map: saved maps/tilegrid.*Map: zone vale renamed from west: its scene is maps/tilegrid/zones/vale.scene"
          "-DMUST_EXIST=@PROJECT@/maps/tilegrid/zones/vale.scene" "-DMUST_NOT_EXIST=@PROJECT@/maps/tilegrid/zones/west.scene"
          "-DFORBID_OUTPUT=\\[error\\]|zones/[a-z]+.scene is an orphan" -P "${map_copy}")
# Warnings (never blocking): west deleted (unsaved) leaves zones/west.scene an orphan; 1_1 painted into east puts West
# Gate outside west.
add_test(NAME editor_map_zone_orphan
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_orphan.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map tilegrid --delete-zone west --map-section Zones"
          -DFOCUS_PANEL=Map -DFRAMES=120 "-DEXPECT_ONCE=Map: maps/tilegrid/zones/west.scene is an orphan: the map has no zone \"west\""
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
add_test(NAME editor_map_zone_root_outside
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_root_outside.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map tilegrid --paint-zone east,1,1 --map-section Tiles,Zones --grid-mode zones --map-select zone:east"
          -DFOCUS_PANEL=Map -DFRAMES=120
          "-DEXPECT_ONCE=Map: maps/tilegrid/zones/west.scene: root entity 'West Gate' at \\(300, 300\\) is in zone east, outside zone west"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
# Read-only during Play: the zone scene button does nothing.
add_test(NAME editor_map_zone_play_locked
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_play_locked.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --play --map-section Zones --zone-scene-click west"
          -DFOCUS_PANEL=Map "-DFORBID_OUTPUT=\\[error\\]|Map: (opened|close|created) zone" -P "${map_smoke}")

# Play (scripts/map_stream.lua moves the camera 0_0 -> 1_0 -> 1_1 -> 2_0 -> 1_0): the open 1_1 unloads (out of range),
# tile scenes stream in and out with their events and kast.loadedScenes() (west's zone scene, loaded on the second
# frame, among them); Ctrl+P stops and the open set comes back.
set(map_stream_head "map stream: loaded map 7 tile 0,0 maps/tilegrid/scenes/0_0.scene.*map stream: unloaded map 7 tile 0,0 maps/tilegrid/scenes/0_0.scene.*map stream: loaded map 7 tile 1,0 maps/tilegrid/scenes/1_0.scene.*map stream: unloaded map 7 tile 1,0.*map stream: loaded map 7 tile 1,1.*map stream: scenes scenes/tilegrid.scene,maps/tilegrid/zones/west.scene,maps/tilegrid/scenes/1_1.scene")
set(map_stream_tail "map stream: unloaded map 7 tile 1,1.*map stream: loaded map 7 tile 1,0.*map stream: scenes scenes/tilegrid.scene,maps/tilegrid/scenes/1_0.scene")
add_test(NAME editor_map_play_stream
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_play_stream.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --open-additive ${KAST_TEST_PROJECT}/maps/tilegrid/scenes/1_1.scene --play --sim-key 170,ctrl+P --size 1600x1000"
          -DFOCUS_PANEL=Map -DFRAMES=230
          "-DEXPECT_OUTPUT=Unloaded scene 'maps/tilegrid/scenes/1_1.scene'.*map stream: unloaded map 7 tile 1,1 .*${map_stream_head}.*${map_stream_tail}.*Stop"
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
# Zones in Play (docs/engine/zones.html, scripts/map_zones.lua): zone.entered west alone at the start, with kast.map.zone();
# west's zone scene streams in; kast.map.zoneAt() in each zone and off the map; into east (frame 80) zone.left then
# zone.entered and west's scene unloads (east has none); back to west; off the map (frame 130) zone.left alone and
# kast.map.zone() is nil. Nothing blocks: no scene switch.
set(map_zones_start "map zones: entered map 7 zone west 'West Vale' pvp=false zone\\(\\)=west,West Vale,false.*map zones: scene loaded map 7 zone west maps/tilegrid/zones/west.scene.*map zones: zoneAt west,east,nil,nil")
set(map_zones_east "map zones: left map 7 zone west.*map zones: entered map 7 zone east 'East March' pvp=true zone\\(\\)=east,East March,true")
set(map_zones_back "map zones: left map 7 zone east.*map zones: entered map 7 zone west.*map zones: scene loaded map 7 zone west.*map zones: left map 7 zone west.*map zones: scene unloaded map 7 zone west.*map zones: off the map zone\\(\\)=nil")
add_test(NAME editor_map_play_zones
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_play_zones.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --play --sim-key 170,ctrl+P --size 1600x1000" -DFOCUS_PANEL=Console -DFRAMES=230
          "-DEXPECT_OUTPUT=${map_zones_start}.*${map_zones_east}.*map zones: scene unloaded map 7 zone west.*${map_zones_back}.*Stop"
          "-DEXPECT_ONCE=map zones: entered map 7 zone east;map zones: left map 7 zone east;map zones: off the map"
          "-DFORBID_OUTPUT=\\[error\\]|Switched to scene" -P "${map_smoke}")
# Unified undo (docs/editor/undo-redo.html#documents): a scene edit (--set-field), a map edit (--paint-zone) and a
# Scene view Ctrl+D, then real Ctrl+Z x3 / Ctrl+Y x3 with the Scene view (or the Map tab) focused: the latest edit is undone
# first, map or scene, and redone in the order made. Nothing is saved.
set(map_undo_args "--scene ${KAST_TILEGRID} --size 1600x1000 --set-field 1,DirectionalLight.intensity,2 --map tilegrid --paint-zone east,1,0,1,2 --select 3 --sim-key 20,ctrl+D --sim-key 30,ctrl+Z --sim-key 40,ctrl+Z --sim-key 50,ctrl+Z --sim-key 60,ctrl+Y --sim-key 70,ctrl+Y --sim-key 80,ctrl+Y")
set(map_undo_order "Undo Duplicate Main Camera.*Undo Edit Zones.*Undo Set DirectionalLight.intensity.*Redo Set DirectionalLight.intensity.*Redo Edit Zones.*Redo Duplicate Main Camera")
add_test(NAME editor_map_undo_order_scene_focus
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_undo_order_scene_focus.png"
          "-DEXTRA_ARGS=${map_undo_args}" -DFOCUS_PANEL=Scene -DFRAMES=100 "-DEXPECT_OUTPUT=${map_undo_order}"
          "-DFORBID_OUTPUT=\\[error\\]|Saved" -P "${map_smoke}")
# The Map tab focused: Ctrl+D is the Scene view's, so the scene edits are --set-field (startup) and nothing after the map's.
add_test(NAME editor_map_undo_order_map_focus
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_undo_order_map_focus.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --set-field 1,DirectionalLight.intensity,2 --map tilegrid --paint-zone east,1,0,1,2 --sim-key 30,ctrl+Z --sim-key 40,ctrl+Z --sim-key 50,ctrl+Y --sim-key 60,ctrl+Y"
          -DFOCUS_PANEL=Map -DFRAMES=80
          "-DEXPECT_OUTPUT=Undo Edit Zones.*Undo Set DirectionalLight.intensity.*Redo Set DirectionalLight.intensity.*Redo Edit Zones"
          "-DFORBID_OUTPUT=\\[error\\]|Saved" -P "${map_smoke}")
# Only map edits pending (the scene itself clean): opening another scene asks first, naming the map; a --frames run quits
# at the prompt, so nothing is opened or saved.
add_test(NAME editor_map_unsaved_prompt
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_unsaved_prompt.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map tilegrid --paint-zone east,1,0,1,2 --open-scene ${KAST_TEST_PROJECT}/additive_a.scene"
          -DFOCUS_PANEL=Scene -DFRAMES=40 "-DEXPECT_OUTPUT=Unsaved changes: tilegrid and maps/tilegrid"
          "-DFORBID_OUTPUT=\\[error\\]|Opened scene '[^']*additive_a|Saved" -P "${map_smoke}")
# The New Map wizard (docs/editor/map-builder.html#new-map). On the copy's empty scene one Create adds the Terrain
# entity, makes maps/highlands (folder from the name) with one zone of every tile and the height range, generated
# heights, frames the map in the Scene view (the saved camera pivot is its centre), and saves the scene and the map.
set(map_wizard_files [=[@PROJECT@/maps/highlands/map.json="name":"Highlands","tiles":\[\[0,0,2,1\]\],"heightRange":\[-10\.0,60\.0\];@PROJECT@/maps/highlands/zones.json="zones":\[{"id":"highlands","name":"Highlands","tiles":\[\[0,0,2,1\]\]}\];@PROJECT@/scenes/empty.scene="name":"Terrain".*"Terrain":{"map":0,;@PROJECT@/scenes/empty.scene="camera":{"pivot":\[384\.0,[0-9.]+,256\.0\]]=])
add_test(NAME editor_map_wizard_create
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_wizard_create.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/empty.scene --size 1600x1000 --map-wizard Highlands,3,2,-10,60,generated --map-wizard-create"
          -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=Map: new map maps/highlands \\(id 0, 3x2 tiles\\).*Map: added a Terrain entity.*Map: New Map maps/highlands: 3 x 2 tiles, heights -10 .. 60 m, generated terrain, one zone.*Saved scene '[^']*scenes/empty.scene'.*Map: saved maps/highlands"
          "-DMUST_EXIST=@PROJECT@/maps/highlands/heights/0_0.r16;@PROJECT@/maps/highlands/heights/2_1.r16"
          "-DFILE_MATCHES=${map_wizard_files}" "-DFORBID_OUTPUT=\\[error\\]|asking" -P "${map_copy}")
# The folder is taken (the name Tilegrid makes maps/tilegrid): Create is disabled, nothing is made or saved.
add_test(NAME editor_map_wizard_folder_exists
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_wizard_folder_exists.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/empty.scene --size 1600x1000 --map-wizard Tilegrid --map-wizard-create"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: New Map: Create is disabled: maps/tilegrid already exists"
          "-DFORBID_OUTPUT=\\[error\\]|Map: new map|Saved|Terrain entity" -P "${map_copy}")
# The scene's Terrain already names maps/tilegrid: Create asks before replacing the link (nothing made yet); Replace
# makes and saves maps/highlands (flat, 2 x 2, 0 .. 100 m) and points the Terrain at it. maps/tilegrid keeps every file.
add_test(NAME editor_map_wizard_replace_ask
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_wizard_replace_ask.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-wizard Highlands --map-wizard-create"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: New Map: the scene's Terrain uses maps/tilegrid. asking before replacing the link"
          "-DMUST_NOT_EXIST=@PROJECT@/maps/highlands" "-DKEEP_FILES=@PROJECT@/maps/tilegrid"
          "-DFORBID_OUTPUT=\\[error\\]|Map: new map|Saved" -P "${map_copy}")
add_test(NAME editor_map_wizard_replace_confirm
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_wizard_replace_confirm.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-wizard Highlands --map-wizard-create --map-wizard-confirm"
          -DFOCUS_PANEL=Map
          "-DEXPECT_OUTPUT=asking before replacing the link.*Map: New Map maps/highlands: 2 x 2 tiles, heights 0 .. 100 m, flat terrain, one zone.*Saved scene '[^']*scenes/tilegrid.scene'.*Map: saved maps/highlands"
          "-DFILE_MATCHES=@PROJECT@/scenes/tilegrid.scene=\"Terrain\":{\"map\":0,;@PROJECT@/maps/highlands/zones.json=\"tiles\":\\[\\[0,0,1,1\\]\\]"
          "-DKEEP_FILES=@PROJECT@/maps/tilegrid" "-DFORBID_OUTPUT=\\[error\\]" -P "${map_copy}")
# Sub-tabs: --map-section Zones selects the Zones sub-tab, whose tile grid paints zones (the Paint zone mode).
add_test(NAME editor_map_subtab_zones
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_subtab_zones.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-section Zones --map-select zone:west"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: Zones tab: the tile grid paints zones" -DEXPECT_COUNT=1
          "-DFORBID_OUTPUT=\\[error\\]|Terrain tab" -P "${map_smoke}")
# Zones in the Scene view (Zones sub-tab, zone east selected): a left drag over the terrain from 2_1 (east) to 1_1
# paints the tiles it crosses into east, one stroke; Ctrl+S writes zones.json with 1_1 in east.
add_test(NAME editor_map_viewport_zone_paint
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_viewport_zone_paint.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-section Zones --map-select zone:east --sim-mouse 40,left,-170,0,0,170,0 --sim-key 60,ctrl+S"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: painted 1 tile\\(s\\) into zone east.*Map: saved maps/tilegrid" -DEXPECT_COUNT=1
          "-DFILE_MATCHES=@PROJECT@/maps/tilegrid/zones.json=\"id\":\"east\",\"name\":\"EastMarch\",\"tiles\":\\[\\[2,0,2,0\\],\\[1,1,2,1\\],\\[2,2,2,2\\]\\]"
          "-DFORBID_OUTPUT=\\[error\\]|can't paint" -P "${map_copy}")
# A click on 0_1 with east selected (0_1 touches no east tile, and would split west's column) is refused: the
# Console and the Zones tab say why, the zones stay as they were (nothing to save: zones.json untouched).
add_test(NAME editor_map_viewport_zone_refused
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${KAST_TEST_PROJECT}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
          "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_viewport_zone_refused.png"
          "-DEXTRA_ARGS=--scene @PROJECT@/scenes/tilegrid.scene --size 1600x1000 --map-section Zones --map-select zone:east --sim-mouse 40,left,0,0,0,-170,0 --sim-key 60,ctrl+S"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=\\[warn\\] Map: can't paint tile 0_1 into zone east: zone \"(east|west)\": not contiguous" -DEXPECT_COUNT=1
          "-DFILE_MATCHES=@PROJECT@/maps/tilegrid/zones.json=\"id\":\"west\",\"name\":\"WestVale\",\"tiles\":\\[\\[0,0,1,2\\]\\].*\"id\":\"east\",\"name\":\"EastMarch\",\"tiles\":\\[\\[2,0,2,2\\]\\]"
          "-DFORBID_OUTPUT=\\[error\\]|Map: painted|Map: saved" -P "${map_copy}")
# The zone overlay: each zone tinted with its name on the terrain while the Zones sub-tab shows (not on Setup).
add_test(NAME editor_map_zone_overlay_setup
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_zone_overlay_setup.png"
          "-DEXTRA_ARGS=--scene ${KAST_TILEGRID} --size 1600x1000 --map-section Setup" -DFOCUS_PANEL=Map
          "-DFORBID_OUTPUT=\\[error\\]" -P "${map_smoke}")
add_test(NAME editor_map_zone_overlay_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/map_subtab_zones.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/map_zone_overlay_setup.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(editor_map_subtab_zones editor_map_zone_overlay_setup PROPERTIES FIXTURES_SETUP map_zone_overlay_shots)
set_tests_properties(editor_map_zone_overlay_differs PROPERTIES FIXTURES_REQUIRED map_zone_overlay_shots)
set_tests_properties(editor_map_tab editor_map_tab_old_layout editor_map_tab_pre_mapedit_layout editor_map_tab_zone_own_dock
                     editor_map_no_terrain
                     editor_map_no_project editor_map_click_open
                     editor_map_click_open_hierarchy editor_map_click_close editor_map_click_undeclared
                     editor_map_click_create editor_map_click_create_save editor_map_click_create_close_prompt
                     editor_map_warnings editor_map_orphan_after_remove editor_map_play_stream editor_map_zones
                     editor_map_zone_scene_open_close editor_map_zone_scene_create editor_map_zone_scene_create_save
                     editor_map_zone_rename_save editor_map_zone_orphan editor_map_zone_root_outside editor_map_zone_play_locked
                     editor_map_play_zones editor_map_undo_order_scene_focus editor_map_undo_order_map_focus
                     editor_map_unsaved_prompt editor_map_wizard_create editor_map_wizard_folder_exists
                     editor_map_wizard_replace_ask editor_map_wizard_replace_confirm editor_map_subtab_zones
                     editor_map_viewport_zone_paint editor_map_viewport_zone_refused editor_map_zone_overlay_setup
                     PROPERTIES LABELS smoke)

# The built player (game "tilegrid" in KAST_GAMES, its copy's 2_0.scene broken): the same streaming path; the broken
# scene logs one error and streaming goes on (1_0 loads again after it).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/tilegrid/maps/tilegrid/scenes/2_0.scene" "{ not a scene\n")
add_test(NAME player_tilegrid_stream
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/tilegrid" "-DEXE=Tile Grid Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/tilegrid_stream.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_tilegrid_stream.png"
          -DEXPECT=0 "-DEXPECT_OUTPUT=${map_stream_head}.*Map: tile scene 'maps/tilegrid/scenes/2_0.scene' did not load.*${map_stream_tail}"
          "-DEXPECT_ONCE=Map: tile scene 'maps/tilegrid/scenes/2_0.scene' did not load" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_tilegrid_stream PROPERTIES FIXTURES_REQUIRED game_tilegrid_built TIMEOUT 90 LABELS integration)
# The same zones in the player (run to frame 160), from game.pak's zones.json and zones/*.scene: its copy has an
# east.scene too (the "East Post" on tile 2_0), which streams in on the crossing, after the broken 2_0 tile scene is skipped.
file(READ "${KAST_TEST_PROJECT}/maps/tilegrid/zones/west.scene" east_scene)
string(REPLACE "West Gate" "East Post" east_scene "${east_scene}")
string(REPLACE "[300, 25, 300]" "[640, 25, 60]" east_scene "${east_scene}") # on tile 2_0, in view on the crossing
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/tilegrid/maps/tilegrid/zones/east.scene" "${east_scene}")
add_test(NAME player_tilegrid_zones
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/tilegrid" "-DEXE=Tile Grid Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/tilegrid_zones.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_tilegrid_zones.png"
          -DEXPECT=0 "-DEXTRA_ARGS=--frames 160"
          "-DEXPECT_OUTPUT=${map_zones_start}.*${map_zones_east}.*Map: tile scene 'maps/tilegrid/scenes/2_0.scene' did not load.*map zones: scene loaded map 7 zone east maps/tilegrid/zones/east.scene.*${map_zones_back}"
          "-DEXPECT_ONCE=map zones: entered map 7 zone east;map zones: left map 7 zone east;map zones: off the map;map zones: scene unloaded map 7 zone east"
          "-DFORBID_OUTPUT=zone scene '|Switched to scene" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_tilegrid_zones PROPERTIES FIXTURES_REQUIRED game_tilegrid_built TIMEOUT 90 LABELS integration)
