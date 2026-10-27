# Bake Lightmaps runs in the background (editor/lightmap_job.h), on a copy of a project: lightmap_room_slow.scene
# (lightmap_room.scene at 1024 samples per texel, a few seconds) baked with --bake-lightmaps-async (as the Lighting
# panel's button: not waited for) must log its progress from at least three different frames (the editor kept drawing)
# and land its files; and with --cancel-lightmaps (its Cancel) shortly after the start it must stop, write nothing and
# leave the scene's previous bake (the committed lightmap_room_lightmaps) as it was.
# Usage: cmake -DEDITOR=<kast_editor> -DPROJECT=<dir> -DOUT_DIR=<dir> -P lightmap_job_test.cmake
set(copy "${OUT_DIR}/project")
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
file(COPY "${PROJECT}/" DESTINATION "${copy}")
file(REMOVE_RECURSE "${copy}/lightmap_room_slow_lightmaps")
file(SHA256 "${copy}/lightmap_room_lightmaps/lightmaps.json" before)
function(run name frames)
  execute_process(COMMAND "${EDITOR}" --frames ${frames} --screenshot "${OUT_DIR}/${name}.png" --projects-file "${OUT_DIR}/projects.json"
                          --settings-file "${OUT_DIR}/editor_settings.json" --project "${copy}"
                          --scene "${copy}/lightmap_room_slow.scene" --size 1600x900 ${ARGN}
                  WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  message("${out}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "kast_editor (${name}) exited with ${result}")
  endif()
  set(out "${out}" PARENT_SCOPE)
endfunction()

run(cancelled 120 --bake-lightmaps-async --cancel-lightmaps 40)
file(SHA256 "${copy}/lightmap_room_lightmaps/lightmaps.json" after)
if(NOT out MATCHES "Lightmap bake cancelled; the previous bake is kept" OR out MATCHES "Baked lightmaps to")
  message(FATAL_ERROR "Cancel didn't stop the bake")
endif()
if(EXISTS "${copy}/lightmap_room_slow_lightmaps" OR NOT before STREQUAL after)
  message(FATAL_ERROR "a cancelled bake wrote files, or changed the previous bake")
endif()

run(background 3000 --bake-lightmaps-async)
string(REGEX MATCHALL "Baking lightmaps: [0-9]+% \\(frame [0-9]+\\)" progress "${out}")
set(frames "")
foreach(line IN LISTS progress)
  string(REGEX MATCH "frame ([0-9]+)" ignored "${line}")
  list(APPEND frames "${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES frames)
list(LENGTH frames count)
if(count LESS 3)
  message(FATAL_ERROR "progress logged from ${count} frames (${frames}): the editor didn't keep drawing")
endif()
if(NOT out MATCHES "Baked lightmaps to 'lightmap_room_slow_lightmaps/lightmaps.json'" OR
   NOT EXISTS "${copy}/lightmap_room_slow_lightmaps/lightmaps.json")
  message(FATAL_ERROR "the background bake didn't land")
endif()
message(STATUS "OK: progress from frames ${frames}; Cancel kept the previous bake")
