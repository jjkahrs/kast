# Zone builder tests (tools/zone-builder), included by CMakeLists.txt. Windowed runs get KAST_WINDOW_POS like the rest.
set(KAST_ZONE_OUT "${CMAKE_CURRENT_BINARY_DIR}/zone_builder.run")
file(MAKE_DIRECTORY "${KAST_ZONE_OUT}")
# kast_mapbake comes from the separate services/world build; the tests that need it skip its checks if it is not built.
set(KAST_MAPBAKE "" CACHE FILEPATH "kast_mapbake.exe for the zone builder tests; empty = services/world/build/{release,debug}")
if(NOT KAST_MAPBAKE)
  foreach(preset release debug)
    if(NOT KAST_MAPBAKE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_mapbake.exe")
      set(KAST_MAPBAKE "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_mapbake.exe")
    endif()
  endforeach()
endif()

set(zone_test "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zone_builder_test.cmake")
foreach(case "meadow;samples/starter-mmo" "cavern;samples/starter-mmo" "hills;tests/project")
  list(GET case 0 map)
  list(GET case 1 project)
  add_test(NAME zone_builder_roundtrip_${map}
    COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/${project}"
            "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=roundtrip_${map} -DMODE=roundtrip -DMAP=${map} -P "${zone_test}")
endforeach()
add_test(NAME zone_builder_new_map
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=new_map -DMODE=new_map -DMAP=plains "-DMAPBAKE=${KAST_MAPBAKE}" -P "${zone_test}")
# Strokes on the corner of all 4 tiles and along a border, an import of an exported tile, a heightRange change: the
# saved heights must pass kast_mapbake's seam lint.
set(zone_png "${KAST_ZONE_OUT}/sculpt_0_0.png")
add_test(NAME zone_builder_sculpt
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=sculpt -DMODE=edit -DMAP=hilly "-DMAPBAKE=${KAST_MAPBAKE}"
          "-DARGS=--material|materials/meadow_grass.mat|--sculpt|256,256,60,30|--sculpt|256,256,20,-10|--flatten|256,120,25,12|--sculpt|400,300,40,20|--export-png|0,0,${zone_png}|--import-png|1,1,${zone_png}|--height-range|-10,60"
          "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16"
          -P "${zone_test}")
# Areas (rect, polygon, parent, portal), spawns and a patrol path, one spawn moved across a tile border: saved, linted
# by kast_mapbake and loaded by the world server.
set(KAST_WORLD_EXE "")
foreach(preset release debug)
  if(NOT KAST_WORLD_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_world.exe")
    set(KAST_WORLD_EXE "${CMAKE_CURRENT_SOURCE_DIR}/services/world/build/${preset}/kast_world.exe")
  endif()
endforeach()
add_test(NAME zone_builder_objects
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=objects -DMODE=edit -DMAP=realm "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
          "-DARGS=--material|materials/meadow_grass.mat|--sculpt|250,250,80,6|--add-area|town,1,20,20,200,200|--add-polygon|ruins,2,300,300,420,300,360,420|--area-parent|ruins,town|--area-portal|ruins,10,128,0,128,0|--add-spawn|guard_a,Guard,100,100,0,30|--add-path|guard_a,2,1,100,100,160,100,160,160|--add-spawn|bat_a,Bat,200,300,1.5,-1|--move-spawn|guard_a,300,120"
          "-DEXPECT_FILES=areas.json|.server/spawns/1_0.json|.server/spawns/0_1.json"
          -P "${zone_test}")
# Invalid edits block the save: nothing written (meadow: area cavern_gate p10 at [150,124,156,132], spawn guard_1).
foreach(case "overlap|--add-area|dup,10,140,120,160,140" "portal|--area-portal|cavern_gate,999,128,0,128,0"
             "spawn_id|--add-spawn|guard_1,Bat,100,100" "spawn_off_map|--add-spawn|far,Bat,300,100" "map_id|--map-id|11")
  string(FIND "${case}" "|" bar)
  string(SUBSTRING "${case}" 0 ${bar} name)
  math(EXPR bar "${bar} + 1")
  string(SUBSTRING "${case}" ${bar} -1 args)
  add_test(NAME zone_builder_blocked_${name}
    COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
            "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=blocked_${name} -DMODE=blocked -DMAP=meadow "-DARGS=${args}" -P "${zone_test}")
endforeach()
# Tiles: a saved map with a hill on its x = 512 edge and a spawn on tile 1_1; then 1_1 removed (its files deleted on
# save) and 2_0 added (taking the hill's edge): the result must pass kast_mapbake's seam lint.
add_test(NAME zone_builder_tiles
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=tiles -DMODE=edit -DMAP=isle "-DMAPBAKE=${KAST_MAPBAKE}"
          "-DARGS=--material|materials/meadow_grass.mat|--sculpt|505,200,40,25|--sculpt|400,400,40,10|--add-spawn|keeper,Guard,400,400|--set-spawn|200,200,1.57"
          "-DARGS2=--remove-tile|1,1|--add-tile|2,0"
          "-DEXPECT_FILES=heights/1_0.r16|heights/2_0.r16"
          "-DEXPECT_GONE=heights/1_1.r16|.server/spawns/1_1.json"
          -P "${zone_test}")
# Generate heights (seed 7), blue-to-red height colors and Create texture & material, saved: the texture and material
# written, seamless and deterministic heights, undo, re-bake after a sculpt; linted by kast_mapbake and loaded by the
# world server.
add_test(NAME zone_builder_terrain_gen
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=terrain_gen -DMODE=edit -DMAP=terra "-DMAPBAKE=${KAST_MAPBAKE}" "-DWORLD=${KAST_WORLD_EXE}"
          "-DARGS=--generate-heights|7,256,5,0.5,2|--terrain-colors|0,0,0,1,100,1,0,0|--create-terrain-material"
          "-DEXPECT_FILES=heights/0_0.r16|heights/1_0.r16|heights/0_1.r16|heights/1_1.r16|../../textures/terra_terrain.png|../../materials/terra_terrain.mat"
          -DTERRAIN_GEN=1 -P "${zone_test}")
# Bake Navmesh from the window on a copy of meadow: kast_mapbake's navtiles written, the UI drawing meanwhile.
add_test(NAME zone_builder_bake
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:kast_zone_builder> "-DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          "-DOUT_DIR=${KAST_ZONE_OUT}" -DNAME=bake -DMODE=bake -DMAP=meadow "-DMAPBAKE=${KAST_MAPBAKE}" -P "${zone_test}")
add_test(NAME zone_builder_no_focus
  COMMAND kast_focus_probe "$<TARGET_FILE:kast_zone_builder>" --project "${CMAKE_CURRENT_SOURCE_DIR}/samples/starter-mmo"
          --map meadow --frames 120
  WORKING_DIRECTORY "${KAST_ZONE_OUT}")
set_tests_properties(zone_builder_no_focus PROPERTIES TIMEOUT 120 RESOURCE_LOCK no_focus_dir)
