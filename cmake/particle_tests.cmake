# GPU particles (docs/engine/particles.html), included from CMakeLists.txt: the tests/project/particles_*.scene emitters
# in Play at a fixed 1/60 s step (--fixed-dt), so the captures are the same every run, read back with
# kast_self_check --probe-png at 1280x720 window fractions (the Game view spans about x 0.20..0.80, y 0.06..0.69).
# Black backgrounds are the fog color with density 0 (the view clears to it and nothing is fogged).
set(KAST_PARTICLE_DT --fixed-dt 0.016666667)

# kast_particle_shot(<name> <scene path> [<smoke_test.cmake args>...]): particles_<name>.png of the scene in Play.
function(kast_particle_shot name scene)
  add_test(NAME particles_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/particles_${name}.png"
            "-DEXTRA_ARGS=--scene ${scene} --play ${KAST_PARTICLE_DT}" "-DFORBID_OUTPUT=\\[error\\]" ${ARGN}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(particles_${name}_screenshot PROPERTIES FIXTURES_SETUP particles_${name}_shot)
endfunction()
# kast_particle_probe(<name> <fx,fy,r,g,b,tolerance>...): particles_<name>_probe reads particles_<name>.png.
function(kast_particle_probe name)
  add_test(NAME particles_${name}_probe
    COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/particles_${name}.png" ${ARGN})
  set_tests_properties(particles_${name}_probe PROPERTIES FIXTURES_REQUIRED particles_${name}_shot)
endfunction()
# kast_particle_differs(<name> <a> <b>): particles_<a>.png and particles_<b>.png differ.
function(kast_particle_differs name a b)
  add_test(NAME particles_${name}_differs
    COMMAND "${CMAKE_COMMAND}" "-DA=${CMAKE_CURRENT_BINARY_DIR}/particles_${a}.png"
            "-DB=${CMAKE_CURRENT_BINARY_DIR}/particles_${b}.png" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
  set_tests_properties(particles_${name}_differs PROPERTIES FIXTURES_REQUIRED "particles_${a}_shot;particles_${b}_shot")
endfunction()

# Shapes: the cone beam (angle 0) is one straight column, red and thin at the bottom, yellow and 3x wider at the top
# (color and size over lifetime); the sphere a green ball around its center; the box a wide blue curtain.
set(KAST_PARTICLE_SHAPES "${KAST_TEST_PROJECT}/particles_shapes.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_PARTICLE_SHAPES}")
kast_particle_shot(shapes "${KAST_PARTICLE_SHAPES}")
kast_particle_probe(shapes 0.3664,0.4861,255,64,0,30 0.3664,0.3472,255,242,0,20 0.3594,0.3472,213,208,0,30
                    0.3609,0.4861,0,0,0,8 0.3359,0.4167,0,0,0,8 0.5000,0.3750,0,255,0,10 0.4375,0.3750,0,0,0,8
                    0.5625,0.3750,0,0,0,8 0.6055,0.4167,0,0,255,10 0.6602,0.4167,0,0,255,10 0.6328,0.5278,0,0,0,8
                    0.7031,0.4167,0,0,0,8)
# Edit mode previews the emitters in the Scene view: it differs from a copy whose emitters don't play on start.
file(READ "${KAST_PARTICLE_SHAPES}" particles_scene)
kast_plain_asset_refs(particles_scene)
string(REPLACE "\"playOnStart\": true" "\"playOnStart\": false" particles_scene "${particles_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/particles_shapes_off.scene" "${particles_scene}")
foreach(shot shapes_edit shapes_edit_off)
  set(scene "${KAST_PARTICLE_SHAPES}")
  if(shot STREQUAL "shapes_edit_off")
    set(scene "${CMAKE_CURRENT_BINARY_DIR}/particles_shapes_off.scene")
  endif()
  add_test(NAME particles_${shot}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/particles_${shot}.png"
            "-DEXTRA_ARGS=--scene ${scene} ${KAST_PARTICLE_DT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(particles_${shot}_screenshot PROPERTIES FIXTURES_SETUP particles_${shot}_shot)
endforeach()
kast_particle_differs(edit shapes_edit_off shapes_edit)
# Save -> reopen keeps every emitter field.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/particles_roundtrip")
add_test(NAME particles_roundtrip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${KAST_PARTICLE_SHAPES}"
          "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/particles_roundtrip" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip_test.cmake")
# An out-of-range field fails the scene's load (exit 1, the error names the field).
string(REPLACE "\"maxParticles\": 1000" "\"maxParticles\": 0" particles_scene "${particles_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/particles_bad.scene" "${particles_scene}")
add_test(NAME particles_bad_rejected
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCENE=${CMAKE_CURRENT_BINARY_DIR}/particles_bad.scene"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/load_fails_test.cmake")
kast_particle_probe(shapes_edit 0.5000,0.3750,0,255,0,10 0.6200,0.3900,0,0,255,10)
kast_particle_probe(shapes_edit_off 0.5000,0.3750,0,0,0,8 0.6200,0.3900,0,0,0,8)

# Blend: the same blue cluster over a red background, alpha blended (blue) and additive (magenta).
kast_particle_shot(blend "${KAST_TEST_PROJECT}/particles_blend.scene")
kast_particle_probe(blend 0.4320,0.3792,0,0,255,10 0.5664,0.3792,255,0,255,10 0.5000,0.3792,255,0,0,4
                    0.4320,0.2778,255,0,0,4)
# HDR: bloom on; one additive yellow particle at 8x (HDR) glows past its edge, the same particle at 1x doesn't.
kast_particle_shot(hdr "${KAST_TEST_PROJECT}/particles_hdr.scene")
kast_particle_probe(hdr 0.4320,0.3792,255,255,0,6 0.5648,0.3792,255,255,0,6 0.4320,0.3236,0,0,0,6
                    0.5648,0.3236,66,66,0,20 0.3906,0.3792,0,0,0,6 0.6094,0.3792,39,39,0,20)
# Texture: one big particle each: textures/checker.png (its blue corner marker top-left, then orange and cream cells),
# the procedural soft circle (black past the circle, inside the quad), and a missing texture: warned once, the circle.
kast_particle_shot(texture "${KAST_TEST_PROJECT}/particles_texture.scene"
                   "-DEXPECT_OUTPUT=Texture 'textures/missing_particle.png': cannot read the file" -DEXPECT_COUNT=1)
kast_particle_probe(texture 0.3352,0.2917,40,80,220,12 0.3492,0.2917,230,120,40,12 0.3633,0.2917,240,230,210,12
                    0.5000,0.3778,255,255,255,6 0.4609,0.3056,0,0,0,6 0.6148,0.3778,255,255,255,6 0.5758,0.3083,0,0,0,6)
# Fog: green fog (density 0.06/m): the near particle (3 m) stays nearly white; the alpha and additive ones ~15 m away
# are 53% fogged (linear 0.47 -> sRGB 182 on red and blue), as fs_mesh.sc fogs a mesh there.
kast_particle_shot(fog "${KAST_TEST_PROJECT}/particles_fog.scene")
kast_particle_probe(fog 0.5000,0.3778,251,255,251,6 0.4117,0.3778,182,255,182,8 0.5875,0.3778,182,255,182,8
                    0.2578,0.1389,0,255,0,4)
# Space: a beam on a child 2 m out from a parent spinning 180 deg/s. World space leaves a helix around the parent;
# local space (a configure-time copy) carries the particles: one short beam where the child is now.
set(KAST_PARTICLE_SPACE "${KAST_TEST_PROJECT}/particles_space.scene")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KAST_PARTICLE_SPACE}")
file(READ "${KAST_PARTICLE_SPACE}" particles_scene)
kast_plain_asset_refs(particles_scene)
string(REPLACE "\"space\": \"world\"" "\"space\": \"local\"" particles_scene "${particles_scene}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/particles_space_local.scene" "${particles_scene}")
kast_particle_shot(space_world "${KAST_PARTICLE_SPACE}")
kast_particle_shot(space_local "${CMAKE_CURRENT_BINARY_DIR}/particles_space_local.scene")
kast_particle_probe(space_world 0.4219,0.4472,248,248,248,12 0.5000,0.4861,220,220,220,20 0.5781,0.4167,0,0,0,8)
kast_particle_probe(space_local 0.5781,0.4167,255,255,255,12 0.4219,0.4472,0,0,0,8 0.5000,0.4861,0,0,0,8)
kast_particle_differs(space space_world space_local)

# The built player (game "particles" in KAST_GAMES: particles_shapes.scene, real time): the same emitters, read back at
# spots that don't depend on the exact time: the sphere's and the box's middle, the beam's lower and upper part, the
# black between them.
add_test(NAME player_particles_probe
  COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_particles.png" 0.5000,0.5000,0,255,0,16
          0.7070,0.5694,0,0,255,40 0.2906,0.6111,255,140,0,50 0.2906,0.4583,255,239,0,30 0.1562,0.1389,0,0,0,4
          0.3750,0.5556,0,0,0,8 0.6094,0.5556,0,0,0,8)
set_tests_properties(player_particles_probe PROPERTIES FIXTURES_REQUIRED player_particles_shot)

# Stress (docs/engine/particles.html#limits): 8 emitters x 12,500 = 100,000 live particles load and play without errors.
# The frame time is measured by hand: kast_editor --scene tests/scenes/particles_stress.scene --play --log-fps --no-vsync.
kast_particle_shot(stress "${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/particles_stress.scene")

# The CPU side, the JSON, the no-compute path and the GPU handle bookkeeping (Noop renderer): kast_self_check --particles.
add_test(NAME particles_self_check COMMAND kast_self_check --particles)
