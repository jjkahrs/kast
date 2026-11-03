# kast_editor's map edit flags (editor/map_cli.h) end to end, included by CMakeLists.txt. Windowed runs get
# KAST_WINDOW_POS like the rest.
set(KAST_MAPEDIT_OUT "${CMAKE_CURRENT_BINARY_DIR}/mapedit.run")
file(MAKE_DIRECTORY "${KAST_MAPEDIT_OUT}")
# kast_mapbake and kast_world come from the separate services/world build; the tests that need them skip those checks
# if they are not built.
set(KAST_MAPBAKE "" CACHE FILEPATH "kast_mapbake.exe for the map tests; empty = services/world/build/{release,debug}")
if(NOT KAST_MAPBAKE)
  foreach(preset release debug)
    if(NOT KAST_MAPBAKE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_mapbake.exe")
      set(KAST_MAPBAKE "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_mapbake.exe")
    endif()
  endforeach()
endif()
set(KAST_WORLD_EXE "")
foreach(preset release debug)
  if(NOT KAST_WORLD_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_world.exe")
    set(KAST_WORLD_EXE "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_world.exe")
  endif()
endforeach()

set(mapedit_test "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mapedit_test.cmake")
set(starter "${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo")
# mapedit_cli_<name>: mapedit_test.cmake on kast_editor; the remaining arguments are its -D options.
function(kast_mapedit_cli_test name)
  add_test(NAME mapedit_cli_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DOUT_DIR=${KAST_MAPEDIT_OUT}" -DNAME=${name} ${ARGN}
            -P "${mapedit_test}")
endfunction()

foreach(case "meadow;samples/starter-mmo" "cavern;samples/starter-mmo" "hills;tests/project" "crypt;tests/project"
             "tilegrid;tests/project")
  list(GET case 0 map)
  list(GET case 1 project)
  kast_mapedit_cli_test(roundtrip_${map} "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/${project}" -DMODE=roundtrip -DMAP=${map})
endforeach()
# Edits then undo and redo back to none: the save writes the same bytes.
kast_mapedit_cli_test(roundtrip_undo "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=roundtrip -DMAP=hills
  "-DARGS=--sculpt|256,256,40,20|--add-spawn|undone,Bat,200,200|--undo|--redo|--undo|--undo")
kast_mapedit_cli_test(new_map "-DPROJECT=${starter}" -DMODE=new_map -DMAP=plains "-DMAPBAKE=${KAST_MAPBAKE}"
  "-DWORLD=${KAST_WORLD_EXE}")
# Strokes on the corner of all 4 tiles and along a border, an import of an exported tile, a heightRange change: the
# saved heights must pass kast_mapbake's seam lint.
set(map_png "${KAST_MAPEDIT_OUT}/sculpt_0_0.png")
kast_mapedit_cli_test(sculpt "-DPROJECT=${starter}" -DMODE=edit -DMAP=hilly "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|256,256,60,30|--sculpt|256,256,20,-10|--flatten|256,120,25,12|--sculpt|400,300,40,20|--export-png|0,0,${map_png}|--import-png|1,1,${map_png}|--height-range|-10,60"
  "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16")
# Areas (rect, polygon, parent, portal), spawns and a patrol path, one spawn moved across a tile border: saved, linted
# by kast_mapbake and loaded by the world server.
kast_mapedit_cli_test(objects "-DPROJECT=${starter}" -DMODE=edit -DMAP=realm "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|250,250,80,6|--add-area|town,1,20,20,200,200|--add-polygon|ruins,2,300,300,420,300,360,420|--area-parent|ruins,town|--area-portal|ruins,10,128,0,128,0|--add-spawn|guard_a,Guard,100,100,0,30|--add-path|guard_a,2,1,100,100,160,100,160,160|--add-spawn|bat_a,Bat,200,300,1.5,-1|--move-spawn|guard_a,300,120"
  "-DEXPECT_FILES=areas.json|.server/spawns/1_0.json|.server/spawns/0_1.json")
# Map objects: one of each type (a cylinder rock, a box shrine with its server fields), two scatter strokes (the second
# undone), a visual erased and the rock moved across a tile border: saved, linted and baked by kast_mapbake (the
# obstacles cut out), and loaded by the world server (the interactable).
kast_mapedit_cli_test(map_objects "-DPROJECT=${starter}" -DMODE=edit -DMAP=grove "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--add-object|visual,prefabs/grass.prefab,50,50|--add-object|obstacle,prefabs/rock.prefab,100,100,0.5,1,cylinder,2,2|--add-object|interactable,prefabs/shrine.prefab,300,80,0,1,box,1,1,1|--set-interactable|1,4,grove.altar,Altar,{\"gold\": 3}|--scatter|200,200,20,30,5,0.8,1.2,prefabs/grass.prefab+prefabs/flower.prefab|--scatter|220,200,20,30,6,0.8,1.2,prefabs/grass.prefab|--undo|--erase-visuals|50,50,1|--move-object|0,0,0,400,100"
  "-DEXPECT_FILES=objects/0_0.json|objects/1_0.json|.server/interactables/1_0.json")
# Invalid edits block the save: nothing written (meadow: area cavern_gate p10 at [150,124,156,132], spawn guard_1,
# the Meadow Shrine: interactable 1).
foreach(case "overlap|--add-area|dup,10,140,120,160,140" "portal|--area-portal|cavern_gate,999,128,0,128,0"
             "spawn_id|--add-spawn|guard_1,Bat,100,100" "spawn_off_map|--add-spawn|far,Bat,300,100" "map_id|--map-id|11"
             "interactable_event|--set-interactable|1,3,"
             "instance_exit_map|--instance|2,30,999,128,0,128" "instance_exit_portal|--instance|2,30,11,103,0,128")
  string(FIND "${case}" "|" bar)
  string(SUBSTRING "${case}" 0 ${bar} name)
  math(EXPR bar "${bar} + 1")
  string(SUBSTRING "${case}" ${bar} -1 args)
  kast_mapedit_cli_test(blocked_${name} "-DPROJECT=${starter}" -DMODE=blocked -DMAP=meadow "-DARGS=${args}")
endforeach()
# Tiles: a saved map with a hill on its x = 512 edge and a spawn on tile 1_1; then 1_1 removed (its files deleted on
# save) and 2_0 added (taking the hill's edge): the result must pass kast_mapbake's seam lint and load in the world server.
kast_mapedit_cli_test(tiles "-DPROJECT=${starter}" -DMODE=edit -DMAP=isle "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|505,200,40,25|--sculpt|400,400,40,10|--add-spawn|keeper,Guard,400,400|--set-spawn|200,200,1.57"
  "-DARGS2=--remove-tile|1,1|--add-tile|2,0"
  "-DEXPECT_FILES=heights/1_0.r16|heights/2_0.r16"
  "-DEXPECT_GONE=heights/1_1.r16|.server/spawns/1_1.json")
# Generate heights (seed 7), blue-to-red height colors and Create texture & material, saved: the texture and material
# written, seamless and deterministic heights, undo, re-bake after a sculpt; linted by kast_mapbake and loaded by the
# world server.
kast_mapedit_cli_test(terrain_gen "-DPROJECT=${starter}" -DMODE=edit -DMAP=terra "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--generate-heights|7,256,5,0.5,2|--terrain-colors|0,0,0,1,100,1,0,0|--create-terrain-material"
  "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16|../../textures/terra_terrain.png|../../materials/terra_terrain.mat"
  -DTERRAIN_GEN=1)
# An instance map (docs/services/world/instances.html): 2 players, 30 s empty timeout, exit on meadow at its spawn;
# saved, linted by kast_mapbake and loaded by the world server.
kast_mapedit_cli_test(instance "-DPROJECT=${starter}" -DMODE=edit -DMAP=keep "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--instance|2,30,10,128,5,128,1.5")
# Bake Navmesh on a copy of meadow, in the window (the Map tab's Bake section drawing meanwhile) and with --headless.
kast_mapedit_cli_test(bake "-DPROJECT=${starter}" -DMODE=bake -DMAP=meadow "-DMAPBAKE=${KAST_MAPBAKE}")
kast_mapedit_cli_test(bake_headless "-DPROJECT=${starter}" -DMODE=bake -DMAP=meadow "-DMAPBAKE=${KAST_MAPBAKE}" -DHEADLESS=1)
# File > Save (Ctrl+S) with an invalid map (a spawn off the map) and a dirty scene (the Terrain's radius): the map
# writes nothing and its problems go to the Console, the scene still saves.
kast_mapedit_cli_test(file_save_blocked "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=file_save -DMAP=hills -DSCENE=terrain.scene
  "-DARGS=--add-spawn|far,Bat,-50,-50|--set-field|2,Terrain.radius,2" "-DEXPECT_SCENE=\"radius\": 2")
# One Ctrl+S saves it all: the scene (a Sun field), the map (west shrunk to x 0, east painted over x 1) and the zone
# scene east's button created (unsaved until then); the map then reopens and validates.
kast_mapedit_cli_test(unified_save "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=unified_save -DMAP=tilegrid -DSCENE=scenes/tilegrid.scene
  "-DARGS=--set-field|1,DirectionalLight.intensity,2|--paint-zone|east,1,0,1,2|--zone-scene-click|east"
  "-DEXPECT_SCENE=\"intensity\": 2" "-DEXPECT_JSON=\"west\".*\\[\\[0,0,0,2\\]\\]" -DEXPECT_FILES=zones/east.scene)
add_test(NAME editor_mapedit_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_editor>" --project "${starter}" --map meadow --frames 120
          --projects-file "${KAST_MAPEDIT_OUT}/no_focus_projects.json"
  WORKING_DIRECTORY "${KAST_MAPEDIT_OUT}")
set_tests_properties(editor_mapedit_no_focus PROPERTIES TIMEOUT 120 RESOURCE_LOCK no_focus_dir)

# The Map tab's Bake Navmesh (kast_editor --bake) on a copy of tests/project: navtiles of maps/hills written,
# kast_mapbake's output and exit code in the Console, the editor drawing meanwhile. Passes as skipped without kast_mapbake.
add_test(NAME editor_mapedit_bake
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/project"
          -DSCENE=terrain.scene -DMAP=hills "-DMAPBAKE=${KAST_MAPBAKE}" "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/editor_mapedit_bake.run"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/editor_mapedit_bake_test.cmake")
# Play with unsaved map edits (a --sculpt, not saved): the Console warns that Play uses the saved map files.
add_test(NAME editor_mapedit_play_dirty
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_play_dirty.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain.scene --map hills --sculpt 256,256,40,20 --play"
          -DFOCUS_PANEL=Map "-DEXPECT_OUTPUT=Map: maps/hills has unsaved changes. Play uses the saved map files"
          -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# --prompt-remove-tile and --focus: the Map tab's remove-tile confirm dialog (listing the tile's files) over the map,
# the camera close over tile 1_1 of maps/hills.
add_test(NAME editor_mapedit_remove_tile_prompt
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/map_remove_tile_prompt.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain.scene --map-section Tiles --grid-mode tiles --prompt-remove-tile 1,1 --focus 384,384,150 --size 1600x1000"
          -DFOCUS_PANEL=Map "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")

# Zones (zone_edit.h) on a copy of tests/project's maps/tilegrid (west x 0..1 with zones/west.scene, east x 2). A new
# zone painted over the top row, flags and names set, west renamed: its scene file follows on save, unknown keys stay.
kast_mapedit_cli_test(zones_edit "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=zones -DMAP=tilegrid
  "-DARGS=--add-zone|north,Northreach|--paint-zone|north,0,2,2,2|--zone-flags|north,{\"pvp\":true}|--zone-name|east,Eastmarch|--rename-zone|west,vale"
  "-DEXPECT_FILES=zones/vale.scene|zones/vale.scene.meta" "-DEXPECT_GONE=zones/west.scene|zones/west.scene.meta"
  "-DEXPECT_JSON=\"id\": \"north\"|\"name\": \"Northreach\"|\"pvp\": true|\"name\": \"Eastmarch\"|\"id\": \"vale\"|\"note\": \"unknown keys are kept\""
  "-DFORBID_JSON=\"id\": \"west\""
  "-DEXPECT_LOG=Map: added zone north.*Map: painted 3 tile\\(s\\) into zone north.*Map: renamed zone west to vale.*Map: zone vale renamed from west: its scene is maps/tilegrid/zones/vale.scene")
# A deleted zone's tiles go to its edge neighbour (east -> west); a new tile joins its first edge neighbour's zone (3_1:
# -x is 2_1, east's; joining west would split it and block the save).
kast_mapedit_cli_test(zones_delete "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=zones -DMAP=tilegrid "-DARGS=--delete-zone|east"
  "-DEXPECT_FILES=zones/west.scene" "-DEXPECT_JSON=\"id\": \"west\"" "-DFORBID_JSON=\"id\": \"east\""
  "-DEXPECT_LOG=Map: deleted zone east \\(its tiles went to zone west\\)")
kast_mapedit_cli_test(zones_add_tile "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=zones -DMAP=tilegrid "-DARGS=--add-tile|3,1"
  "-DEXPECT_JSON=\"id\": \"east\"")
# Zone edits undone one step each: the save writes the same bytes.
kast_mapedit_cli_test(zones_undo "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=roundtrip -DMAP=tilegrid
  "-DARGS=--add-zone|x|--paint-zone|x,0,0|--zone-flags|east,{}|--rename-zone|east,far|--delete-zone|west|--undo|--undo|--undo|--undo|--undo")
# Invalid zones block the save, nothing written: a paint that splits west, an empty zone, removed tiles that split west,
# every zone deleted (tiles in no zone).
foreach(case "zone_split|--paint-zone|east,0,1" "zone_empty|--add-zone|empty"
             "zone_remove_tile_split|--remove-tile|0,1|--remove-tile|1,1" "zone_unassigned|--delete-zone|east|--delete-zone|west")
  string(FIND "${case}" "|" bar)
  string(SUBSTRING "${case}" 0 ${bar} name)
  math(EXPR bar "${bar} + 1")
  string(SUBSTRING "${case}" ${bar} -1 args)
  kast_mapedit_cli_test(blocked_${name} "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=blocked -DMAP=tilegrid "-DARGS=${args}")
endforeach()
# A zone flag naming an unknown zone fails the run with the form.
add_test(NAME mapedit_cli_zone_unknown
  COMMAND kast_editor --project "${KAST_TEST_PROJECT}" --projects-file "${KAST_MAPEDIT_OUT}/zone_unknown_projects.json"
          --settings-file "${KAST_MAPEDIT_OUT}/zone_unknown_settings.json" --map tilegrid --paint-zone nope,0,0 --headless)
set_tests_properties(mapedit_cli_zone_unknown PROPERTIES
  PASS_REGULAR_EXPRESSION "--paint-zone nope,0,0: expected id,tx,tz\\[,tx2,tz2\\] \\(an existing zone\\)")

# --new-map's size cap (mapedit::kMaxMapTiles, read from map_io.h): the cap a side opens, one more is refused with the
# range in the Console; --add-tile past the cap is refused too. In memory only (--headless, no save).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/engine/mapedit/map_io.h" cap REGEX "kMaxMapTiles = [0-9]+")
string(REGEX MATCH "[0-9]+" cap "${cap}")
math(EXPR over "${cap} + 1")
foreach(case "max|${cap},${cap}|Map: new map maps/big \\(id [0-9]+, ${cap}x${cap} tiles\\)"
             "over|${over},1|--new-map big,${over},1: tiles ${over} x 1: 1 to ${cap} each way"
             "add_over|${cap},1|--add-tile|${cap},0|--add-tile ${cap},0: expected a tile not on the map, within ${cap} tiles a side")
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 name)
  list(GET case 1 size)
  list(GET case -1 expect)
  list(SUBLIST case 2 -1 extra) # more map flags after --new-map
  list(REMOVE_AT extra -1)
  add_test(NAME mapedit_cli_new_map_${name}
    COMMAND kast_editor --project "${starter}" --projects-file "${KAST_MAPEDIT_OUT}/new_map_${name}_projects.json"
            --settings-file "${KAST_MAPEDIT_OUT}/new_map_${name}_settings.json" --new-map "big,${size}" ${extra} --headless)
  set_tests_properties(mapedit_cli_new_map_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
endforeach()

# The Scene view meshes only the tiles within mapedit::kEditRadius (4) of the camera's pivot (the log's mesh count): a new
# 12 x 12 map (in memory, not saved) framed at its centre, 9 x 9; with the camera on its corner tile, 5 x 5. The tile
# borders over 81 tiles pass ImGui's 64k vertices per draw list (the backend's vertex offsets).
foreach(case "centre||81" "corner|--focus 10,10,500|25")
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 name)
  list(GET case 1 focus)
  list(GET case 2 meshes)
  add_test(NAME editor_mapedit_view_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_MAPEDIT_OUT}/view_${name}.png"
            "-DEXTRA_ARGS=--project ${starter} --new-map big,12,12 ${focus} --size 1600x900" -DFRAMES=30
            "-DEXPECT_OUTPUT=Map view: ${meshes} terrain meshes" -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()

# The Map tab's tile grid on a new cap-size map (in memory, not saved), in its Tile scenes mode and (--grid-mode tiles)
# its Add / remove tiles mode, with a free cell around the map: fitted whole by default (cells too small for
# thumbnails: none), --grid-view zoomed on its centre, and on its far corner, where --map-click 60,60 creates that
# tile's scene. Only the visible cells get thumbnails.
math(EXPR cap_last "${cap} - 1")
foreach(case "default||Map: view 0_0\\.\\.${cap_last}_${cap_last} \\([0-9]+ px cells, 0 thumbnails\\)|Map: new map maps/big \\(id [0-9]+, ${cap}x${cap} tiles\\)"
             "zoomed|--grid-view 32,32,16|Map: view [23][0-9]_[23][0-9]\\.\\.3[2-9]_3[2-9] \\([0-9]+ px cells, [1-9][0-9] thumbnails\\)|Map: tile grid view [23][0-9]_[23][0-9]\\.\\.3[2-9]_3[2-9] "
             "panned|--grid-view 60,60,16 --map-click 60,60|Map: view 5[0-9]_5[0-9]\\.\\.${cap_last}_${cap_last} \\([0-9]+ px cells, [1-9][0-9] thumbnails\\).*Map: created maps/big/scenes/60_60.scene for tile 60_60|Map: tile grid view 5[0-9]_5[0-9]\\.\\.${cap}_${cap} ")
  string(REPLACE "|" ";" case "${case}")
  list(GET case 0 name)
  list(GET case 1 view)
  list(GET case 2 map_expect)
  list(GET case 3 tiles_expect)
  add_test(NAME editor_map_tab_big_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_MAPEDIT_OUT}/map_tab_big_${name}.png"
            "-DEXTRA_ARGS=--project ${starter} --new-map big,${cap},${cap} ${view} --size 1600x1000" -DFOCUS_PANEL=Map
            -DFRAMES=60 "-DEXPECT_OUTPUT=${map_expect}" "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  add_test(NAME editor_map_tiles_big_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${KAST_MAPEDIT_OUT}/map_tiles_big_${name}.png"
            "-DEXTRA_ARGS=--project ${starter} --new-map big,${cap},${cap} --map-section Tiles --grid-mode tiles ${view} --size 1600x1000"
            -DFOCUS_PANEL=Map -DFRAMES=60 "-DEXPECT_OUTPUT=${tiles_expect}" "-DFORBID_OUTPUT=\\[error\\]"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
