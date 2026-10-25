# Screen-space effects (SSAO, SSR, depth of field, volumetric fog; post_process.h, volumetric_fog.h), included from CMakeLists.txt after the
# post-processing tests, whose post_processing.scene, its project copy KAST_POST_PROJECT and shots these reuse.
# - post_processing_ss_disabled_same: the four new "postProcessing" blocks written out, all disabled, must render as the
#   scene without them.
# - post_processing_gdata_same / _play_same: SSAO and SSR on at intensity 0 and volumetric fog at density 0: the scene
#   view draws its opaque meshes into the G-data attachments and its blended ones after the screen-space views, which
#   must change nothing (Scene view and Play's Game view).
# - gdata_<scene>_<n>: --show-gdata n (1 view-space normal + roughness, 2 indirect diffuse, 3 indirect specular) with the
#   post toggle off, over scenes covering every fs_mesh variant: static, skinned (shadows), instanced (materials),
#   terrain, lightmap, VoxelGI, SDFGI, IBL, .kshader (shader_showcase), shader graph and the sky, glass and particles
#   (none of which may show). gdata_differs: the normal picture is not the scene's.
set(ss_scene "${post_scene}")
foreach(block ssao ssr depthOfField volumetricFog)
  string(JSON ss_scene SET "${ss_scene}" postProcessing ${block} "{\"enabled\": false}")
endforeach()
file(WRITE "${KAST_POST_PROJECT}/post_processing_ss_disabled.scene" "${ss_scene}")
set(ss_scene "${post_scene}")
string(JSON ss_scene SET "${ss_scene}" postProcessing ssao "{\"enabled\": true, \"intensity\": 0}")
string(JSON ss_scene SET "${ss_scene}" postProcessing ssr "{\"enabled\": true, \"intensity\": 0}")
string(JSON ss_scene SET "${ss_scene}" postProcessing volumetricFog "{\"enabled\": true, \"density\": 0}")
file(WRITE "${KAST_POST_PROJECT}/post_processing_gdata.scene" "${ss_scene}")

foreach(name ss_disabled gdata)
  add_test(NAME post_processing_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/post_processing_${name}.scene --size 1600x900"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(post_processing_${name}_screenshot PROPERTIES FIXTURES_SETUP post_processing_${name}_shot)
  add_test(NAME post_processing_${name}_same
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/post_processing_${name}.png"
            "-DB=${CMAKE_CURRENT_BINARY_DIR}/post_processing.png" -DSAME=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(post_processing_${name}_same PROPERTIES
                       FIXTURES_REQUIRED "post_processing_shot;post_processing_${name}_shot")
endforeach()
# Play's shot shows the Console, which logs the project's font path: the reference is a copy in the same project.
file(WRITE "${KAST_POST_PROJECT}/post_processing_ref.scene" "${post_scene}")
foreach(name ref gdata)
  add_test(NAME post_processing_${name}_play_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/post_processing_${name}_play.png"
            "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/post_processing_${name}.scene --size 1600x900 --play"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(post_processing_${name}_play_screenshot PROPERTIES FIXTURES_SETUP post_processing_${name}_play_shot)
endforeach()
add_test(NAME post_processing_gdata_play_same
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/post_processing_gdata_play.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/post_processing_ref_play.png" -DSAME=ON
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(post_processing_gdata_play_same PROPERTIES
                     FIXTURES_REQUIRED "post_processing_ref_play_shot;post_processing_gdata_play_shot")

foreach(scene post_processing materials shadows terrain lightmap_room voxelgi_room sdfgi_outdoor ibl_sky shader_showcase
              shader_graph_demo particles_fog)
  foreach(attachment 1 2 3)
    add_test(NAME gdata_${scene}_${attachment}_screenshot
      COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
              "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gdata_${scene}_${attachment}.png"
              "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016 --no-scene-post --show-gdata ${attachment}"
              "-DFORBID_OUTPUT=\\[error\\]" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  endforeach()
endforeach()
set_tests_properties(gdata_post_processing_1_screenshot PROPERTIES FIXTURES_SETUP gdata_post_processing_1_shot)
add_test(NAME gdata_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/gdata_post_processing_1.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/post_processing_toggled_off.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(gdata_differs PROPERTIES FIXTURES_REQUIRED "gdata_post_processing_1_shot;post_processing_toggled_off_shot")

# SSAO (post_process.h): tests/project/ssao.scene (a wall corner, a cube, a sphere and stacked boxes on the ground, sun
# shadows, strong ambient light, SSAO on). Copies in KAST_POST_PROJECT change one thing each: SSAO off, intensity 0 (must
# match off), radius / slices / power (must differ), no ambient light with SSAO on and off (must match: AO darkens only
# the indirect diffuse, never direct light), Lua's kast.ssao.set (scripts/ssao_set.lua; its Play shot must match the
# scene saved with the values it sets) and an out-of-range sliceCount (must fail to load). The post toggle off must
# match SSAO off (both toggled off); Play differs on and off; the player (built games "ssao", "ssao_off") likewise.
file(READ "${KAST_TEST_PROJECT}/ssao.scene" ssao_scene)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/ssao.scene")
set(ssao_lua_systems "\"systems\": [\"scripts/ssao_set.lua\"],\n  \"editor\":")
foreach(variant "off|\"enabled\": true, \"radius\"|\"enabled\": false, \"radius\""
                "zero|\"intensity\": 1, \"power\"|\"intensity\": 0, \"power\""
                "radius|\"radius\": 1,|\"radius\": 2,"
                "slices|\"sliceCount\": 2|\"sliceCount\": 4"
                "power|\"power\": 1.5|\"power\": 3"
                "no_ambient|\"ambientIntensity\": 0.6|\"ambientIntensity\": 0"
                "bad|\"sliceCount\": 2|\"sliceCount\": 9"
                "lua|\"editor\":|${ssao_lua_systems}")
  string(REPLACE "|" ";" variant "${variant}")
  list(GET variant 0 name)
  list(GET variant 1 from)
  list(GET variant 2 to)
  string(REPLACE "${from}" "${to}" text "${ssao_scene}")
  file(WRITE "${KAST_POST_PROJECT}/ssao_${name}.scene" "${text}")
  set(ssao_${name}_scene "${text}")
endforeach()
string(REPLACE "\"enabled\": true, \"radius\"" "\"enabled\": false, \"radius\"" text "${ssao_no_ambient_scene}")
file(WRITE "${KAST_POST_PROJECT}/ssao_no_ambient_off.scene" "${text}")
string(REPLACE "\"radius\": 1, \"intensity\": 1," "\"radius\": 2, \"intensity\": 2," text "${ssao_lua_scene}")
file(WRITE "${KAST_POST_PROJECT}/ssao_lua_ref.scene" "${text}")
file(COPY_FILE "${KAST_POST_PROJECT}/ssao_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/ssao_off/ssao_off.scene")

# kast_ss_shot(<effect> <shot> <scene> <editor flags> [smoke_test args...]): <effect>_<shot>.png of
# KAST_POST_PROJECT/<scene>.scene (the Scene view; with --play the Game view).
function(kast_ss_shot effect name scene flags)
  add_test(NAME ${effect}_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/${effect}_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016 ${flags}"
            "-DFORBID_OUTPUT=\\[error\\]|Lua error" ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(${effect}_${name}_screenshot PROPERTIES FIXTURES_SETUP ${effect}_${name}_shot)
endfunction()
# kast_ss_compare(<effect> <test> <shot a> <shot b> SAME|DIFFERS)
function(kast_ss_compare effect test a b mode)
  set(same OFF)
  if(mode STREQUAL "SAME")
    set(same ON)
  endif()
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/${effect}_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/${effect}_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "${effect}_${a}_shot;${effect}_${b}_shot")
endfunction()
foreach(name ssao ssao_off ssao_zero ssao_radius ssao_slices ssao_power ssao_no_ambient ssao_no_ambient_off)
  string(REGEX REPLACE "^ssao_?" "" shot "${name}")
  if(shot STREQUAL "")
    set(shot on)
  endif()
  kast_ss_shot(ssao ${shot} ${name} "")
endforeach()
kast_ss_shot(ssao toggled_off ssao --no-scene-post)
kast_ss_shot(ssao off_toggled_off ssao_off --no-scene-post) # the toolbar's Post checkbox shows in the shot
kast_ss_shot(ssao play ssao --play)
kast_ss_shot(ssao off_play ssao_off --play)
kast_ss_shot(ssao lua_play ssao_lua --play "-DEXPECT_OUTPUT=ssao_set.lua: ssao set" -DEXPECT_COUNT=1)
kast_ss_shot(ssao lua_ref_play ssao_lua_ref --play)
kast_ss_compare(ssao ssao_differs on off DIFFERS)
kast_ss_compare(ssao ssao_zero_same zero off SAME)
kast_ss_compare(ssao ssao_radius_differs radius on DIFFERS)
kast_ss_compare(ssao ssao_slices_differs slices on DIFFERS)
kast_ss_compare(ssao ssao_power_differs power on DIFFERS)
kast_ss_compare(ssao ssao_no_ambient_same no_ambient no_ambient_off SAME)
kast_ss_compare(ssao ssao_toggle_same toggled_off off_toggled_off SAME)
kast_ss_compare(ssao ssao_play_differs play off_play DIFFERS)
kast_ss_compare(ssao ssao_lua_same lua_play lua_ref_play SAME)
add_test(NAME ssao_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ssao_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/ssao.scene --size 1600x900 --scroll-post-section SSAO"
          "-DFOCUS_PANEL=Post Processing"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ssao_roundtrip")
add_test(NAME ssao_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/ssao.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/ssao_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME ssao_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/ssao_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME player_ssao_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_ssao.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_ssao_off.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_ssao_differs PROPERTIES FIXTURES_REQUIRED "player_ssao_shot;player_ssao_off_shot")

# SSR (post_process.h): tests/project/ssr.scene (a smooth and a rough metal floor side by side, colored cubes and spheres
# on them, a pillar behind, procedural sky IBL as the fallback, SSR on). Copies in KAST_POST_PROJECT change one thing
# each: SSR off, intensity 0 (must match off), maxRoughness 0.2 (the rough floor keeps its IBL), maxDistance 2 and 8
# steps (must differ), the floors alone with SSR on and off (must match: rays that hit nothing leave the IBL specular,
# no seam or added light), Lua's kast.ssr.set (scripts/ssr_set.lua; its Play shot must match the scene saved with the
# values it sets) and steps 200 (must fail to load). The post toggle off must match SSR off (both toggled off); Play
# differs on and off; the player (built games "ssr", "ssr_off") likewise.
file(READ "${KAST_TEST_PROJECT}/ssr.scene" ssr_scene)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/ssr.scene")
set(ssr_lua_systems "\"systems\": [\"scripts/ssr_set.lua\"],")
foreach(variant "off|\"enabled\": true, \"maxDistance\"|\"enabled\": false, \"maxDistance\""
                "zero|\"intensity\": 1, \"steps\"|\"intensity\": 0, \"steps\""
                "rough_cut|\"maxRoughness\": 0.6|\"maxRoughness\": 0.2"
                "distance|\"maxDistance\": 50|\"maxDistance\": 2"
                "steps|\"steps\": 32|\"steps\": 8"
                "bad|\"steps\": 32|\"steps\": 200"
                "lua|\"systems\": [],|${ssr_lua_systems}")
  string(REPLACE "|" ";" variant "${variant}")
  list(GET variant 0 name)
  list(GET variant 1 from)
  list(GET variant 2 to)
  string(REPLACE "${from}" "${to}" text "${ssr_scene}")
  file(WRITE "${KAST_POST_PROJECT}/ssr_${name}.scene" "${text}")
  set(ssr_${name}_scene "${text}")
endforeach()
string(REPLACE "\"maxRoughness\": 0.6, \"intensity\": 1," "\"maxRoughness\": 0.25, \"intensity\": 0.5," text
       "${ssr_lua_scene}")
file(WRITE "${KAST_POST_PROJECT}/ssr_lua_ref.scene" "${text}")
# The floors alone (Sun, floors, camera): every ray misses.
foreach(name ssr ssr_off)
  string(JSON sun GET "${${name}_scene}" entities 0)
  string(JSON smooth GET "${${name}_scene}" entities 1)
  string(JSON rough GET "${${name}_scene}" entities 2)
  string(JSON camera GET "${${name}_scene}" entities 8)
  string(JSON text SET "${${name}_scene}" entities "[${sun}, ${smooth}, ${rough}, ${camera}]")
  string(REPLACE "ssr" "ssr_floors" floors "${name}")
  file(WRITE "${KAST_POST_PROJECT}/${floors}.scene" "${text}")
endforeach()
file(COPY_FILE "${KAST_POST_PROJECT}/ssr_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/ssr_off/ssr_off.scene")

foreach(name ssr ssr_off ssr_zero ssr_rough_cut ssr_distance ssr_steps ssr_floors ssr_floors_off)
  string(REGEX REPLACE "^ssr_?" "" shot "${name}")
  if(shot STREQUAL "")
    set(shot on)
  endif()
  kast_ss_shot(ssr ${shot} ${name} "")
endforeach()
kast_ss_shot(ssr toggled_off ssr --no-scene-post)
kast_ss_shot(ssr off_toggled_off ssr_off --no-scene-post)
kast_ss_shot(ssr play ssr --play)
kast_ss_shot(ssr off_play ssr_off --play)
kast_ss_shot(ssr lua_play ssr_lua --play "-DEXPECT_OUTPUT=ssr_set.lua: ssr set" -DEXPECT_COUNT=1)
kast_ss_shot(ssr lua_ref_play ssr_lua_ref --play)
kast_ss_compare(ssr ssr_differs on off DIFFERS)
kast_ss_compare(ssr ssr_zero_same zero off SAME)
kast_ss_compare(ssr ssr_rough_cut_differs rough_cut on DIFFERS)
kast_ss_compare(ssr ssr_distance_differs distance on DIFFERS)
kast_ss_compare(ssr ssr_steps_differs steps on DIFFERS)
kast_ss_compare(ssr ssr_floors_same floors floors_off SAME)
kast_ss_compare(ssr ssr_toggle_same toggled_off off_toggled_off SAME)
kast_ss_compare(ssr ssr_play_differs play off_play DIFFERS)
kast_ss_compare(ssr ssr_lua_same lua_play lua_ref_play SAME)
add_test(NAME ssr_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ssr_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/ssr.scene --size 1600x900 --scroll-post-section SSR"
          "-DFOCUS_PANEL=Post Processing" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ssr_roundtrip")
add_test(NAME ssr_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/ssr.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/ssr_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME ssr_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/ssr_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME player_ssr_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_ssr.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_ssr_off.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_ssr_differs PROPERTIES FIXTURES_REQUIRED "player_ssr_shot;player_ssr_off_shot")

# Depth of field (post_process.h): tests/project/dof.scene (rows at 2.5, 6, 14 m and emissive highlights at 30 m before
# a dark wall, focus 6 m at f/1.4, 85 mm, DoF on). Copies in KAST_POST_PROJECT change one thing each: DoF off, f/8,
# focus 14 m, 35 mm (must differ), Lua's kast.dof.set (scripts/dof_set.lua, a focus pull; its Play shot must match the scene
# saved with the values it sets) and fStop 0.2 (must fail to load). The post toggle off must match DoF off (both
# toggled off); Play differs on and off; the player (built games "dof", "dof_off") likewise.
file(READ "${KAST_TEST_PROJECT}/dof.scene" dof_scene)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/dof.scene")
set(dof_lua_systems "\"systems\": [\"scripts/dof_set.lua\"],")
foreach(variant "off|\"enabled\": true, \"focusDistance\"|\"enabled\": false, \"focusDistance\""
                "fstop|\"fStop\": 1.4|\"fStop\": 8"
                "focus|\"focusDistance\": 6,|\"focusDistance\": 14,"
                "focal|\"focalLength\": 85|\"focalLength\": 35"
                "bad|\"fStop\": 1.4|\"fStop\": 0.2"
                "lua|\"systems\": [],|${dof_lua_systems}")
  string(REPLACE "|" ";" variant "${variant}")
  list(GET variant 0 name)
  list(GET variant 1 from)
  list(GET variant 2 to)
  string(REPLACE "${from}" "${to}" text "${dof_scene}")
  file(WRITE "${KAST_POST_PROJECT}/dof_${name}.scene" "${text}")
  set(dof_${name}_scene "${text}")
endforeach()
string(REPLACE "\"focusDistance\": 6, \"fStop\": 1.4," "\"focusDistance\": 14, \"fStop\": 2," text "${dof_lua_scene}")
file(WRITE "${KAST_POST_PROJECT}/dof_lua_ref.scene" "${text}")
file(COPY_FILE "${KAST_POST_PROJECT}/dof_off.scene" "${CMAKE_CURRENT_BINARY_DIR}/game_projects/dof_off/dof_off.scene")

foreach(name dof dof_off dof_fstop dof_focus dof_focal)
  string(REGEX REPLACE "^dof_?" "" shot "${name}")
  if(shot STREQUAL "")
    set(shot on)
  endif()
  kast_ss_shot(dof ${shot} ${name} "")
endforeach()
kast_ss_shot(dof toggled_off dof --no-scene-post)
kast_ss_shot(dof off_toggled_off dof_off --no-scene-post)
kast_ss_shot(dof play dof --play)
kast_ss_shot(dof off_play dof_off --play)
kast_ss_shot(dof lua_play dof_lua --play "-DEXPECT_OUTPUT=dof_set.lua: dof set" -DEXPECT_COUNT=1)
kast_ss_shot(dof lua_ref_play dof_lua_ref --play)
kast_ss_compare(dof dof_differs on off DIFFERS)
kast_ss_compare(dof dof_fstop_differs fstop on DIFFERS)
kast_ss_compare(dof dof_focus_differs focus on DIFFERS)
kast_ss_compare(dof dof_focal_differs focal on DIFFERS)
kast_ss_compare(dof dof_toggle_same toggled_off off_toggled_off SAME)
kast_ss_compare(dof dof_play_differs play off_play DIFFERS)
kast_ss_compare(dof dof_lua_same lua_play lua_ref_play SAME)
add_test(NAME dof_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/dof_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/dof.scene --size 1600x900 --scroll-post-section Depth"
          "-DFOCUS_PANEL=Post Processing" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dof_roundtrip")
add_test(NAME dof_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/dof.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/dof_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
add_test(NAME dof_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/dof_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
add_test(NAME player_dof_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_dof.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_dof_off.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_dof_differs PROPERTIES FIXTURES_REQUIRED "player_dof_shot;player_dof_off_shot")

# Volumetric fog (volumetric_fog.h): tests/project/volumetric_fog.scene (a hall whose front wall's gaps let the low sun
# in as shafts, a shadowed point light in a Box FogVolume and a shadowed spot light in a Sphere one, each with an
# occluder, a glass sphere, a smoke particle, volumetric fog on). Copies in KAST_POST_PROJECT change one thing each:
# volumetric fog off; density 0 with the FogVolumes at 0 (must match off: nothing runs); the FogVolumes at 0, anisotropy
# -0.6 (must differ); the exp^2 distance fog added (must differ from both volumetric fog alone and the distance fog
# alone: they combine); Lua's kast.volumetricFog.set (scripts/volfog_set.lua; its Play shot must match the scene saved
# with the values it sets); anisotropy 0.95 and a FogVolume edgeFalloff 1.5 (must fail to load). The post toggle off must
# match off (both toggled off); Play differs on and off; the player (built games "vfog", "vfog_off") likewise; the
# camera sweeping sideways in Play (scripts/volfog_camera.lua: ghosting, by eye). Each FogVolume selected shows its
# Inspector and outline; one saved as a prefab and dropped back keeps its fields.
file(READ "${KAST_TEST_PROJECT}/volumetric_fog.scene" vfog_scene)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_TEST_PROJECT}/volumetric_fog.scene")
set(vfog_box "\"shape\": \"box\", \"density\": 0.5,")
set(vfog_sphere "\"shape\": \"sphere\", \"density\": 0.6,")
set(vfog_exp2 "\"postProcessing\": {\n    \"fog\": { \"enabled\": true, \"color\": [0.35, 0.4, 0.5], \"density\": 0.04, \"start\": 4 },")
foreach(variant "off|\"enabled\": true, \"density\": 0.06|\"enabled\": false, \"density\": 0.06"
                "zero|\"density\": 0.06, \"albedo\"|\"density\": 0, \"albedo\""
                "no_volumes|${vfog_box}|${vfog_box}"
                "anisotropy|\"anisotropy\": 0.6|\"anisotropy\": -0.6"
                "combined|\"postProcessing\": {|${vfog_exp2}"
                "bad|\"anisotropy\": 0.6|\"anisotropy\": 0.95"
                "bad_volume|\"edgeFalloff\": 0.3|\"edgeFalloff\": 1.5"
                "lua|\"systems\": [],|\"systems\": [\"scripts/volfog_set.lua\"],"
                "motion|\"systems\": [],|\"systems\": [\"scripts/volfog_camera.lua\"],")
  string(REPLACE "|" ";" variant "${variant}")
  list(GET variant 0 name)
  list(GET variant 1 from)
  list(GET variant 2 to)
  string(REPLACE "${from}" "${to}" text "${vfog_scene}")
  set(vfog_${name}_scene "${text}")
endforeach()
# zero and no_volumes: both FogVolumes at density 0; fog_only: the distance fog without volumetric fog.
foreach(name zero no_volumes)
  string(REPLACE "${vfog_box}" "\"shape\": \"box\", \"density\": 0," text "${vfog_${name}_scene}")
  string(REPLACE "${vfog_sphere}" "\"shape\": \"sphere\", \"density\": 0," vfog_${name}_scene "${text}")
endforeach()
string(REPLACE "\"enabled\": true, \"density\": 0.06" "\"enabled\": false, \"density\": 0.06" vfog_fog_only_scene
       "${vfog_combined_scene}")
string(REPLACE "\"density\": 0.06, \"albedo\": [0.9, 0.9, 0.9], \"anisotropy\": 0.6"
       "\"density\": 0.1, \"albedo\": [0.9, 0.9, 0.9], \"anisotropy\": 0.3" text "${vfog_lua_scene}")
string(REPLACE "\"temporalBlend\": 0.9" "\"temporalBlend\": 0" vfog_lua_ref_scene "${text}")
foreach(name off zero no_volumes anisotropy combined fog_only bad bad_volume lua lua_ref motion)
  file(WRITE "${KAST_POST_PROJECT}/volumetric_fog_${name}.scene" "${vfog_${name}_scene}")
endforeach()
file(COPY_FILE "${KAST_POST_PROJECT}/volumetric_fog_off.scene"
     "${CMAKE_CURRENT_BINARY_DIR}/game_projects/vfog_off/volumetric_fog_off.scene")

foreach(name volumetric_fog volumetric_fog_off volumetric_fog_zero volumetric_fog_no_volumes volumetric_fog_anisotropy
             volumetric_fog_combined volumetric_fog_fog_only)
  string(REGEX REPLACE "^volumetric_fog_?" "" shot "${name}")
  if(shot STREQUAL "")
    set(shot on)
  endif()
  kast_ss_shot(vfog ${shot} ${name} "")
endforeach()
kast_ss_shot(vfog toggled_off volumetric_fog --no-scene-post)
kast_ss_shot(vfog off_toggled_off volumetric_fog_off --no-scene-post)
kast_ss_shot(vfog play volumetric_fog --play)
kast_ss_shot(vfog off_play volumetric_fog_off --play)
kast_ss_shot(vfog lua_play volumetric_fog_lua --play "-DEXPECT_OUTPUT=volfog_set.lua: volumetric fog set" -DEXPECT_COUNT=1)
kast_ss_shot(vfog lua_ref_play volumetric_fog_lua_ref --play)
kast_ss_shot(vfog motion_play volumetric_fog_motion --play)
kast_ss_shot(vfog box_selected volumetric_fog "--select 15")
kast_ss_shot(vfog sphere_selected volumetric_fog "--select 16")
kast_ss_compare(vfog vfog_differs on off DIFFERS)
kast_ss_compare(vfog vfog_zero_same zero off SAME)
kast_ss_compare(vfog vfog_volumes_differs no_volumes on DIFFERS)
kast_ss_compare(vfog vfog_anisotropy_differs anisotropy on DIFFERS)
kast_ss_compare(vfog vfog_combined_differs combined on DIFFERS)
kast_ss_compare(vfog vfog_combined_fog_differs combined fog_only DIFFERS)
kast_ss_compare(vfog vfog_toggle_same toggled_off off_toggled_off SAME)
kast_ss_compare(vfog vfog_play_differs play off_play DIFFERS)
kast_ss_compare(vfog vfog_lua_same lua_play lua_ref_play SAME)
kast_ss_compare(vfog vfog_motion_differs motion_play play DIFFERS)
kast_ss_compare(vfog vfog_box_selected_differs box_selected on DIFFERS)
kast_ss_compare(vfog vfog_sphere_selected_differs sphere_selected on DIFFERS)
add_test(NAME vfog_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/vfog_panel.png"
          "-DEXTRA_ARGS=--scene ${KAST_POST_PROJECT}/volumetric_fog.scene --size 1600x900 --scroll-post-section Volumetric"
          "-DFOCUS_PANEL=Post Processing" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vfog_roundtrip")
add_test(NAME vfog_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/volumetric_fog.scene"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/vfog_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
foreach(name bad bad_volume)
  add_test(NAME vfog_${name}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_POST_PROJECT}/volumetric_fog_${name}.scene"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()
# prefab_vfog: FogSphere (16) saved as a prefab, then an instance dropped back: the .prefab holds its FogVolume, the
# scene both instances.
kast_prefab_test(vfog "--select 16 --create-prefab prefabs --drop-prefab prefabs/FogSphere.prefab"
  -DSCENE=volumetric_fog.scene
  "-DEXPECT=prefabs/FogSphere.prefab|\"FogVolume\":{\"shape\":\"sphere\",\"density\":0.6,\"albedo\":\\[0.8,1.0,0.85\\],\"edgeFalloff\":0.6,\"heightFalloff\":0.3}\;out.scene|\"name\":\"FogSphere\",\"parent\":null,\"prefab\":\"prefabs/FogSphere.prefab\".*\"name\":\"FogSphere\",\"parent\":null,\"prefab\":\"prefabs/FogSphere.prefab\"")
add_test(NAME player_vfog_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/player_vfog.png"
          "-DB=${CMAKE_CURRENT_BINARY_DIR}/player_vfog_off.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(player_vfog_differs PROPERTIES FIXTURES_REQUIRED "player_vfog_shot;player_vfog_off_shot")
