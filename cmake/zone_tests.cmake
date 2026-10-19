# kast_editor's zone edit flags (editor/zone_cli.h) end to end, included by CMakeLists.txt. Windowed runs get
# KAST_WINDOW_POS like the rest.
set(KAST_ZONE_OUT "${CMAKE_CURRENT_BINARY_DIR}/zone.run")
file(MAKE_DIRECTORY "${KAST_ZONE_OUT}")
# kast_mapbake and kast_world come from the separate services/world build; the tests that need them skip those checks
# if they are not built.
set(KAST_MAPBAKE "" CACHE FILEPATH "kast_mapbake.exe for the zone tests; empty = services/world/build/{release,debug}")
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

set(zone_test "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zone_test.cmake")
set(starter "${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo")
# zone_cli_<name>: zone_test.cmake on kast_editor; the remaining arguments are its -D options.
function(kast_zone_cli_test name)
  add_test(NAME zone_cli_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=${name} ${ARGN}
            -P "${zone_test}")
endfunction()

foreach(case "meadow;samples/starter-mmo" "cavern;samples/starter-mmo" "hills;tests/project")
  list(GET case 0 map)
  list(GET case 1 project)
  kast_zone_cli_test(roundtrip_${map} "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/${project}" -DMODE=roundtrip -DMAP=${map})
endforeach()
# Edits then undo and redo back to none: the save writes the same bytes.
kast_zone_cli_test(roundtrip_undo "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=roundtrip -DMAP=hills
  "-DARGS=--sculpt|256,256,40,20|--add-spawn|undone,Bat,200,200|--undo|--redo|--undo|--undo")
kast_zone_cli_test(new_map "-DPROJECT=${starter}" -DMODE=new_map -DMAP=plains "-DMAPBAKE=${KAST_MAPBAKE}"
  "-DWORLD=${KAST_WORLD_EXE}")
# Strokes on the corner of all 4 tiles and along a border, an import of an exported tile, a heightRange change: the
# saved heights must pass kast_mapbake's seam lint.
set(zone_png "${KAST_ZONE_OUT}/sculpt_0_0.png")
kast_zone_cli_test(sculpt "-DPROJECT=${starter}" -DMODE=edit -DMAP=hilly "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|256,256,60,30|--sculpt|256,256,20,-10|--flatten|256,120,25,12|--sculpt|400,300,40,20|--export-png|0,0,${zone_png}|--import-png|1,1,${zone_png}|--height-range|-10,60"
  "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16")
# Areas (rect, polygon, parent, portal), spawns and a patrol path, one spawn moved across a tile border: saved, linted
# by kast_mapbake and loaded by the world server.
kast_zone_cli_test(objects "-DPROJECT=${starter}" -DMODE=edit -DMAP=realm "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|250,250,80,6|--add-area|town,1,20,20,200,200|--add-polygon|ruins,2,300,300,420,300,360,420|--area-parent|ruins,town|--area-portal|ruins,10,128,0,128,0|--add-spawn|guard_a,Guard,100,100,0,30|--add-path|guard_a,2,1,100,100,160,100,160,160|--add-spawn|bat_a,Bat,200,300,1.5,-1|--move-spawn|guard_a,300,120"
  "-DEXPECT_FILES=areas.json|.server/spawns/1_0.json|.server/spawns/0_1.json")
# Zone objects: one of each type (a cylinder rock, a box shrine with its server fields), two scatter strokes (the second
# undone), a visual erased and the rock moved across a tile border: saved, linted and baked by kast_mapbake (the
# obstacles cut out), and loaded by the world server (the interactable).
kast_zone_cli_test(zone_objects "-DPROJECT=${starter}" -DMODE=edit -DMAP=grove "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--add-object|visual,prefabs/grass.prefab,50,50|--add-object|obstacle,prefabs/rock.prefab,100,100,0.5,1,cylinder,2,2|--add-object|interactable,prefabs/shrine.prefab,300,80,0,1,box,1,1,1|--set-interactable|1,4,grove.altar,Altar,{\"gold\": 3}|--scatter|200,200,20,30,5,0.8,1.2,prefabs/grass.prefab+prefabs/flower.prefab|--scatter|220,200,20,30,6,0.8,1.2,prefabs/grass.prefab|--undo|--erase-visuals|50,50,1|--move-object|0,0,0,400,100"
  "-DEXPECT_FILES=objects/0_0.json|objects/1_0.json|.server/interactables/1_0.json")
# Invalid edits block the save: nothing written (meadow: area cavern_gate p10 at [150,124,156,132], spawn guard_1,
# the Meadow Shrine: interactable 1).
foreach(case "overlap|--add-area|dup,10,140,120,160,140" "portal|--area-portal|cavern_gate,999,128,0,128,0"
             "spawn_id|--add-spawn|guard_1,Bat,100,100" "spawn_off_map|--add-spawn|far,Bat,300,100" "map_id|--map-id|11"
             "interactable_event|--set-interactable|1,3,")
  string(FIND "${case}" "|" bar)
  string(SUBSTRING "${case}" 0 ${bar} name)
  math(EXPR bar "${bar} + 1")
  string(SUBSTRING "${case}" ${bar} -1 args)
  kast_zone_cli_test(blocked_${name} "-DPROJECT=${starter}" -DMODE=blocked -DMAP=meadow "-DARGS=${args}")
endforeach()
# Tiles: a saved map with a hill on its x = 512 edge and a spawn on tile 1_1; then 1_1 removed (its files deleted on
# save) and 2_0 added (taking the hill's edge): the result must pass kast_mapbake's seam lint and load in the world server.
kast_zone_cli_test(tiles "-DPROJECT=${starter}" -DMODE=edit -DMAP=isle "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--material|materials/meadow_grass.mat|--sculpt|505,200,40,25|--sculpt|400,400,40,10|--add-spawn|keeper,Guard,400,400|--set-spawn|200,200,1.57"
  "-DARGS2=--remove-tile|1,1|--add-tile|2,0"
  "-DEXPECT_FILES=heights/1_0.r16|heights/2_0.r16"
  "-DEXPECT_GONE=heights/1_1.r16|.server/spawns/1_1.json")
# Generate heights (seed 7), blue-to-red height colors and Create texture & material, saved: the texture and material
# written, seamless and deterministic heights, undo, re-bake after a sculpt; linted by kast_mapbake and loaded by the
# world server.
kast_zone_cli_test(terrain_gen "-DPROJECT=${starter}" -DMODE=edit -DMAP=terra "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
  "-DARGS=--generate-heights|7,256,5,0.5,2|--terrain-colors|0,0,0,1,100,1,0,0|--create-terrain-material"
  "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16|../../textures/terra_terrain.png|../../materials/terra_terrain.mat"
  -DTERRAIN_GEN=1)
# Bake Navmesh on a copy of meadow, in the window (the Zone tab's Bake section drawing meanwhile) and with --headless.
kast_zone_cli_test(bake "-DPROJECT=${starter}" -DMODE=bake -DMAP=meadow "-DMAPBAKE=${KAST_MAPBAKE}")
kast_zone_cli_test(bake_headless "-DPROJECT=${starter}" -DMODE=bake -DMAP=meadow "-DMAPBAKE=${KAST_MAPBAKE}" -DHEADLESS=1)
# File > Save (Ctrl+S) with an invalid zone (a spawn off the map) and a dirty scene (the Terrain's radius): the zone
# writes nothing and its problems go to the Console, the scene still saves.
kast_zone_cli_test(file_save_blocked "-DPROJECT=${KAST_TEST_PROJECT}" -DMODE=file_save -DMAP=hills -DSCENE=terrain.scene
  "-DARGS=--add-spawn|far,Bat,-50,-50|--set-field|2,Terrain.radius,2" "-DEXPECT_SCENE=\"radius\": 2")
add_test(NAME editor_zone_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_editor>" --project "${starter}" --map meadow --frames 120
          --projects-file "${KAST_ZONE_OUT}/no_focus_projects.json"
  WORKING_DIRECTORY "${KAST_ZONE_OUT}")
set_tests_properties(editor_zone_no_focus PROPERTIES TIMEOUT 120 RESOURCE_LOCK no_focus_dir)

# The Zone tab's Bake Navmesh (kast_editor --bake) on a copy of tests/project: navtiles of maps/hills written,
# kast_mapbake's output and exit code in the Console, the editor drawing meanwhile. Passes as skipped without kast_mapbake.
add_test(NAME editor_zone_bake
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/project"
          -DSCENE=terrain.scene -DMAP=hills "-DMAPBAKE=${KAST_MAPBAKE}" "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/editor_zone_bake.run"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/editor_zone_bake_test.cmake")
# Play with unsaved zone edits (a --sculpt, not saved): the Console warns that Play uses the saved map files.
add_test(NAME editor_zone_play_dirty
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_play_dirty.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain.scene --map hills --sculpt 256,256,40,20 --play"
          -DFOCUS_PANEL=Zone "-DEXPECT_OUTPUT=Zone: maps/hills has unsaved changes. Play uses the saved map files"
          -DEXPECT_COUNT=1 "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
# --prompt-remove-tile and --focus: the Zone tab's remove-tile confirm dialog (listing the tile's files) over the zone,
# the camera close over tile 1_1 of maps/hills.
add_test(NAME editor_zone_remove_tile_prompt
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/zone_remove_tile_prompt.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/terrain.scene --zone-section Map --prompt-remove-tile 1,1 --focus 384,384,150 --size 1600x1000"
          -DFOCUS_PANEL=Zone "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
