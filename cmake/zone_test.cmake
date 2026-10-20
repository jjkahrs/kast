# kast_editor's zone edit flags (editor/zone_cli.h) end to end on a copy of a project's project.kast, maps and materials
# (the whole project with SCENE; the source project is never written). Windowed runs show the map as the zone of the
# editor's scene (the scene's Terrain entity points at it).
# Usage: cmake -DEDITOR=<kast_editor> -DPROJECT=<dir> -DOUT_DIR=<dir> -DNAME=<test> -DMODE=<mode> [-DMAP=<folder>]
#              [-DMAPBAKE=<kast_mapbake>] [-DWORLD=<kast_world>] [-DARGS=<|-separated extra edits>] [-DSCENE=<file>]
#              -P zone_test.cmake
# MODE roundtrip: open MAP, save with no edits, take a screenshot; every file of the map must be byte-identical.
# MODE blocked:   open MAP, run ARGS (edits that make it invalid), save: it must refuse, and every file stays
#                 byte-identical with none added.
# MODE new_map:   create MAP (2x2 tiles), save, screenshot; it must reopen and validate, and kast_mapbake (if given and
#                 built) must lint and bake it.
# MODE edit:      create MAP (2x2 tiles), run ARGS, save, screenshot; then as new_map. EXPECT_FILES: map-relative files
#                 that must exist afterwards. With WORLD (if built), the world server must load every map of the copy,
#                 MAP among them ("maps loaded"; it then stops at the missing database). ARGS2: a second run on the saved
#                 MAP with these edits, then save (before the checks); EXPECT_GONE: map-relative files it must delete.
#                 TERRAIN_GEN: also terrain_gen_checks (below).
# MODE bake:      open MAP and Bake Navmesh in the window (MAPBAKE); it must exit 0, write .server/nav/*.navtile and
#                 have drawn frames while kast_mapbake ran. HEADLESS: with --headless instead (no window, no screenshot).
#                 Skipped (passes) without kast_mapbake.
# MODE file_save: open SCENE (project-relative) and MAP, run ARGS (edits that make the zone invalid and the scene dirty),
#                 then File > Save (--file-save, as Ctrl+S): the zone's files stay byte-identical with the problems in
#                 the Console, and the scene is still written (it must then contain EXPECT_SCENE).
string(REPLACE "|" ";" ARGS "${ARGS}") # | in the test command: no ; escaping through add_test
string(REPLACE "|" ";" EXPECT_FILES "${EXPECT_FILES}")
string(REPLACE "|" ";" ARGS2 "${ARGS2}")
string(REPLACE "|" ";" EXPECT_GONE "${EXPECT_GONE}")
set(copy "${OUT_DIR}/${NAME}")
set(shot "${OUT_DIR}/${NAME}.png")
set(work "${OUT_DIR}/${NAME}.run") # the editor's working directory: a fresh imgui.ini, recent list and settings
file(REMOVE_RECURSE "${copy}" "${work}")
file(REMOVE "${shot}")
file(MAKE_DIRECTORY "${work}")
if(SCENE)
  file(COPY "${PROJECT}/" DESTINATION "${copy}")
else()
  file(COPY "${PROJECT}/maps" "${PROJECT}/project.kast" DESTINATION "${copy}")
endif()
if(EXISTS "${PROJECT}/materials")
  file(COPY "${PROJECT}/materials" DESTINATION "${copy}")
endif()

function(run)
  execute_process(COMMAND "${EDITOR}" --project "${copy}" --no-recycle --projects-file "${work}/projects.json"
                          --settings-file "${work}/editor_settings.json" ${ARGN}
                  WORKING_DIRECTORY "${work}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "kast_editor ${ARGN} exited with ${result}:\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

# TERRAIN_GEN (MODE edit, ARGS generating seed 7 + Create): the generated texture and material, the heights' seams,
# determinism, undo, and the texture re-baked after a sculpt.
function(terrain_gen_checks)
  set(dir "${copy}/maps/${MAP}")
  set(png "${copy}/textures/${MAP}_terrain.png")
  file(READ "${png}" ihdr OFFSET 16 LIMIT 8 HEX)
  if(NOT ihdr STREQUAL "0000020000000200")
    message(FATAL_ERROR "${png}: expected 512 x 512, IHDR size ${ihdr}")
  endif()
  file(READ "${dir}/map.json" manifest)
  string(JSON uv GET "${manifest}" terrainUv)
  string(JSON material GET "${manifest}" material)
  string(JSON stops LENGTH "${manifest}" terrainColors)
  if(NOT uv STREQUAL "map" OR NOT material STREQUAL "materials/${MAP}_terrain.mat" OR stops LESS 2)
    message(FATAL_ERROR "map.json: terrainUv ${uv}, material ${material}, ${stops} terrainColors")
  endif()
  file(READ "${copy}/materials/${MAP}_terrain.mat" mat)
  string(JSON texture GET "${mat}" baseColorTexture)
  if(NOT texture STREQUAL "textures/${MAP}_terrain.png")
    message(FATAL_ERROR "${MAP}_terrain.mat: baseColorTexture ${texture}")
  endif()
  # Seams: 0_0's last row is 0_1's first; 0_0's last column is 1_0's first (spot-checked every 16 rows).
  file(READ "${dir}/heights/0_0.r16" a OFFSET 131584 LIMIT 514 HEX) # row 256: 256 * 257 * 2
  file(READ "${dir}/heights/0_1.r16" b OFFSET 0 LIMIT 514 HEX)
  if(NOT a STREQUAL b)
    message(FATAL_ERROR "seam between tiles 0_0 and 0_1")
  endif()
  foreach(v RANGE 0 256 16)
    math(EXPR at "(${v} * 257 + 256) * 2")
    math(EXPR bt "${v} * 257 * 2")
    file(READ "${dir}/heights/0_0.r16" a OFFSET ${at} LIMIT 2 HEX)
    file(READ "${dir}/heights/1_0.r16" b OFFSET ${bt} LIMIT 2 HEX)
    if(NOT a STREQUAL b)
      message(FATAL_ERROR "seam between tiles 0_0 and 1_0 at row ${v}: ${a} vs ${b}")
    endif()
  endforeach()
  # The same seed on another map with the same tiles: the same bytes; another seed: different.
  run(--new-map "${MAP}_same,2,2" --generate-heights 7 --save-zone --headless)
  run(--new-map "${MAP}_other,2,2" --generate-heights 8 --save-zone --headless)
  foreach(t 0_0 1_0 0_1 1_1)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/heights/${t}.r16" "${copy}/maps/${MAP}_same/heights/${t}.r16"
                    RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "seed 7 twice: heights/${t}.r16 differ")
    endif()
  endforeach()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/heights/0_0.r16" "${copy}/maps/${MAP}_other/heights/0_0.r16"
                  RESULT_VARIABLE differ)
  if(differ EQUAL 0)
    message(FATAL_ERROR "seeds 7 and 8 gave the same heights")
  endif()
  # Generate then undo: nothing to save, the heights stay byte-identical.
  file(SHA256 "${dir}/heights/0_0.r16" before)
  file(SHA256 "${png}" pngBefore)
  run(--map "${MAP}" --generate-heights 99 --undo --save-zone --headless)
  file(SHA256 "${dir}/heights/0_0.r16" after)
  if(NOT before STREQUAL after)
    message(FATAL_ERROR "--generate-heights then --undo changed heights/0_0.r16")
  endif()
  # A sculpt re-bakes the texture on save.
  run(--map "${MAP}" --sculpt 300,200,40,20 --save-zone --headless)
  file(SHA256 "${png}" pngAfter)
  if(pngBefore STREQUAL pngAfter)
    message(FATAL_ERROR "sculpt + save did not re-bake ${png}")
  endif()
  message(STATUS "terrain gen: 512 x 512 texture, seams, determinism, undo and re-bake OK")
endfunction()

if(MODE STREQUAL "roundtrip" OR MODE STREQUAL "blocked" OR MODE STREQUAL "file_save")
  if(MODE STREQUAL "blocked")
    run(--map "${MAP}" ${ARGS} --expect-save-fail --frames 20 --screenshot "${shot}")
  elseif(MODE STREQUAL "file_save")
    file(READ "${copy}/${SCENE}" sceneBefore)
    run(--scene "${copy}/${SCENE}" --map "${MAP}" ${ARGS} --file-save --focus-panel Zone --zone-section Problems --frames 30
        --screenshot "${shot}")
    if(NOT out MATCHES "Zone: save of maps/${MAP} blocked: [1-9][0-9]* problem")
      message(FATAL_ERROR "the blocked zone save is not in the Console:\n${out}")
    endif()
    file(READ "${copy}/${SCENE}" sceneAfter)
    if(sceneAfter STREQUAL sceneBefore OR NOT sceneAfter MATCHES "${EXPECT_SCENE}")
      message(FATAL_ERROR "${SCENE} was not saved with ${EXPECT_SCENE}:\n${sceneAfter}")
    endif()
  else()
    run(--map "${MAP}" ${ARGS} --save-zone --frames 20 --screenshot "${shot}")
  endif()
  file(GLOB_RECURSE files RELATIVE "${PROJECT}/maps/${MAP}" "${PROJECT}/maps/${MAP}/*")
  file(GLOB_RECURSE after RELATIVE "${copy}/maps/${MAP}" "${copy}/maps/${MAP}/*")
  if(NOT files STREQUAL after)
    message(FATAL_ERROR "maps/${MAP} files changed: ${files} -> ${after}")
  endif()
  foreach(f ${files})
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${PROJECT}/maps/${MAP}/${f}" "${copy}/maps/${MAP}/${f}"
                    RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "maps/${MAP}/${f} changed after a save with no edits (or a blocked one)")
    endif()
  endforeach()
  message(STATUS "OK: maps/${MAP}: ${files} unchanged")
elseif(MODE STREQUAL "new_map" OR MODE STREQUAL "edit")
  run(--new-map "${MAP},2,2" ${ARGS} --save-zone --frames 20 --screenshot "${shot}")
  if(ARGS2)
    run(--map "${MAP}" ${ARGS2} --save-zone --frames 20 --screenshot "${shot}")
  endif()
  run(--map "${MAP}" --validate --headless)
  foreach(f ${EXPECT_GONE})
    if(EXISTS "${copy}/maps/${MAP}/${f}")
      message(FATAL_ERROR "maps/${MAP}/${f} should have been deleted")
    endif()
  endforeach()
  foreach(f map.json ${EXPECT_FILES})
    if(NOT EXISTS "${copy}/maps/${MAP}/${f}")
      message(FATAL_ERROR "maps/${MAP}/${f} was not written")
    endif()
  endforeach()
  if(MAPBAKE AND EXISTS "${MAPBAKE}")
    execute_process(COMMAND "${MAPBAKE}" "${copy}/maps/${MAP}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "kast_mapbake maps/${MAP} exited with ${result}:\n${out}")
    endif()
    message(STATUS "kast_mapbake: ${out}")
  else()
    message(STATUS "kast_mapbake not built (${MAPBAKE}): lint skipped")
  endif()
  if(WORLD AND EXISTS "${WORLD}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "MAP_ROOT=${copy}/maps" START_MAP=10 AUTH_JWKS_URL=http://127.0.0.1:9/jwks
                            DB_HOST=127.0.0.1 DB_PORT=9 DB_USER=x DB_PASSWORD=x DB_NAME=x REALM_ID=1 DB_CONNECT_TIMEOUT_SECONDS=1
                            "LUA_ROOT=${copy}/no_lua" "${WORLD}"
                    OUTPUT_VARIABLE out ERROR_VARIABLE out TIMEOUT 60)
    string(REGEX MATCH "[^
]*maps loaded[^
]*" loadedLine "${out}")
    if(NOT loadedLine MATCHES "\"name\":\"${MAP}\"")
      message(FATAL_ERROR "kast_world did not load maps/${MAP}:
${out}")
    endif()
    message(STATUS "kast_world: ${loadedLine}")
  else()
    message(STATUS "kast_world not built (${WORLD}): server load skipped")
  endif()
  if(TERRAIN_GEN)
    terrain_gen_checks()
  endif()
elseif(MODE STREQUAL "bake")
  if(NOT MAPBAKE OR NOT EXISTS "${MAPBAKE}")
    message(STATUS "kast_mapbake not built (${MAPBAKE}): skipped")
    return()
  endif()
  file(REMOVE_RECURSE "${copy}/maps/${MAP}/.server/nav") # the copy's own navtiles would pass the check below
  if(HEADLESS)
    run(--map "${MAP}" --mapbake "${MAPBAKE}" --bake --headless)
  else()
    run(--map "${MAP}" --mapbake "${MAPBAKE}" --bake --focus-panel Zone --zone-section Bake --frames 20 --screenshot "${shot}")
  endif()
  file(GLOB navtiles "${copy}/maps/${MAP}/.server/nav/*.navtile")
  if(HEADLESS)
    if(NOT navtiles OR NOT out MATCHES "Zone: Bake finished: exit code 0")
      message(FATAL_ERROR "--headless --bake: no navtiles written or no exit code logged:\n${out}")
    endif()
    message(STATUS "OK: ${NAME}: ${navtiles}")
    return()
  endif()
  if(NOT navtiles)
    message(FATAL_ERROR "no navtiles written:\n${out}")
  endif()
  string(REGEX MATCH "([0-9]+) frames drawn while kast_mapbake ran" drawn "${out}")
  if(NOT drawn OR CMAKE_MATCH_1 LESS 2)
    message(FATAL_ERROR "the UI did not keep drawing while kast_mapbake ran:\n${out}")
  endif()
  message(STATUS "${drawn}; ${navtiles}")
else()
  message(FATAL_ERROR "unknown MODE ${MODE}")
endif()
if(NOT EXISTS "${shot}")
  message(FATAL_ERROR "no screenshot ${shot}")
endif()
message(STATUS "OK: ${NAME}")
