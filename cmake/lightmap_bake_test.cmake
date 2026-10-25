# Bake Lightmaps from the command line (kast_editor --bake-lightmaps --file-save: Window > Lighting's Bake Lightmaps,
# then File > Save) on a copy of a project (the source is never written) without the scene's baked files: the bake must
# write <scene-dir>/<scene-name>_lightmaps/ (lightmaps.json, meshes.klmm, the atlas pages; with TERRAIN_TILES, that many
# tiles under the map folder too), within MAX_SECONDS if given (the time it logs), the saved scene must name it, and a
# second run that only loads it must render the same Scene view (kast_self_check --diff-png, exactly). Then each of
# PLAY_SCENES (scenes of the copy, lit by the files just baked) is shot in Play: <OUT_DIR>/<scene>_play.png.
# Usage: cmake -DEDITOR=<kast_editor> -DSELF_CHECK=<kast_self_check> -DPROJECT=<dir> -DSCENE=<project-relative .scene>
#              -DOUT_DIR=<dir> [-DMAX_SECONDS=<s>] [-DTERRAIN_TILES=<n> -DMAP_FOLDER=<maps/name>]
#              [-DPLAY_SCENES=<a.scene,b.scene>] [-DINSTANCING_CHECK=ON] -P lightmap_bake_test.cmake
# INSTANCING_CHECK: the loaded scene renders the same with --no-instancing, in more draw calls (--log-fps): its
# lightmapped draws were instanced.
include("${CMAKE_CURRENT_LIST_DIR}/asset_refs.cmake")
set(copy "${OUT_DIR}/project")
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
file(COPY "${PROJECT}/" DESTINATION "${copy}")
get_filename_component(stem "${SCENE}" NAME_WE)
get_filename_component(dir "${SCENE}" DIRECTORY)
if(dir)
  set(dir "${dir}/")
endif()
file(REMOVE_RECURSE "${copy}/${dir}${stem}_lightmaps")
if(DEFINED MAP_FOLDER)
  file(REMOVE_RECURSE "${copy}/${MAP_FOLDER}/lightmaps")
endif()
function(run name)
  execute_process(COMMAND "${EDITOR}" --frames 40 --screenshot "${OUT_DIR}/${name}.png" --projects-file "${OUT_DIR}/projects.json"
                          --settings-file "${OUT_DIR}/editor_settings.json" --project "${copy}" ${ARGN} --size 1600x900
                          --fixed-dt 0.016 --focus-panel Assets
                  WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  message("${out}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "kast_editor (${name}) exited with ${result}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()
run(baked --scene "${copy}/${SCENE}" --bake-lightmaps --file-save)
string(REGEX MATCH "Baked lightmaps to '([^']*)' in ([0-9.]+) s: ([0-9]+) atlas pages, ([0-9]+) texels, ([0-9]+) probes, ([0-9]+) terrain tiles"
       baked "${out}")
set(sidecar "${CMAKE_MATCH_1}") # before the next MATCHES clears them
set(seconds "${CMAKE_MATCH_2}")
set(tiles "${CMAKE_MATCH_6}")
if(NOT baked OR NOT out MATCHES "Saved scene")
  message(FATAL_ERROR "no lightmaps baked, or the scene not saved")
endif()
message(STATUS "Bake time: ${seconds} s")
if(DEFINED MAX_SECONDS AND seconds GREATER MAX_SECONDS)
  message(FATAL_ERROR "the bake took ${seconds} s, over ${MAX_SECONDS} s")
endif()
if(NOT sidecar STREQUAL "${dir}${stem}_lightmaps/lightmaps.json")
  message(FATAL_ERROR "baked to '${sidecar}', not beside the scene")
endif()
foreach(file lightmaps.json meshes.klmm)
  if(NOT EXISTS "${copy}/${dir}${stem}_lightmaps/${file}")
    message(FATAL_ERROR "the bake wrote no ${file}")
  endif()
endforeach()
file(READ "${copy}/${SCENE}" saved)
kast_plain_asset_refs(saved) # the hint, whether the save wrote a ref or a plain path
if(NOT saved MATCHES "\"bakedPath\": \"${sidecar}\"")
  message(FATAL_ERROR "the saved scene does not name ${sidecar}")
endif()
if(DEFINED TERRAIN_TILES)
  file(GLOB ktx "${copy}/${MAP_FOLDER}/lightmaps/*_dir.ktx")
  list(LENGTH ktx count)
  if(NOT tiles EQUAL TERRAIN_TILES OR NOT count EQUAL TERRAIN_TILES OR NOT EXISTS "${copy}/${MAP_FOLDER}/lightmaps/lightmaps.json")
    message(FATAL_ERROR "${tiles} terrain tiles baked, ${count} written to ${MAP_FOLDER}/lightmaps, expected ${TERRAIN_TILES}")
  endif()
endif()
run(loaded --scene "${copy}/${SCENE}")
if(out MATCHES "Lightmaps '")
  message(FATAL_ERROR "the second run warned about the lightmaps")
endif()
execute_process(COMMAND "${SELF_CHECK}" --diff-png "${OUT_DIR}/baked.png" "${OUT_DIR}/loaded.png" 0 322,76,955,551
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the reloaded lightmaps render differently")
endif()
if(INSTANCING_CHECK)
  foreach(mode instanced single)
    set(flag "")
    if(mode STREQUAL "single")
      set(flag --no-instancing)
    endif()
    execute_process(COMMAND "${EDITOR}" --frames 150 --screenshot "${OUT_DIR}/${mode}.png" --projects-file "${OUT_DIR}/projects.json"
                            --settings-file "${OUT_DIR}/editor_settings.json" --project "${copy}" --scene "${copy}/${SCENE}"
                            --size 1600x900 --fixed-dt 0.016 --focus-panel Assets --log-fps ${flag}
                    WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX MATCHALL "([0-9]+) draw calls" calls "${out}")
    list(GET calls -1 last)
    string(REGEX MATCH "[0-9]+" draws_${mode} "${last}")
    message(STATUS "${mode}: ${draws_${mode}} draw calls")
  endforeach()
  if(NOT draws_instanced LESS draws_single)
    message(FATAL_ERROR "${draws_instanced} draw calls instanced, ${draws_single} without: the lightmapped cubes didn't batch")
  endif()
  execute_process(COMMAND "${SELF_CHECK}" --diff-png "${OUT_DIR}/instanced.png" "${OUT_DIR}/single.png" 0 322,76,955,551
                  RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  message("${out}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "instanced lightmapped draws render differently from single ones")
  endif()
endif()
string(REPLACE "," ";" PLAY_SCENES "${PLAY_SCENES}")
foreach(scene IN LISTS PLAY_SCENES)
  get_filename_component(name "${scene}" NAME_WE)
  run(${name}_play --scene "${copy}/${scene}" --play)
  if(out MATCHES "\\[warn\\]|\\[error\\]")
    message(FATAL_ERROR "${scene} in Play logged a warning or an error")
  endif()
endforeach()
message(STATUS "OK: baked in ${seconds} s, saved and reloaded")
