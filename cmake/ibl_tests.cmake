# Image-based lighting and reflection probes (docs/engine/ibl-reflection-probes.html), included from CMakeLists.txt.
# Scenes in tests/project: ibl_sky.scene (chrome, rough metal, plastic and matte spheres under a procedural sky, ambient
# intensity 1, no lights), ibl_sky_cubemap.scene (the same under the sky_test cube faces), ibl_shader.scene (.kshader
# light() and toon materials and a toon shader graph beside the built-in material), orm_split.scene (a cube whose
# occlusion and metallic-roughness are two images), reflection_probe.scene (a lit room with a probe baked into
# reflection_probe_probes/, chrome and rough spheres in its box, a chrome sphere outside), reflection_probe_priority.scene
# (the room's probe and an overlapping higher-priority one of intensity 0 over its right half) and
# reflection_probe_missing.scene (bakedPath names a file that isn't there). Configure-time copies in the shadow project
# change one thing each. All shots: 1600x900, fixed dt, the Assets tab over the Console (so the log doesn't show).
set(KAST_IBL_SKY "${KAST_TEST_PROJECT}/ibl_sky.scene")
set(KAST_IBL_SHADER "${KAST_TEST_PROJECT}/ibl_shader.scene")
set(KAST_IBL_CUBEMAP "${KAST_TEST_PROJECT}/ibl_sky_cubemap.scene")
set(KAST_PROBE "${KAST_TEST_PROJECT}/reflection_probe.scene")
set(KAST_PROBE_PRIORITY "${KAST_TEST_PROJECT}/reflection_probe_priority.scene")
set(KAST_PROBE_MISSING "${KAST_TEST_PROJECT}/reflection_probe_missing.scene")
set(KAST_ORM_SPLIT "${KAST_TEST_PROJECT}/orm_split.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_IBL_SKY}" "${KAST_IBL_SHADER}" "${KAST_IBL_CUBEMAP}"
             "${KAST_PROBE}" "${KAST_PROBE_PRIORITY}" "${KAST_PROBE_MISSING}" "${KAST_ORM_SPLIT}")
file(READ "${KAST_IBL_SKY}" ibl_sky_scene)
file(READ "${KAST_IBL_SHADER}" ibl_shader_scene)
file(READ "${KAST_IBL_CUBEMAP}" ibl_cubemap_scene)
file(READ "${KAST_PROBE}" probe_scene)
file(READ "${KAST_PROBE_PRIORITY}" probe_priority_scene)
file(READ "${KAST_PROBE_MISSING}" probe_missing_scene)
file(READ "${KAST_ORM_SPLIT}" orm_split_scene)
set(sky_mode "\"mode\": \"(procedural|cubemap)\"")
kast_shadow_variant(ibl_sky_off "${ibl_sky_scene}" "${sky_mode}" "\"mode\": \"off\"")
kast_shadow_variant(ibl_sky_ambient0 "${ibl_sky_scene}" "\"ambientIntensity\": 1" "\"ambientIntensity\": 0")
kast_shadow_variant(ibl_sky_tinted "${ibl_sky_scene}" "\"ambientColor\": \\[1, 1, 1\\]" "\"ambientColor\": [1, 0.3, 0.3]")
kast_shadow_variant(ibl_sky_cubemap_off "${ibl_cubemap_scene}" "${sky_mode}" "\"mode\": \"off\"")
kast_shadow_variant(ibl_shader_off "${ibl_shader_scene}" "${sky_mode}" "\"mode\": \"off\"")
# Recolored: the sky as scripts/ibl_sky_set.lua sets it from Lua at 0.2 s; set from Lua it must light the same.
string(REPLACE "\"top\": [0.1, 0.3, 0.9]" "\"top\": [0.9, 0.2, 0.1]" ibl_sky_recolored "${ibl_sky_scene}")
kast_shadow_variant(ibl_sky_recolored "${ibl_sky_recolored}" "\"bottom\": \\[0.35, 0.22, 0.08\\]" "\"bottom\": [0.1, 0.6, 0.2]")
kast_shadow_variant(ibl_sky_lua "${ibl_sky_scene}" "\"systems\": \\[\\]" "\"systems\": [\"scripts/ibl_sky_set.lua\"]")
# The probe scenes without their probes (a missing probe must light as none), the priorities swapped, and the room probe
# unbaked, with a corrupt file, and baked at another resolution.
set(probe_entity ",[\r\n]+    {[\r\n]+      \"id\": 10,[^{]*{[\r\n]+[^\r\n]*[\r\n]+[^\r\n]*ReflectionProbe[^\r\n]*[\r\n]+      }[\r\n]+    }")
kast_shadow_variant(reflection_probe_none "${probe_scene}" "${probe_entity}" "")
kast_shadow_variant(reflection_probe_missing_none "${probe_missing_scene}" "${probe_entity}" "")
kast_shadow_variant(reflection_probe_priority_swapped "${probe_priority_scene}" "\"priority\": 1," "\"priority\": -1,")
kast_shadow_variant(reflection_probe_unbaked "${probe_scene}" "\"bakedPath\": \"[^\"]*\"" "\"bakedPath\": \"\"")
kast_shadow_variant(reflection_probe_corrupt "${probe_scene}" "reflection_probe_probes/10" "reflection_probe_corrupt_probes/10")
file(WRITE "${KAST_SHADOW_PROJECT}/reflection_probe_corrupt_probes/10.ktx" "not a KTX")
file(COPY_FILE "${KAST_TEST_PROJECT}/reflection_probe_probes/10.json" "${KAST_SHADOW_PROJECT}/reflection_probe_corrupt_probes/10.json")
kast_shadow_variant(reflection_probe_mismatch "${probe_scene}" "\"resolution\": 64" "\"resolution\": 128")
kast_shadow_variant(orm_packed "${orm_split_scene}" "materials/orm_split.mat" "materials/mf_orm.mat")
foreach(scene ibl_sky ibl_shader ibl_sky_cubemap reflection_probe reflection_probe_priority reflection_probe_missing
              orm_split)
  file(COPY_FILE "${KAST_TEST_PROJECT}/${scene}.scene" "${KAST_SHADOW_PROJECT}/${scene}.scene")
endforeach()

# kast_ibl_shot(<shot> <scene> <editor flags> [PLAY] [smoke_test args...]): ibl_<shot>.png of the shadow project's
# <scene>.scene.
function(kast_ibl_shot name scene flags)
  set(play "")
  if(PLAY IN_LIST ARGN)
    list(REMOVE_ITEM ARGN PLAY)
    set(play " --play")
  endif()
  add_test(NAME ibl_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ibl_${name}.png"
            "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/${scene}.scene --size 1600x900 --fixed-dt 0.016${play} ${flags}"
            -DFOCUS_PANEL=Assets ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(ibl_${name}_screenshot PROPERTIES FIXTURES_SETUP ibl_${name}_shot)
endfunction()
# kast_ibl_compare(<test> <a> <b> <same: ON / OFF>): ibl_<a>.png against ibl_<b>.png, the whole window.
function(kast_ibl_compare test a b same)
  add_test(NAME ${test}
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/ibl_${a}.png" "-DB=${CMAKE_CURRENT_BINARY_DIR}/ibl_${b}.png"
            -DSAME=${same} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "ibl_${a}_shot;ibl_${b}_shot")
endfunction()

set(no_errors "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
foreach(shot ibl_sky ibl_sky_off ibl_sky_ambient0 ibl_sky_tinted ibl_sky_cubemap ibl_sky_cubemap_off ibl_shader
             ibl_shader_off reflection_probe reflection_probe_none reflection_probe_priority
             reflection_probe_priority_swapped orm_split orm_packed)
  kast_ibl_shot(${shot} ${shot} "" "${no_errors}")
endforeach()
foreach(shot ibl_sky ibl_sky_off reflection_probe reflection_probe_none ibl_sky_recolored)
  kast_ibl_shot(${shot}_play ${shot} "" PLAY "${no_errors}")
endforeach()
kast_ibl_shot(ibl_sky_lua_play ibl_sky_lua "" PLAY "-DEXPECT_OUTPUT=ibl_sky_set.lua: sky recolored" -DEXPECT_COUNT=1)
kast_ibl_shot(reflection_probe_gizmo reflection_probe "--select 10" "${no_errors}")
# Each way a probe can't be used warns once, naming it, and lights its meshes as outside it: the missing one exactly as
# the scene without it.
set(probe_warning "Reflection probe 'Room Probe': ")
kast_ibl_shot(reflection_probe_missing reflection_probe_missing ""
  "-DEXPECT_OUTPUT=${probe_warning}'reflection_probe_missing_probes/10.ktx': cannot read the file. meshes in its box are lit as outside it"
  -DEXPECT_COUNT=1)
kast_ibl_shot(reflection_probe_missing_none reflection_probe_missing_none "" "${no_errors}")
kast_ibl_shot(reflection_probe_unbaked reflection_probe_unbaked "" "-DEXPECT_OUTPUT=${probe_warning}not baked" -DEXPECT_COUNT=1)
kast_ibl_shot(reflection_probe_corrupt reflection_probe_corrupt ""
  "-DEXPECT_OUTPUT=${probe_warning}'reflection_probe_corrupt_probes/10.ktx': not a valid KTX or DDS file" -DEXPECT_COUNT=1)
kast_ibl_shot(reflection_probe_mismatch reflection_probe_mismatch ""
  "-DEXPECT_OUTPUT=${probe_warning}'reflection_probe_probes/10.ktx': baked at 64, but its resolution is 128" -DEXPECT_COUNT=1)

# The sky lights the meshes: off, ambient 0 and a tint each change the shot; the cubemap and custom shaders' too.
foreach(pair "ibl_sky|ibl_sky_off" "ibl_sky|ibl_sky_ambient0" "ibl_sky|ibl_sky_tinted" "ibl_sky_cubemap|ibl_sky_cubemap_off"
             "ibl_sky_cubemap|ibl_sky" "ibl_shader|ibl_shader_off" "ibl_sky_play|ibl_sky_off_play"
             "reflection_probe|reflection_probe_none" "reflection_probe_play|reflection_probe_none_play"
             "reflection_probe_priority|reflection_probe_priority_swapped" "reflection_probe|reflection_probe_gizmo"
             "ibl_sky_play|ibl_sky_recolored_play")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  kast_ibl_compare(ibl_${a}_vs_${b}_differs ${a} ${b} OFF)
endforeach()
# ...and these must not change it: a sky recolored from Lua lights as one saved with those colors; a missing probe as
# none; occlusion and metallic-roughness as two images as one ORM image of the same pixels.
foreach(pair "ibl_sky_lua_play|ibl_sky_recolored_play" "reflection_probe_missing|reflection_probe_missing_none"
             "orm_split|orm_packed")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 a)
  list(GET pair 1 b)
  add_test(NAME ibl_${a}_matches_${b}
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/ibl_${a}.png" "${CMAKE_CURRENT_BINARY_DIR}/ibl_${b}.png" 0
            322,44,955,583)
  set_tests_properties(ibl_${a}_matches_${b} PROPERTIES FIXTURES_REQUIRED "ibl_${a}_shot;ibl_${b}_shot")
endforeach()

# Round trips, and a copy breaking each ReflectionProbe rule must fail to load, naming it.
foreach(scene ibl_sky reflection_probe reflection_probe_priority)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip")
  add_test(NAME ${scene}_roundtrip
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_SHADOW_PROJECT}/${scene}.scene"
            "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${scene}_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
endforeach()
foreach(bad "box_size|\"boxSize\": \\[8.4, 4.4, 8.4\\]|\"boxSize\": [8.4, 0, 8.4]|.boxSize. must be > 0 on every axis"
            "resolution|\"resolution\": 64|\"resolution\": 100|.resolution. must be 64, 128 or 256"
            "intensity|\"intensity\": 1,|\"intensity\": -1,|.intensity. must be >= 0")
  string(REPLACE "|" ";" bad "${bad}")
  list(GET bad 0 field)
  list(GET bad 1 regex)
  list(GET bad 2 replacement)
  list(GET bad 3 message)
  kast_shadow_variant(reflection_probe_bad_${field} "${probe_scene}" "${regex}" "${replacement}")
  add_test(NAME reflection_probe_bad_${field}_rejected
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>"
            "-DSCENE=${KAST_SHADOW_PROJECT}/reflection_probe_bad_${field}.scene" "-DEXPECT_OUTPUT=${message}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
endforeach()

# The editor: GameObject > Reflection Probe, the probe's Inspector (status, Bake), the Lighting panel's Bake All.
add_test(NAME ibl_gameobject_menu_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ibl_gameobject_menu.png"
          "-DEXTRA_ARGS=--scene ${KAST_SHADOW_PROJECT}/reflection_probe.scene --open-menu GameObject"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
foreach(shot "inspector|reflection_probe|--select 10" "inspector_missing|reflection_probe_missing|--select 10"
             "inspector_unbaked|reflection_probe_unbaked|--select 10")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 scene)
  list(GET shot 2 flags)
  add_test(NAME ibl_probe_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ibl_probe_${name}.png"
            "-DEXTRA_ARGS=--project ${KAST_SHADOW_PROJECT} --scene ${KAST_SHADOW_PROJECT}/${scene}.scene --size 1600x900 ${flags}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endforeach()
add_test(NAME ibl_lighting_panel_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ibl_lighting_panel.png"
          "-DEXTRA_ARGS=--project ${KAST_SHADOW_PROJECT} --scene ${KAST_SHADOW_PROJECT}/reflection_probe.scene --size 1600x900"
          -DFOCUS_PANEL=Lighting -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")

# Bake from the command line (--bake-probes, as Bake All Probes; --file-save) on a project copy without the baked files:
# both probes' .ktx and .json are written, the scene names them, and a second run that only loads them renders the
# same Scene view.
add_test(NAME reflection_probe_bake
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSELF_CHECK=$<TARGET_FILE:kast_self_check>"
          "-DPROJECT=${KAST_TEST_PROJECT}" -DSCENE=reflection_probe_priority.scene
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/reflection_probe_bake" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/probe_bake_test.cmake")

# The player: games built from reflection_probe.scene (the baked files ship in game.pak as they are) and orm_split.scene
# (the packed ORM image cooked by Build) render as the editor's Play does: their 957x584 screenshots match its Game view.
foreach(game "probes|Probes Test|reflection_probe|reflection_probe_play" "orm|ORM Test|orm_split|orm_split_play")
  string(REPLACE "|" ";" game "${game}")
  list(GET game 0 name)
  list(GET game 1 project_name)
  list(GET game 2 scene)
  list(GET game 3 editor_shot)
  kast_game_project(${name} "${project_name}" ${scene}.scene FALSE "")
  add_test(NAME build_game_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}"
            "-DOUT=${KAST_GAME_OUT}/${name}" "-DEXE=${project_name}.exe" -DEXPECT=0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
  add_test(NAME player_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/${name}" "-DEXE=${project_name}.exe"
            "-DWORK_DIR=${KAST_GAME_OUT}/${name}.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png"
            "-DEXTRA_ARGS=--fixed-dt 0.016 --size 957x584" -DEXPECT=0 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
  add_test(NAME player_${name}_matches_editor
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/ibl_${editor_shot}.png"
            "${CMAKE_CURRENT_BINARY_DIR}/player_${name}.png" 0.00392 322,44,955,583,1,0)
  set_tests_properties(build_game_${name} PROPERTIES FIXTURES_SETUP game_${name}_built)
  set_tests_properties(player_${name}_screenshot PROPERTIES FIXTURES_REQUIRED game_${name}_built
                       FIXTURES_SETUP player_${name}_shot TIMEOUT 90)
  set_tests_properties(player_${name}_matches_editor PROPERTIES FIXTURES_REQUIRED "player_${name}_shot;ibl_${editor_shot}_shot")
endforeach()
kast_ibl_shot(orm_split_play orm_split "" PLAY "${no_errors}")
add_test(NAME game_probes_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/probes/game.pak"
          "-DMUST_CONTAIN=reflection_probe_probes/10.ktx;reflection_probe_probes/10.json"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_probes_pak PROPERTIES FIXTURES_REQUIRED game_probes_built)
