# Bake VoxelGI from the command line (kast_editor --bake-voxelgi --file-save: Window > Lighting's Bake VoxelGI, then
# File > Save) on a copy of a project (the source is never written) without the scene's baked volumes: the bake must
# write <scene-dir>/<scene-name>_voxelgi/<id>.json and <id>.kvox for every volume, the saved scene must name them, and a
# second run that only loads them must render the same Scene view (kast_self_check --diff-png, exactly): baked.png and
# loaded.png in OUT_DIR.
# Usage: cmake -DEDITOR=<kast_editor> -DSELF_CHECK=<kast_self_check> -DPROJECT=<dir> -DSCENE=<project-relative .scene>
#              -DVOLUMES=<count> -DOUT_DIR=<dir> -P voxelgi_bake_test.cmake
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
file(REMOVE_RECURSE "${copy}/${dir}${stem}_voxelgi")
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
run(baked --scene "${copy}/${SCENE}" --bake-voxelgi --file-save)
string(REGEX MATCHALL "Baked VoxelGI '[^']*' to '[^']*' in [0-9.]+ s: [0-9]+ voxels" baked "${out}")
list(LENGTH baked count)
if(NOT count EQUAL VOLUMES OR NOT out MATCHES "Saved scene")
  message(FATAL_ERROR "${count} VoxelGI volumes baked (expected ${VOLUMES}), or the scene not saved")
endif()
file(READ "${copy}/${SCENE}" saved)
kast_plain_asset_refs(saved) # the hint, whether the save wrote a ref or a plain path
foreach(line IN LISTS baked)
  string(REGEX MATCH "to '([^']*)' in ([0-9.]+) s" ignored "${line}")
  set(sidecar "${CMAKE_MATCH_1}")
  message(STATUS "Bake time: ${CMAKE_MATCH_2} s (${sidecar})")
  if(NOT sidecar MATCHES "^${dir}${stem}_voxelgi/[0-9]+\\.json$")
    message(FATAL_ERROR "baked to '${sidecar}', not beside the scene")
  endif()
  string(REGEX REPLACE "\\.json$" ".kvox" kvox "${sidecar}")
  if(NOT EXISTS "${copy}/${sidecar}" OR NOT EXISTS "${copy}/${kvox}")
    message(FATAL_ERROR "the bake wrote no ${sidecar} or ${kvox}")
  endif()
  if(NOT saved MATCHES "\"bakedPath\": \"${sidecar}\"")
    message(FATAL_ERROR "the saved scene does not name ${sidecar}")
  endif()
endforeach()
run(loaded --scene "${copy}/${SCENE}")
if(out MATCHES "VoxelGI '[^']*': '")
  message(FATAL_ERROR "the second run warned about a VoxelGI bake")
endif()
execute_process(COMMAND "${SELF_CHECK}" --diff-png "${OUT_DIR}/baked.png" "${OUT_DIR}/loaded.png" 0 322,76,955,551
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
message("${out}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the reloaded VoxelGI bake renders differently")
endif()
message(STATUS "OK: ${count} VoxelGI volumes baked, saved and reloaded")
