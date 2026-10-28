# kast_mapbake from the command line (ctest mapbake_cli): lint errors exit 1 and bake nothing; with HILLS set, a fresh
# bake of the sample hills equals its stored navtiles (maps/hills/.server/nav), so they never go stale.
# cmake -DBAKE=<kast_mapbake> -DFIXTURES=<tests/mapbake> -DOUT=<scratch dir> [-DHILLS=<maps/hills>] -P mapbake_cli.cmake
file(REMOVE_RECURSE "${OUT}")

function(expect_lint_error dir pattern)
  execute_process(COMMAND "${BAKE}" "${FIXTURES}/${dir}" --out "${OUT}/${dir}" RESULT_VARIABLE rc
                  OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 1 OR NOT out MATCHES "${pattern}" OR EXISTS "${OUT}/${dir}")
    message(FATAL_ERROR "${dir}: want exit 1, \"${pattern}\" and nothing baked; got exit ${rc}:\n${out}")
  endif()
  message(STATUS "${dir}: exit 1, ${out}")
endfunction()

expect_lint_error(bad_seam "seam 0_0 \\| 1_0: 257 samples differ, worst 1\\.53 m")
expect_lint_error(bad_spawn "spawn \"stray\": \"pos\" is outside tile 0_0")
# An instance field on a world map: the manifest is refused as the server refuses it (map::parse_manifest).
expect_lint_error(bad_instance "\"maxPlayers\" is only for instance maps")
# An unknown type and a blocking object without a shape: both reported, nothing baked.
expect_lint_error(bad_objects "objects\\[1\\]: \"type\" must be \"visual\", \"obstacle\" or \"interactable\".*objects\\[2\\]: a blocking object needs \"shape\"")

if(HILLS)
  execute_process(COMMAND "${BAKE}" "${HILLS}" --out "${OUT}/hills" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "hills: bake failed (exit ${rc}):\n${out}")
  endif()
  message(STATUS "hills:\n${out}")
  foreach(tile 0_0 1_0 0_1 1_1)
    file(SHA256 "${OUT}/hills/${tile}.navtile" fresh)
    file(SHA256 "${HILLS}/.server/nav/${tile}.navtile" stored)
    if(NOT fresh STREQUAL stored)
      message(FATAL_ERROR "hills: stored ${tile}.navtile is stale; run kast_mapbake tests/project/maps/hills")
    endif()
  endforeach()
  message(STATUS "hills: stored navtiles match a fresh bake")
endif()
