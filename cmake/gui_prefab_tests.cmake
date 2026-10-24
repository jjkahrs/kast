# GUI prefabs (docs/editor/gui-builder.html#prefabs, docs/engine/game-gui.html#prefabs; the C++ side is
# kast_self_check --guiprefabs) on the tests/project/ui/prefab_* fixtures: prefab_host.rml holds two instances of
# prefab_button.guiprefab (b overrides its title), one of prefab_nested.guiprefab (holding another prefab_button) and a
# missing one; prefab_missing.rml a missing one and a hand-made cycle (prefab_cycle_a <-> prefab_cycle_b). Edits that
# save run on copies of tests/project (gui_prefab_project, gui_prefab_unpack_project), in fixture order.
set(prefab_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error")
set(prefab_builder "--size 1600x1000")
set(KAST_GUI_PREFAB_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/gui_prefab_project")
set(KAST_GUI_PREFAB_UNPACK "${CMAKE_CURRENT_BINARY_DIR}/gui_prefab_unpack_project")

# kast_gui_prefab_shot(<name> <project dir> "<editor flags>" [EXPECT regex] [FORBID regex] [MUST_EXIST path]
#                      [SETUP fixture] [REQUIRES fixtures...]): gui_builder_prefab_<name>, an editor run on that project
#                      with gui_builder_prefab_<name>.png as its screenshot.
function(kast_gui_prefab_shot name project flags)
  cmake_parse_arguments(ARG "" "EXPECT;FORBID;MUST_EXIST;SETUP" "REQUIRES" ${ARGN})
  set(args "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/gui_builder_prefab_${name}.png"
           "-DEXTRA_ARGS=--project ${project} ${prefab_builder} ${flags}")
  if(ARG_EXPECT)
    list(APPEND args "-DEXPECT_OUTPUT=${ARG_EXPECT}")
  endif()
  if(ARG_FORBID)
    list(APPEND args "-DFORBID_OUTPUT=${ARG_FORBID}")
  endif()
  if(ARG_MUST_EXIST)
    list(APPEND args "-DMUST_EXIST=${ARG_MUST_EXIST}")
  endif()
  add_test(NAME gui_builder_prefab_${name} COMMAND "${CMAKE_COMMAND}" ${args} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  if(ARG_SETUP)
    set_tests_properties(gui_builder_prefab_${name} PROPERTIES FIXTURES_SETUP ${ARG_SETUP})
  endif()
  if(ARG_REQUIRES)
    set_tests_properties(gui_builder_prefab_${name} PROPERTIES FIXTURES_REQUIRED "${ARG_REQUIRES}")
  endif()
endfunction()

# kast_gui_prefab_file(<name> <file> <regex> <fixture>): gui_builder_prefab_<name>_file, the file once fixture has run
# matches regex. It sets up <fixture>_checked: the next edit of that file requires it, so it runs after the check.
function(kast_gui_prefab_file name file regex fixture)
  add_test(NAME gui_builder_prefab_${name}_file COMMAND "${CMAKE_COMMAND}" -E cat "${file}")
  set_tests_properties(gui_builder_prefab_${name}_file PROPERTIES PASS_REGULAR_EXPRESSION "${regex}" FIXTURES_REQUIRED ${fixture}
                       FIXTURES_SETUP ${fixture}_checked)
endfunction()

foreach(copy gui_prefab_project gui_prefab_unpack_project)
  add_test(NAME ${copy}_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/${copy}")
  add_test(NAME ${copy}_copy COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${CMAKE_CURRENT_BINARY_DIR}/${copy}")
  set_tests_properties(${copy}_clean PROPERTIES FIXTURES_SETUP ${copy}_cleaned)
  set_tests_properties(${copy}_copy PROPERTIES FIXTURES_REQUIRED ${copy}_cleaned FIXTURES_SETUP ${copy}_copied)
endforeach()

# The instances in the builder (read-only on tests/project): expanded with prefixed ids, b's overridden title with its
# #ffcc00 markers (class, text), the nested instance listed and selectable, the missing one red with a warning.
kast_gui_prefab_shot(screenshot "${KAST_TEST_PROJECT}" "--open-gui ui/prefab_host.rml --gui-select b_title"
                     EXPECT "prefab 'ui/prefab_gone.guiprefab' \\(#m\\): not found; left as an empty placeholder"
                     FORBID "\\[warn\\] RmlUi|no element with that id")
kast_gui_prefab_shot(nested "${KAST_TEST_PROJECT}" "--open-gui ui/prefab_host.rml --gui-select n_inner_title"
                     FORBID "\\[warn\\] RmlUi|no element with that id")

# Create GUI Prefab from prefab_source.rml's #card (a Panel holding a Text and a Button): card.guiprefab + card.rcss,
# ids without the "card_" they shared; undone, the original element is back (the files stay). Again: card is taken, so
# card_2.guiprefab, and the element is an instance of it.
kast_gui_prefab_shot(create_undo "${KAST_GUI_PREFAB_PROJECT}"
                     "--open-gui ui/prefab_source.rml --gui-select card --gui-create-prefab ui --gui-undo 1 --gui-save"
                     EXPECT "Created GUI prefab '[^']*/ui/card.guiprefab'.*Undo Create GUI Prefab.*Saved GUI"
                     FORBID "${prefab_forbid}" MUST_EXIST "${KAST_GUI_PREFAB_PROJECT}/ui/card.rcss"
                     REQUIRES gui_prefab_project_copied SETUP gui_prefab_created_undone)
kast_gui_prefab_file(create_prefab "${KAST_GUI_PREFAB_PROJECT}/ui/card.guiprefab"
                     "^<div class=\"card\">[\r\n ]*<p id=\"title\" class=\"card-title\">Card title</p>[\r\n ]*<button id=\"button\" data-event=\"card.clicked\">Open</button>[\r\n ]*</div>"
                     gui_prefab_created_undone)
kast_gui_prefab_file(create_undo "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_source.rml"
                     "<div id=\"card\" class=\"card\">[\r\n ]*<p id=\"card_title\" class=\"card-title\">Card title</p>"
                     gui_prefab_created_undone)
kast_gui_prefab_shot(create "${KAST_GUI_PREFAB_PROJECT}" "--open-gui ui/prefab_source.rml --gui-select card --gui-create-prefab ui --gui-save"
                     EXPECT "Created GUI prefab '[^']*/ui/card_2.guiprefab' \\(and its .rcss\\); #card is an instance of it.*Saved GUI"
                     FORBID "${prefab_forbid}" REQUIRES gui_prefab_created_undone_checked SETUP gui_prefab_created)
kast_gui_prefab_file(create "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_source.rml"
                     "<div id=\"card\" data-prefab=\"ui/card_2.guiprefab\" />" gui_prefab_created)

# Place the same prefab twice into main_menu.rml's #buttons: unique ids prefab_button, prefab_button_2 (their ids inside
# prefixed), no RmlUi warning; saved as two references.
kast_gui_prefab_shot(place "${KAST_GUI_PREFAB_PROJECT}"
                     "--open-gui ui/main_menu.rml --gui-select buttons --gui-place-prefab ui/prefab_button.guiprefab --gui-place-prefab ui/prefab_button.guiprefab --gui-save"
                     EXPECT "Placed GUI prefab 'ui/prefab_button.guiprefab' as #prefab_button[\r\n].*Placed GUI prefab 'ui/prefab_button.guiprefab' as #prefab_button_2.*Saved GUI"
                     FORBID "${prefab_forbid}" REQUIRES gui_prefab_created_checked SETUP gui_prefab_placed)
kast_gui_prefab_file(place "${KAST_GUI_PREFAB_PROJECT}/ui/main_menu.rml"
                     "<div id=\"prefab_button\" data-prefab=\"ui/prefab_button.guiprefab\" />[\r\n ]*<div id=\"prefab_button_2\" data-prefab=\"ui/prefab_button.guiprefab\" />"
                     gui_prefab_placed)

# Overrides on prefab_host.rml: an attribute of a's button saved as an <override> (b's untouched); reopened, it is
# still there (the screenshot shows its #ffcc00 marker); set back to the prefab's value it is gone; set again and
# reverted, gone; a's title class set and applied: written into prefab_button.guiprefab, a and the nested one show it
# (b keeps its own class), the host keeps no override for it.
kast_gui_prefab_shot(override "${KAST_GUI_PREFAB_PROJECT}" "--open-gui ui/prefab_host.rml --gui-select a_button --gui-set-attr data-event=a.go --gui-save"
                     EXPECT "--gui-set-attr data-event=.a.go. on 'a_button'.*Saved GUI" REQUIRES gui_prefab_placed_checked SETUP gui_prefab_overridden)
kast_gui_prefab_file(override "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_host.rml"
                     "<div id=\"a\" data-prefab=\"ui/prefab_button.guiprefab\">[\r\n ]*<override target=\"button\" attribute=\"data-event\" value=\"a.go\" />[\r\n ]*</div>[\r\n ]*<div id=\"b\" data-prefab=\"ui/prefab_button.guiprefab\">[\r\n ]*<override target=\"title\" attribute=\"class\" value=\"prefab-title big\" />[\r\n ]*<override target=\"title\" text=\"Overridden\" />"
                     gui_prefab_overridden)
kast_gui_prefab_shot(override_reopen "${KAST_GUI_PREFAB_PROJECT}" "--open-gui ui/prefab_host.rml --gui-select a_button"
                     REQUIRES gui_prefab_overridden_checked SETUP gui_prefab_override_reopened)
kast_gui_prefab_shot(override_setback "${KAST_GUI_PREFAB_PROJECT}"
                     "--open-gui ui/prefab_host.rml --gui-select a_button --gui-set-attr data-event=prefab.clicked --gui-save"
                     EXPECT "Saved GUI" REQUIRES gui_prefab_override_reopened SETUP gui_prefab_set_back)
kast_gui_prefab_file(override_setback "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_host.rml"
                     "<div id=\"a\" data-prefab=\"ui/prefab_button.guiprefab\" />[\r\n ]*<div id=\"b\"" gui_prefab_set_back)
kast_gui_prefab_shot(revert "${KAST_GUI_PREFAB_PROJECT}"
                     "--open-gui ui/prefab_host.rml --gui-select a_title --gui-set-attr class=big --gui-revert class --gui-save"
                     EXPECT "--gui-revert class on 'a_title'.*Saved GUI" REQUIRES gui_prefab_set_back_checked SETUP gui_prefab_reverted)
kast_gui_prefab_file(revert "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_host.rml"
                     "<div id=\"a\" data-prefab=\"ui/prefab_button.guiprefab\" />[\r\n ]*<div id=\"b\"" gui_prefab_reverted)
kast_gui_prefab_shot(apply "${KAST_GUI_PREFAB_PROJECT}"
                     "--open-gui ui/prefab_host.rml --gui-select a_title --gui-set-attr class=big --gui-apply class --gui-save"
                     EXPECT "Applied 'class' to GUI prefab 'ui/prefab_button.guiprefab'.*--gui-apply class on 'a_title'.*Saved GUI"
                     FORBID "${prefab_forbid}" REQUIRES gui_prefab_reverted_checked SETUP gui_prefab_applied)
kast_gui_prefab_file(apply_prefab "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_button.guiprefab"
                     "<p id=\"title\" class=\"big\">Prefab Card</p>" gui_prefab_applied)
kast_gui_prefab_file(apply_host "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_host.rml"
                     "<div id=\"a\" data-prefab=\"ui/prefab_button.guiprefab\" />[\r\n ]*<div id=\"b\" data-prefab=\"ui/prefab_button.guiprefab\">[\r\n ]*<override target=\"title\" attribute=\"class\" value=\"prefab-title big\" />[\r\n ]*<override target=\"title\" text=\"Overridden\" />"
                     gui_prefab_applied)

# Unpack b: plain markup (ids as expanded) that renders exactly as the instance did (the preview before and after,
# nothing selected, compared pixel for pixel).
kast_gui_prefab_shot(unpack_before "${KAST_GUI_PREFAB_UNPACK}" "--open-gui ui/prefab_host.rml" FORBID "\\[warn\\] RmlUi"
                     REQUIRES gui_prefab_unpack_project_copied SETUP gui_prefab_unpack_shot_before)
kast_gui_prefab_shot(unpack "${KAST_GUI_PREFAB_UNPACK}" "--open-gui ui/prefab_host.rml --gui-select b --gui-unpack --gui-save"
                     EXPECT "--gui-unpack 'b'.*Saved GUI" REQUIRES gui_prefab_unpack_shot_before SETUP gui_prefab_unpacked)
kast_gui_prefab_file(unpack "${KAST_GUI_PREFAB_UNPACK}/ui/prefab_host.rml"
                     "<div id=\"b\" class=\"prefab-card\">[\r\n ]*<p id=\"b_title\" class=\"prefab-title big\">Overridden</p>[\r\n ]*<button id=\"b_button\" class=\"prefab-button\" data-event=\"prefab.clicked\">Press</button>"
                     gui_prefab_unpacked)
kast_gui_prefab_shot(unpack_after "${KAST_GUI_PREFAB_UNPACK}" "--open-gui ui/prefab_host.rml" FORBID "\\[warn\\] RmlUi"
                     REQUIRES gui_prefab_unpacked SETUP gui_prefab_unpack_shot_after)
add_test(NAME gui_builder_prefab_unpack_matches
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/gui_builder_prefab_unpack_before.png"
          "${CMAKE_CURRENT_BINARY_DIR}/gui_builder_prefab_unpack_after.png" 0 330,120,940,520)
set_tests_properties(gui_builder_prefab_unpack_matches PROPERTIES FIXTURES_REQUIRED "gui_prefab_unpack_shot_before;gui_prefab_unpack_shot_after")

# A missing prefab and a hand-made cycle: both warned about (document, path), the document still loads (the missing
# node red, its card shown), Save keeps the reference and its override.
kast_gui_prefab_shot(missing "${KAST_GUI_PREFAB_PROJECT}" "--open-gui ui/prefab_missing.rml --gui-select gone --gui-save"
                     EXPECT "GUI 'ui/prefab_missing.rml': prefab 'ui/prefab_gone.guiprefab' \\(#gone\\): not found.*GUI 'ui/prefab_missing.rml': prefab 'ui/prefab_cycle_a.guiprefab' \\(#a\\): a cycle \\(ui/prefab_cycle_a.guiprefab -> ui/prefab_cycle_b.guiprefab -> ui/prefab_cycle_a.guiprefab\\).*Saved GUI"
                     FORBID "\\[warn\\] RmlUi" REQUIRES gui_prefab_applied_checked SETUP gui_prefab_missing_saved)
kast_gui_prefab_file(missing "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_missing.rml"
                     "<div id=\"gone\" data-prefab=\"ui/prefab_gone.guiprefab\">[\r\n ]*<override target=\"title\" text=\"Kept while missing\" />[\r\n ]*</div>[\r\n ]*<div id=\"loop\" data-prefab=\"ui/prefab_cycle_a.guiprefab\" />"
                     gui_prefab_missing_saved)

# Editing a prefab: prefab_nested.guiprefab opened in the builder, its caption's class set and saved: the file keeps
# its one element and its own nested instance (as a reference, with its override); its .rcss is untouched.
kast_gui_prefab_shot(edit "${KAST_GUI_PREFAB_PROJECT}" "--open-gui ui/prefab_nested.guiprefab --gui-select caption --gui-set-attr class=big --gui-save"
                     EXPECT "--gui-set-attr class=.big. on 'caption'.*Saved GUI" FORBID "${prefab_forbid}"
                     REQUIRES gui_prefab_missing_saved_checked SETUP gui_prefab_edited)
kast_gui_prefab_file(edit "${KAST_GUI_PREFAB_PROJECT}/ui/prefab_nested.guiprefab"
                     "^<div class=\"prefab-frame\">[\r\n ]*<p id=\"caption\" class=\"big\">Nested frame</p>[\r\n ]*<div id=\"inner\" data-prefab=\"ui/prefab_button.guiprefab\">[\r\n ]*<override target=\"title\" text=\"Inner card\" />[\r\n ]*</div>[\r\n ]*</div>[\r\n ]*$"
                     gui_prefab_edited)

# A prefab that would hold itself is refused with a message: prefab_button.guiprefab open, its root picked, placing
# prefab_nested (which holds prefab_button) in it.
kast_gui_prefab_shot(cycle "${KAST_TEST_PROJECT}" "--open-gui ui/prefab_button.guiprefab --gui-pick 150,4 --gui-place-prefab ui/prefab_nested.guiprefab"
                     EXPECT "Place GUI Prefab: prefab 'ui/prefab_nested.guiprefab' can't be placed here: it would hold itself, a cycle \\(ui/prefab_button.guiprefab -> ui/prefab_nested.guiprefab -> ui/prefab_button.guiprefab\\)"
                     FORBID "\\[warn\\] RmlUi")

# An instance's elements stay as the prefab has them: delete, move, reparent and adding into it are refused (nothing
# saved: tests/project).
foreach(locked "delete|--gui-select a_title --gui-delete|the edit refused"
               "move|--gui-select a_button --gui-move up|the edit refused"
               "reparent|--gui-select a_title --gui-reparent column|the edit refused"
               "reparent_into|--gui-select spawn_label --gui-reparent a|can't move an element into itself or into a prefab instance"
               "add|--gui-select a --gui-add Text|Add refused")
  string(REPLACE "|" ";" locked "${locked}")
  list(GET locked 0 name)
  list(GET locked 1 flags)
  list(GET locked 2 refusal)
  kast_gui_prefab_shot(locked_${name} "${KAST_TEST_PROJECT}" "--open-gui ui/prefab_host.rml ${flags}"
                       EXPECT "GUI Hierarchy: ${refusal}" FORBID "\\[warn\\] RmlUi")
endforeach()

# Play (ui_prefab.scene: prefab_host.rml driven by scripts/ui_prefab.lua): kast.gui.instantiate spawns #s under #spawn
# and refuses a missing prefab, an unknown parent and a taken id (false, a warning each); --gui-click on the spawned
# button publishes its data-event. The Game view must match tests/reference/ui_prefab.png (its 955x652 pixels at 322,44
# of the 1600x1000 shot). The built game "guiprefab" ships the prefabs and their sheets in game.pak and does the same.
add_test(NAME ui_prefab_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/ui_prefab.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_prefab.scene --play --size 1600x1000 --gui-click s_button"
          "-DEXPECT_OUTPUT=prefab: INSTANTIATED true.*prefab: REFUSED missing=false parent=false taken=false.*prefab: CLICKED id=s_button document=ui/prefab_host.rml"
          "-DFORBID_OUTPUT=${prefab_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set_tests_properties(ui_prefab_screenshot PROPERTIES FIXTURES_SETUP ui_prefab_shot)
add_test(NAME ui_prefab_matches_reference
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference/ui_prefab.png"
          "${CMAKE_CURRENT_BINARY_DIR}/ui_prefab.png" 1.0 0,0,955,652,322,44)
set_tests_properties(ui_prefab_matches_reference PROPERTIES FIXTURES_REQUIRED ui_prefab_shot)
add_test(NAME game_guiprefab_pak
  COMMAND "${CMAKE_COMMAND}" "-DPAK=${KAST_GAME_OUT}/guiprefab/game.pak"
          "-DMUST_CONTAIN=ui/prefab_host.rml;ui/prefab_button.guiprefab;ui/prefab_button.rcss;ui/prefab_nested.guiprefab;ui/prefab_nested.rcss"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pak_check.cmake")
set_tests_properties(game_guiprefab_pak PROPERTIES FIXTURES_REQUIRED game_guiprefab_built)
add_test(NAME player_guiprefab_click
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/guiprefab" "-DEXE=GUI Prefab Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/guiprefab_click.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_guiprefab_click.png"
          -DEXPECT=0 "-DEXTRA_ARGS=--gui-click s_button"
          "-DEXPECT_OUTPUT=prefab: INSTANTIATED true.*prefab: CLICKED id=s_button document=ui/prefab_host.rml"
          "-DFORBID_OUTPUT=${prefab_forbid}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
set_tests_properties(player_guiprefab_click PROPERTIES FIXTURES_REQUIRED game_guiprefab_built TIMEOUT 90)
