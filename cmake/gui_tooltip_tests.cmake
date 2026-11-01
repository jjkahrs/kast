# Built-in game GUI tooltips (docs/engine/game-gui.html#tooltips; the C++ side is kast_self_check --guitooltips:
# checkGuiTooltips). tests/project/ui/tooltip_basic.rml: #bag ("Open bag", over its #bag_icon child too), #rich (BBCode
# with escaped markup and a malformed tag), #blank (only spaces: none) and #corner (the bottom-right corner: its
# tooltip flips left and up); tooltip_override.rml the same with a project rule on #kast-tooltip (blue card, green
# border). --gui-hover puts a synthetic mouse over an element (never the real cursor); the tooltip shows after 0.5 s of
# real time, so the runs go 180 frames (about 3 s at 60 Hz). Probes are at the pixels of each 1600x900 / 1600x1000
# shot where the box is (Game view image at 321,44, GUI Preview image scaled at 322,115).
set(tooltip_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error")
set(tooltip_orange 255,170,0,16)
set(tooltip_card 49,49,49,12)
set(tooltip_clear 43,52,64,12) # the Game view / preview background under the fixture's body

add_test(NAME gui_tooltip_self_check COMMAND kast_self_check --guitooltips)

# kast_tooltip_shot(<name> "<editor flags>" <probe>...): gui_tooltip_<name>, an editor run with gui_tooltip_<name>.png
# as its screenshot, and gui_tooltip_<name>_probe, the probes (fx,fy,r,g,b,tolerance) on it.
function(kast_tooltip_shot name flags)
  add_test(NAME gui_tooltip_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_tooltip_${name}.png"
            "-DEXTRA_ARGS=${flags}" -DFRAMES=180 "-DEXPECT_OUTPUT=--gui-hover " "-DFORBID_OUTPUT=${tooltip_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(gui_tooltip_${name} PROPERTIES FIXTURES_SETUP gui_tooltip_${name}_shot)
  add_test(NAME gui_tooltip_${name}_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/gui_tooltip_${name}.png" ${ARGN})
  set_tests_properties(gui_tooltip_${name}_probe PROPERTIES FIXTURES_REQUIRED gui_tooltip_${name}_shot)
endfunction()

# (a) Play, the default style: #bag's center is 422,108; the box's left border at 437 (the mouse + 16 dp), its card
# right of it; nothing under the mouse itself but #bag.
kast_tooltip_shot(play "--scene ${KAST_TEST_PROJECT}/ui_tooltip.scene --play --size 1600x900 --gui-hover bag"
                  0.27344,0.15611,${tooltip_orange} 0.27594,0.15611,${tooltip_card} 0.29406,0.13833,${tooltip_orange}
                  0.26406,0.12056,32,80,128,12)
# (b) Play, #corner (center 1217,603): flipped left and up, its right border at 1201 and bottom at 587, inside the area;
# right of and below that, the background and #corner.
kast_tooltip_shot(corner "--scene ${KAST_TEST_PROJECT}/ui_tooltip.scene --play --size 1600x900 --gui-hover corner"
                  0.75094,0.63611,${tooltip_orange} 0.74969,0.63611,${tooltip_card} 0.71906,0.65278,${tooltip_orange}
                  0.75156,0.63611,${tooltip_clear} 0.71906,0.65389,${tooltip_clear})
# (c) Play, the project rule on #kast-tooltip wins over the default: #rich's box green-bordered and blue.
kast_tooltip_shot(override "--scene ${KAST_TEST_PROJECT}/ui_tooltip_override.scene --play --size 1600x900 --gui-hover rich"
                  0.37344,0.15611,64,255,64,16 0.37531,0.15611,32,64,192,12 0.56281,0.15611,32,64,192,12)
# (d) The GUI Builder preview (1280x720 scaled into the panel, the GUI Hierarchy inset collapsed): #bag's tooltip.
kast_tooltip_shot(builder "--project ${KAST_TEST_PROJECT} --size 1600x1000 --open-gui ui/tooltip_basic.rml --gui-hierarchy-collapse --gui-hover bag"
                  0.25719,0.18750,${tooltip_card} 0.25781,0.19350,${tooltip_card} 0.27531,0.17850,${tooltip_card}
                  0.28156,0.23050,${tooltip_clear})

# The built player (1280x720): the mouse moved over #bag (center 100,64) on frame 10 by --sim-move; its tooltip's
# left border at 116, its card right of it.
kast_game_project(tooltip "Tooltip Test" ui_tooltip.scene FALSE)
add_test(NAME build_game_tooltip
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/tooltip"
          "-DOUT=${KAST_GAME_OUT}/tooltip" "-DEXE=Tooltip Test.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
set_tests_properties(build_game_tooltip PROPERTIES FIXTURES_SETUP game_tooltip_built)
add_test(NAME player_tooltip
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/tooltip" "-DEXE=Tooltip Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/tooltip.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_tooltip.png"
          "-DEXTRA_ARGS=--frames 180 --size 1280x720 --sim-move 10,100,64" -DEXPECT=0 "-DFORBID_OUTPUT=${tooltip_forbid}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_tooltip PROPERTIES FIXTURES_REQUIRED game_tooltip_built FIXTURES_SETUP player_tooltip_shot TIMEOUT 90)
add_test(NAME player_tooltip_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/player_tooltip.png"
         0.09102,0.13264,${tooltip_orange} 0.09414,0.13264,${tooltip_card})
set_tests_properties(player_tooltip_probe PROPERTIES FIXTURES_REQUIRED player_tooltip_shot)
