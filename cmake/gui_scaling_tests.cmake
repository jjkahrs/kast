# Game GUI resolution and DPI scaling, display modes and their Lua API (docs/engine/game-gui.html#scaling; the C++ side
# is kast_self_check --gui: checkGuiScaling, checkLuaGuiScaling, checkGuiScalingScene). tests/project/ui_scale*.scene
# show ui/scale_dp.rml (dp mode: an orange 160x80 dp box, a blue 160x80 px box, on black) and ui/scale_all.rml (all
# mode: a green 160x80 px button at 400,20 px). Every player run reads and writes its own user settings file
# (--user-settings), never the user's.
set(scale_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gui_scaling_test.cmake")
set(scale_forbid "\\[warn\\] RmlUi|\\[error\\]|Lua error")

# Built games: "scale" (ui_scale.scene), "scale_api" (the kast.gui / kast.display driver) and "scale_display" (window
# modes), each with a "gui" block: reference 640x360 (so a 1280x720 window is dp ratio 2), no OS display scale (the
# same on any monitor), a 640x360 default window.
set(scale_gui "\"gui\": {\"referenceResolution\": [640, 360], \"match\": 0.5, \"useOsDpi\": false, \"width\": 640, \"height\": 360}")
foreach(game "scale|Scale Test|ui_scale.scene" "scale_api|Scale API Test|ui_scale_api.scene"
             "scale_display|Scale Display Test|ui_scale_display.scene")
  string(REPLACE "|" ";" game "${game}")
  list(GET game 0 name)
  list(GET game 1 project_name)
  list(GET game 2 start_scene)
  kast_game_project(${name} "${project_name}" ${start_scene} FALSE)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}/project.kast"
       "{\n  \"version\": 1,\n  \"name\": \"${project_name}\",\n  \"startScene\": \"${start_scene}\",\n  ${scale_gui}\n}\n")
  add_test(NAME build_game_${name}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/${name}"
            "-DOUT=${KAST_GAME_OUT}/${name}" "-DEXE=${project_name}.exe" -DEXPECT=0 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
  set_tests_properties(build_game_${name} PROPERTIES FIXTURES_SETUP game_${name}_built)
endforeach()

# kast_scale_run(<test> <game> "<|-separated player args>" [EXPECT re] [EXPECT2 re] [FORBID re] [CHECK_FILE f]
#                [FILE_EXPECT re] [PRE_FILE f] [PRE_TEXT t] [SETUP fixture] [REQUIRES fixtures...] [SECOND_DISPLAY]):
#                the built game in game_out/<game>.<test>.run.
function(kast_scale_run test game args)
  cmake_parse_arguments(ARG "SECOND_DISPLAY" "EXPECT;EXPECT2;FORBID;CHECK_FILE;FILE_EXPECT;PRE_FILE;PRE_TEXT;SETUP" "REQUIRES" ${ARGN})
  set(names "scale|Scale Test" "scale_api|Scale API Test" "scale_display|Scale Display Test")
  foreach(entry ${names})
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 n)
    if(n STREQUAL game)
      list(GET entry 1 exe)
    endif()
  endforeach()
  set(run "-DEXE=${exe}.exe" "-DCOPY_FROM=${KAST_GAME_OUT}/${game}" "-DWORK_DIR=${KAST_GAME_OUT}/${game}.${test}.run" "-DARGS=${args}"
          "-DFORBID=${scale_forbid}")
  foreach(key EXPECT EXPECT2 CHECK_FILE FILE_EXPECT PRE_FILE PRE_TEXT)
    if(DEFINED ARG_${key})
      list(APPEND run "-D${key}=${ARG_${key}}")
    endif()
  endforeach()
  if(ARG_FORBID)
    list(REMOVE_ITEM run "-DFORBID=${scale_forbid}")
    list(APPEND run "-DFORBID=${ARG_FORBID}")
  endif()
  if(ARG_SECOND_DISPLAY)
    list(APPEND run "-DSECOND_DISPLAY=$<TARGET_FILE:kast_self_check>")
  endif()
  add_test(NAME ${test} COMMAND "${CMAKE_COMMAND}" ${run} -P "${scale_script}")
  set_tests_properties(${test} PROPERTIES FIXTURES_REQUIRED "game_${game}_built;${ARG_REQUIRES}" TIMEOUT 120
                       SKIP_REGULAR_EXPRESSION "SKIPPED:")
  if(ARG_SETUP)
    set_tests_properties(${test} PROPERTIES FIXTURES_SETUP ${ARG_SETUP})
  endif()
endfunction()

# dp ratio 1 (the project's 640x360 window, no user settings file yet) and 2 (--size 1280x720). Probes: the dp box and
# the all box cover the same part of the window at both (they scale), the px box half as much at ratio 2 (it doesn't).
set(scale_out "${CMAKE_CURRENT_BINARY_DIR}")
kast_scale_run(player_scale_ratio1 scale "--frames|60|--user-settings|settings.json|--screenshot|${scale_out}/player_scale_ratio1.png"
               EXPECT "GUI dp ratio 1 \\(640x360 pixels" EXPECT2 "ui_scale: UI scale 1, windowed 640x360" SETUP player_scale_ratio1_shot)
kast_scale_run(player_scale_ratio2 scale "--frames|60|--size|1280x720|--user-settings|settings.json|--screenshot|${scale_out}/player_scale_ratio2.png"
               EXPECT "GUI dp ratio 2 \\(1280x720 pixels" SETUP player_scale_ratio2_shot)
add_test(NAME player_scale_ratio1_probe COMMAND kast_self_check --probe-png "${scale_out}/player_scale_ratio1.png"
         0.25,0.25,255,170,0,8 0.08,0.66,32,96,255,8 0.75,0.167,32,192,64,8 0.08,0.33,0,0,0,8)
set_tests_properties(player_scale_ratio1_probe PROPERTIES FIXTURES_REQUIRED player_scale_ratio1_shot)
add_test(NAME player_scale_ratio2_probe COMMAND kast_self_check --probe-png "${scale_out}/player_scale_ratio2.png"
         0.25,0.25,255,170,0,8 0.08,0.33,32,96,255,8 0.08,0.66,0,0,0,8 0.75,0.167,32,192,64,8)
set_tests_properties(player_scale_ratio2_probe PROPERTIES FIXTURES_REQUIRED player_scale_ratio2_shot)

# A real (pushed SDL) click on the all-mode button at its scaled-up place reaches Lua; one at its unscaled place first
# doesn't.
kast_scale_run(player_scale_all_click scale "--frames|60|--size|1280x720|--user-settings|settings.json|--sim-click|15,480,60|--sim-click|25,960,120"
               EXPECT "Scale all clicked: all_box in ui/scale_all.rml" FORBID "${scale_forbid}|Scale all clicked.*Scale all clicked")

# Every kast.gui scaling and kast.display function from Lua (scripts/ui_scale_api.lua); it leaves windowed 800x600 and
# UI scale 1.5 in the user settings file, which the next runs read: over the project's window, under --size; a corrupt
# file warns and the project's defaults apply.
kast_scale_run(player_scale_api scale_api "--frames|30|--user-settings|settings.json"
               EXPECT "scale api: PASS" FORBID "${scale_forbid}|scale api FAIL"
               CHECK_FILE "${KAST_GAME_OUT}/scale_api.player_scale_api.run/settings.json"
               FILE_EXPECT "\"mode\":\"windowed\",\"width\":800,\"height\":600,\"uiScale\":1.5" SETUP player_scale_api_ran)
kast_scale_run(player_scale_settings_kept scale "--frames|30|--user-settings|${KAST_GAME_OUT}/scale_api.player_scale_api.run/settings.json"
               EXPECT "ui_scale: UI scale 1.5, windowed 800x600" REQUIRES player_scale_api_ran)
kast_scale_run(player_scale_size_wins scale "--frames|30|--size|640x360|--user-settings|${KAST_GAME_OUT}/scale_api.player_scale_api.run/settings.json"
               EXPECT "ui_scale: UI scale 1.5, windowed 640x360" REQUIRES player_scale_api_ran)
kast_scale_run(player_scale_settings_corrupt scale "--frames|30|--user-settings|settings.json"
               PRE_FILE "${KAST_GAME_OUT}/scale.player_scale_settings_corrupt.run/settings.json" PRE_TEXT "{\"mode\": oops"
               EXPECT "\\[warn\\] User settings '[^']*settings.json' are invalid" EXPECT2 "ui_scale: UI scale 1, windowed 640x360")

# Borderless, then exclusive fullscreen in the display's first mode, then windowed again (scripts/ui_scale_display.lua),
# only on a secondary display: skipped on a machine with one.
kast_scale_run(player_scale_display_modes scale_display "--frames|60|--user-settings|settings.json"
               EXPECT "scale display: PASS" EXPECT2 "Display mode: exclusive" FORBID "${scale_forbid}|scale display FAIL"
               SECOND_DISPLAY)

# Editor: the Game view scales the GUIs in Play (simulated 1280x720 at display scale 1 and 2: dp ratio 1 and 2, the
# shots differ); kast.display there only records (scripts/ui_scale_editor.lua) and nothing writes user settings.
set(scale_scene "${KAST_TEST_PROJECT}/ui_scale.scene")
foreach(ratio 1 2)
  add_test(NAME ui_scale_game_view_${ratio}x
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${scale_out}/ui_scale_game_view_${ratio}x.png"
            "-DEXTRA_ARGS=--scene ${scale_scene} --play --game-resolution 1280x720 --game-display-scale ${ratio}"
            "-DEXPECT_OUTPUT=GUI dp ratio ${ratio} \\(1280x720 pixels, display scale ${ratio}" "-DFORBID_OUTPUT=${scale_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(ui_scale_game_view_${ratio}x PROPERTIES FIXTURES_SETUP ui_scale_game_view_${ratio}x_shot)
endforeach()
add_test(NAME ui_scale_game_view_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${scale_out}/ui_scale_game_view_1x.png" "-DB=${scale_out}/ui_scale_game_view_2x.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(ui_scale_game_view_differs PROPERTIES FIXTURES_REQUIRED "ui_scale_game_view_1x_shot;ui_scale_game_view_2x_shot")
add_test(NAME ui_scale_editor_display
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${scale_out}/ui_scale_editor_display.png"
          "-DEXTRA_ARGS=--scene ${KAST_TEST_PROJECT}/ui_scale_editor.scene --play --game-resolution 1280x720 --game-display-scale 1.5"
          "-DEXPECT_OUTPUT=kast.display.setMode borderless 1280x720: recorded.*scale editor: PASS"
          "-DFORBID_OUTPUT=${scale_forbid}|scale editor FAIL|Display mode:|User settings"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")

# GUI Builder: the preview's UI scale (100% vs 200%) on the dp document doubles its dp box; the shots differ.
foreach(percent 100 200)
  math(EXPR ratio "${percent} / 100")
  add_test(NAME gui_builder_scale_${percent}
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${scale_out}/gui_builder_scale_${percent}.png"
            "-DEXTRA_ARGS=--project ${KAST_TEST_PROJECT} --open-gui ui/scale_dp.rml --size 1600x1000 --gui-preview-scale ${percent}"
            "-DEXPECT_OUTPUT=GUI Preview dp ratio ${ratio} \\(1280x720, UI scale ${percent}%\\)" "-DFORBID_OUTPUT=${scale_forbid}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
  set_tests_properties(gui_builder_scale_${percent} PROPERTIES FIXTURES_SETUP gui_builder_scale_${percent}_shot)
endforeach()
add_test(NAME gui_builder_scale_differs
  COMMAND "${CMAKE_COMMAND}" "-DA=${scale_out}/gui_builder_scale_100.png" "-DB=${scale_out}/gui_builder_scale_200.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/differs_test.cmake")
set_tests_properties(gui_builder_scale_differs PROPERTIES FIXTURES_REQUIRED "gui_builder_scale_100_shot;gui_builder_scale_200_shot")

# Inspector: the UI Document's Scaling dropdown (shot on the all-mode one); an edit of it (--set-field, the Inspector's
# undoable edit) saves "scaling": "all", and undone, the scene saves without the key again. Display & GUI Scaling
# (Build menu): the modal (shot), and its Save (--gui-settings-set) writes project.kast's "gui" block.
set(scale_edit "${CMAKE_CURRENT_BINARY_DIR}/gui_scale_project")
file(REMOVE_RECURSE "${scale_edit}")
file(MAKE_DIRECTORY "${scale_edit}/ui" "${scale_edit}/scripts")
file(WRITE "${scale_edit}/project.kast" "{\n  \"version\": 1,\n  \"name\": \"Scale Edit\",\n  \"startScene\": \"ui_scale.scene\"\n}\n")
foreach(file ui/scale_dp.rml ui/scale_dp.rcss ui/scale_all.rml ui/scale_all.rcss ui_scale.scene scripts/ui_scale.lua)
  file(COPY_FILE "${KAST_TEST_PROJECT}/${file}" "${scale_edit}/${file}")
endforeach()
add_test(NAME ui_scale_inspector_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${scale_out}/ui_scale_inspector.png"
          "-DEXTRA_ARGS=--scene ${scale_scene} --select 2" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
set(scale_editor_args "--projects-file|projects.json|--settings-file|editor_settings.json|--frames|30")
set(scale_set_field "--scene|ui_scale.scene|--set-field|1,UIDocument.scaling,\"all\"")
add_test(NAME ui_scale_inspector_set
  COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:kast_editor>" "-DCOPY_FROM=${scale_edit}"
          "-DWORK_DIR=${scale_out}/ui_scale_inspector_set.run" "-DARGS=${scale_set_field}|--file-save|${scale_editor_args}"
          "-DCHECK_FILE=${scale_out}/ui_scale_inspector_set.run/ui_scale.scene"
          "-DFILE_EXPECT=\"path\":\"ui/scale_dp.rml\",\"visible\":true,\"scaling\":\"all\"" -P "${scale_script}")
add_test(NAME ui_scale_inspector_undo
  COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:kast_editor>" "-DCOPY_FROM=${scale_edit}"
          "-DWORK_DIR=${scale_out}/ui_scale_inspector_undo.run"
          "-DARGS=${scale_set_field}|--scene-undo|1|--file-save|${scale_editor_args}"
          "-DCHECK_FILE=${scale_out}/ui_scale_inspector_undo.run/ui_scale.scene"
          "-DFILE_EXPECT=\"path\":\"ui/scale_all.rml\",\"visible\":true,\"scaling\":\"all\""
          "-DFILE_FORBID=scale_dp.rml\",\"visible\":true,\"scaling\"" -P "${scale_script}")
add_test(NAME gui_settings_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${scale_out}/gui_settings.png"
          "-DEXTRA_ARGS=--project ${CMAKE_CURRENT_BINARY_DIR}/game_projects/scale --open-gui-settings"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
add_test(NAME gui_settings_save
  COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:kast_editor>" "-DCOPY_FROM=${scale_edit}"
          "-DWORK_DIR=${scale_out}/gui_settings_save.run"
          "-DARGS=--project|.|--gui-settings-set|{\"referenceResolution\":[1920,1080],\"match\":0.25,\"mode\":\"borderless\",\"refresh\":60}|${scale_editor_args}"
          "-DEXPECT=Display & GUI Scaling saved in project.kast"
          "-DCHECK_FILE=${scale_out}/gui_settings_save.run/project.kast"
          "-DFILE_EXPECT=\"gui\":{\"referenceResolution\":\\[1920,1080\\],\"match\":0.25,\"useOsDpi\":true,\"uiScale\":1.0,\"mode\":\"borderless\",\"width\":1280,\"height\":720,\"refresh\":60}"
          -P "${scale_script}")
