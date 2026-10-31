# Shader Graph panels (docs/editor/shader-graph.html), included from CMakeLists.txt: --open-graph and the --graph-* flags
# run the panels' edits on tests/project/shaders/graph_demo.kshadergraph (node ids in that file), screenshots of the
# canvas and its prompts; edits that write go to a fresh copy of the project. They run in shader_test_project (a copy
# made at configure time): an open graph compiles (the Shader Preview), and its cache must not land in tests/project,
# which shader_cache_first copies expecting none. The edit API itself is checked by kast_self_check --shaders
# (checkShaderGraphEdits).
set(KAST_GRAPH_ARGS --project ${KAST_SHADER_TEST_PROJECT} --size 1600x900 --open-graph shaders/graph_demo.kshadergraph)

# kast_graph_shot(<name> <editor args> [<smoke_test.cmake args>...]): shader_graph_<name>.png.
function(kast_graph_shot name args)
  add_test(NAME shader_graph_${name}_screenshot
    COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/shader_graph_${name}.png"
            "-DEXTRA_ARGS=${args}" -DFRAMES=60 ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.cmake")
endfunction()

# The canvas: typed, colored pins, the "Scrolling UV" comment frame; opening changes nothing (no "*").
kast_graph_shot(canvas "${KAST_GRAPH_ARGS}" "-DEXPECT_OUTPUT=Opened shader graph" "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]")
# A texture into a number input is refused: the red link and why on the canvas, the warning in the log.
kast_graph_shot(refused "${KAST_GRAPH_ARGS} --graph-link 7.out,6.b"
  "-DEXPECT_OUTPUT=--graph-link 7.out,6.b refused: a texture2D can't go into a number input")
# The add menu from the Float's output: only nodes with an input that takes it (no Input or Constant nodes).
kast_graph_shot(add_menu "${KAST_GRAPH_ARGS} --graph-add-menu 11.out" "-DFORBID_OUTPUT=\\[error\\]")
# Copy / paste of Time, Param speed and Multiply: fresh ids 12..14 with their two links, and a new comment frame.
kast_graph_shot(paste "${KAST_GRAPH_ARGS} --graph-select 3,4,5 --graph-paste 120,340 --graph-comment Pasted,100,300,440,190"
  "-DEXPECT_OUTPUT=--graph-paste 120,340: nodes 12,13,14" "-DFORBID_OUTPUT=\\[error\\]")
# Undo / redo on the graph's own history.
kast_graph_shot(undo "${KAST_GRAPH_ARGS} --graph-add multiply,560,330 --graph-link 11.out,12.a --graph-undo 2 --graph-redo 1"
  "-DEXPECT_OUTPUT=Undo Link.*Undo Add Multiply.*Redo Add Multiply" "-DFORBID_OUTPUT=\\[error\\]")
# Opening another graph while dirty asks Save / Discard / Cancel (the title has the "*").
kast_graph_shot(open_prompt "${KAST_GRAPH_ARGS} --graph-add multiply,560,330 --open-graph shaders/empty.kshadergraph"
  "-DFORBID_OUTPUT=Opened shader graph '[^']*empty")

# On a fresh copy: Assets > Create > Shader Graph writes the starter, which opens; --graph-save writes an edit.
set(KAST_GRAPH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_project")
add_test(NAME shader_graph_project_copy
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KAST_TEST_PROJECT}" "${KAST_GRAPH_PROJECT}")
add_test(NAME shader_graph_project_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KAST_GRAPH_PROJECT}")
set_tests_properties(shader_graph_project_clean PROPERTIES FIXTURES_SETUP shader_graph_clean)
set_tests_properties(shader_graph_project_copy PROPERTIES FIXTURES_REQUIRED shader_graph_clean FIXTURES_SETUP shader_graph_copy)
kast_graph_shot(create "--project ${KAST_GRAPH_PROJECT} --size 1600x900 --assets-create shadergraph:shaders"
  "-DFOCUS_PANEL=Assets" "-DMUST_EXIST=${KAST_GRAPH_PROJECT}/shaders/new_shader_graph.kshadergraph"
  "-DEXPECT_OUTPUT=Created shader graph 'shaders/new_shader_graph.kshadergraph'")
kast_graph_shot(starter "--project ${KAST_GRAPH_PROJECT} --size 1600x900 --open-graph shaders/new_shader_graph.kshadergraph"
  "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]")
kast_graph_shot(save "--project ${KAST_GRAPH_PROJECT} --size 1600x900 --open-graph shaders/graph_demo.kshadergraph --graph-add one_minus,560,330 --graph-link 11.out,12.x --graph-save"
  "-DEXPECT_OUTPUT=Saved shader graph")
add_test(NAME shader_graph_saved_file COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_GRAPH_PROJECT}/shaders/graph_demo.kshadergraph")
set_tests_properties(shader_graph_saved_file PROPERTIES PASS_REGULAR_EXPRESSION
  "\"id\": 12,[^}]*\"type\": \"one_minus\".*11,[ \n]*\"out\"[ \n]*\\],[ \n]*\"to\": \\[[ \n]*12,[ \n]*\"x\"")
set_tests_properties(shader_graph_create_screenshot PROPERTIES FIXTURES_REQUIRED shader_graph_copy FIXTURES_SETUP shader_graph_created)
set_tests_properties(shader_graph_starter_screenshot PROPERTIES FIXTURES_REQUIRED shader_graph_created)
set_tests_properties(shader_graph_save_screenshot PROPERTIES FIXTURES_REQUIRED shader_graph_copy FIXTURES_SETUP shader_graph_saved)
set_tests_properties(shader_graph_saved_file PROPERTIES FIXTURES_REQUIRED shader_graph_saved)

# A graph changed on disk while open: reloaded when clean; while dirty, Reload / Keep mine is asked (screenshot).
add_test(NAME shader_graph_reload
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_graph_reload_test.ps1"
          -Editor "$<TARGET_FILE:kast_editor>" -Project "${KAST_TEST_PROJECT}" -Out "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_reload")
set_tests_properties(shader_graph_reload PROPERTIES PASS_REGULAR_EXPRESSION "\nOK" TIMEOUT 300)

# Phase 4: the Shader Preview, live update, errors on nodes, the Inspectors. Edits are never saved: the project's graph
# file stays as it is.
# - shader_graph_update: on shader_graph_demo.scene, a red Vector3 into the Fragment Output's EMISSION. 0.3 s after the
#   edit the graph compiles on the library's worker (frames keep drawing: the log counts them, at least 2, where a
#   blocking compile counts 1) and the Scene view's cube turns red without a save (probe); shader_graph_update_preview:
#   the same edit with the canvas in front, its preview inset's sphere red.
set(KAST_GRAPH_UPDATE_ARGS --scene ${KAST_SHADER_TEST_PROJECT}/shader_graph_demo.scene --size 1600x900 --fixed-dt 0.016
    --open-graph shaders/graph_demo.kshadergraph)
kast_graph_shot(update "${KAST_GRAPH_UPDATE_ARGS} --focus-panel Scene --graph-add constant_vec3,900,500 --graph-set 12.value=[1,0,0] --graph-link 12.out,1.EMISSION --screenshot-on-log 10,live-updated"
  -DFRAMES=600 "-DEXPECT_OUTPUT=live-updated [0-9.]+ s after the edit \\(([2-9]|[1-9][0-9]+) frames drawn while it compiled\\)"
  "-DFORBID_OUTPUT=\\[error\\]|Saved shader graph")
add_test(NAME shader_graph_update_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_update.png"
         0.428125,0.4,255,100,85,60)
set_tests_properties(shader_graph_update_screenshot PROPERTIES FIXTURES_SETUP shader_graph_update_shot)
set_tests_properties(shader_graph_update_probe PROPERTIES FIXTURES_REQUIRED shader_graph_update_shot)
kast_graph_shot(update_preview "${KAST_GRAPH_UPDATE_ARGS} --graph-add constant_vec3,900,500 --graph-set 12.value=[1,0,0] --graph-link 12.out,1.EMISSION --screenshot-on-log 10,live-updated"
  -DFRAMES=600 "-DEXPECT_OUTPUT=live-updated" "-DFORBID_OUTPUT=\\[error\\]|Saved shader graph")
add_test(NAME shader_graph_update_preview_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_update_preview.png"
         0.70625,0.5889,255,108,90,60)
set_tests_properties(shader_graph_update_preview_screenshot PROPERTIES FIXTURES_SETUP shader_graph_update_preview_shot)
set_tests_properties(shader_graph_update_preview_probe PROPERTIES FIXTURES_REQUIRED shader_graph_update_preview_shot)
# - shader_graph_broken: a Custom Expression shaderc rejects (its "vec3 o" spelled with a JSON escape for the
#   space: EXTRA_ARGS splits on spaces), and a vec4 into ALBEDO (a warning): the error maps to node
#   12 (outlined red, in the summary line and the log), the warning to node 1 (#ffcc00); the preview keeps the last good
#   version (not magenta: probe, the canvas' preview inset).
kast_graph_shot(broken "${KAST_GRAPH_UPDATE_ARGS} --graph-add custom_expression,1000,480 --graph-set 12.outputs=\"vec3\\u0020o\" --graph-set 12.code=\"o=undefinedThing\" --graph-link 12.o,1.EMISSION --graph-link 8.rgba,1.ALBEDO --screenshot-on-log 10,live-update"
  -DFRAMES=600 "-DEXPECT_OUTPUT=node 1: input 'ALBEDO': vec4 truncated to vec3.*node 12: .*live-update failed [0-9.]+ s after the edit. the last good version keeps rendering")
add_test(NAME shader_graph_broken_probe COMMAND kast_self_check --probe-png "${CMAKE_CURRENT_BINARY_DIR}/shader_graph_broken.png"
         0.70625,0.5889,126,108,90,60)
set_tests_properties(shader_graph_broken_screenshot PROPERTIES FIXTURES_SETUP shader_graph_broken_shot)
set_tests_properties(shader_graph_broken_probe PROPERTIES FIXTURES_REQUIRED shader_graph_broken_shot)
# - shader_graph_code: "View generated code" (the library's text); shader_graph_preview_cube: the preview's cube.
kast_graph_shot(code "${KAST_GRAPH_ARGS} --graph-view-code" "-DFORBID_OUTPUT=\\[error\\]")
kast_graph_shot(preview_cube "${KAST_GRAPH_ARGS} --graph-preview cube" "-DFORBID_OUTPUT=\\[error\\]")
# - shader_graph_asset_inspector: a .kshadergraph selected in Assets: its compile status and "Open in Shader Graph".
kast_graph_shot(asset_inspector "--project ${KAST_SHADER_TEST_PROJECT} --size 1600x900 --assets-select shaders/graph_demo.kshadergraph"
  "-DEXPECT_OUTPUT=Compiled shader 'shaders/graph_demo.kshadergraph'" "-DFORBID_OUTPUT=\\[error\\]")
# - shader_graph_material_edit: the material Inspector edits a graph's params on a fresh copy: two edits, Undo; the
#   .mat keeps the first.
kast_graph_shot(material_edit "--project ${KAST_GRAPH_PROJECT} --size 1600x900 --assets-select materials/shader_graph_demo.mat --material-set speed=2.5 --material-set tint=[0,1,0] --material-undo 1"
  "-DEXPECT_OUTPUT=--material-set speed = 2.5.*--material-set tint = \\[0,1,0\\]" "-DFORBID_OUTPUT=\\[error\\]")
add_test(NAME shader_graph_material_written COMMAND "${CMAKE_COMMAND}" -E cat "${KAST_GRAPH_PROJECT}/materials/shader_graph_demo.mat")
set_tests_properties(shader_graph_material_edit_screenshot PROPERTIES FIXTURES_REQUIRED shader_graph_saved
                     FIXTURES_SETUP shader_graph_material_edited)
set_tests_properties(shader_graph_material_written PROPERTIES FIXTURES_REQUIRED shader_graph_material_edited
  PASS_REGULAR_EXPRESSION "\"speed\": 2.5,[ \n]*\"tint\": \\[[ \n]*1.0,[ \n]*0.8,[ \n]*0.5[ \n]*\\]" FAIL_REGULAR_EXPRESSION "\"speed\": 1.0")

# Canvas gestures through --sim-mouse (in-app SDL events; ",hold" keeps the button down over the target) and --sim-key,
# at graph_demo's pin positions in the 1600x900 window (offsets from the Scene view's center, 800,352); the preview inset
# is collapsed where it would cover the Fragment Output's pins:
# - sim_link: Float.out dragged onto the Fragment Output's EMISSION links them.
# - sim_refused: Param pattern (texture2D) held over ALPHA: the red link and why; no link is made.
# - sim_pin_menu: Float.out dropped on empty canvas opens the add menu filtered from that pin.
# - sim_delete: a box around Time, Multiply and Param speed selects them; Delete removes them.
# - sim_copy_paste: the same box, Ctrl+C, Ctrl+V: three new nodes (their links kept) at the cursor, selected.
kast_graph_shot(sim_link "${KAST_GRAPH_ARGS} --graph-preview-collapse --sim-mouse 30,left,201,-164,0,86,160,8"
  "-DEXPECT_OUTPUT=Shader Graph: linked 11.out to 1.EMISSION" "-DFORBID_OUTPUT=\\[error\\]")
kast_graph_shot(sim_refused "${KAST_GRAPH_ARGS} --sim-mouse 30,left,292,91,0,-5,-150,100" -DFRAMES=50
  "-DFORBID_OUTPUT=\\[error\\]|Shader Graph: linked")
kast_graph_shot(sim_pin_menu "${KAST_GRAPH_ARGS} --graph-preview-collapse --sim-mouse 30,left,124,68,0,86,160,3"
  "-DEXPECT_OUTPUT=Shader Graph: add menu, [0-9]+ entries that link from 11.out" "-DFORBID_OUTPUT=\\[error\\]")
kast_graph_shot(sim_delete "${KAST_GRAPH_ARGS} --sim-mouse 30,left,315,-270,0,-465,248,3 --sim-key 45,Delete" -DFRAMES=70
  "-DEXPECT_OUTPUT=Shader Graph: deleted 3 nodes" "-DFORBID_OUTPUT=\\[error\\]")
kast_graph_shot(sim_copy_paste "${KAST_GRAPH_ARGS} --sim-mouse 30,left,315,-270,0,-465,248,3 --sim-key 45,ctrl+C --sim-key 55,ctrl+V"
  -DFRAMES=80 "-DEXPECT_OUTPUT=Shader Graph: copied 3 nodes.*Shader Graph: pasted 3 nodes" "-DFORBID_OUTPUT=\\[error\\]")
# - sim_canvas_menu: a right-click on empty canvas opens the add menu; sim_inset_menu / sim_inset_space: a right-click,
#   or Space, over the preview inset's image (1120,507) doesn't.
kast_graph_shot(sim_canvas_menu "${KAST_GRAPH_ARGS} --sim-mouse 30,right,0,0,0,-300,200"
  "-DEXPECT_OUTPUT=Shader Graph: add menu, [0-9]+ entries" "-DFORBID_OUTPUT=\\[error\\]")
kast_graph_shot(sim_inset_menu "${KAST_GRAPH_ARGS} --sim-mouse 30,right,0,0,0,320,155"
  "-DEXPECT_OUTPUT=--sim-mouse: press at 1120,507" "-DFORBID_OUTPUT=\\[error\\]|add menu")
kast_graph_shot(sim_inset_space "${KAST_GRAPH_ARGS} --sim-mouse 30,none,0,0,0,320,155 --sim-key 40,Space"
  "-DEXPECT_OUTPUT=--sim-mouse: press at 1120,507" "-DFORBID_OUTPUT=\\[error\\]|add menu")
# - sim_revert: an unsaved live edit, then opening another graph and clicking Discard: the library drops the edit and
#   recompiles graph_demo from its file (the Scene goes back to it).
kast_graph_shot(sim_revert "${KAST_GRAPH_UPDATE_ARGS} --graph-add constant_vec3,900,500 --graph-set 12.value=[1,0,0] --graph-link 12.out,1.EMISSION --open-graph shaders/empty.kshadergraph --sim-mouse 45,left,0,0,0,-176,121,1"
  -DFRAMES=90 "-DEXPECT_OUTPUT=live-updated.*Opened shader graph '[^']*empty.kshadergraph'.*Recompiled shader 'shaders/graph_demo.kshadergraph'"
  "-DFORBID_OUTPUT=Saved shader graph")

# Phase 5: the samples. shader_graphs.scene is custom_shaders.scene with materials/graph_*.mat, whose shaders are
# shader_graphs/*.kshadergraph: the hand-written pulse, wind, dissolve, water and toon samples rebuilt as graphs, with
# no Custom Expression. At frames 60 and 120, in the Scene view and in Play (the dissolve's threshold driven by
# scripts/dissolve_driver.lua), each shot must match custom_shaders.scene's (shader_custom*) with kast_self_check
# --diff-png at a mean of at most 1/255: the whole window, and each sample's crop (pixel rects in the 1600x900 default
# layout's Scene view and Game view).
set(graphs_compiled "Compiled shader 'shader_graphs/(pulse|wind|dissolve|water|toon)\\.kshadergraph'")
kast_shader_shot(graphs "${KAST_SHADER_TEST_PROJECT}" shader_graphs.scene
  "-DEXPECT_OUTPUT=${graphs_compiled}" -DEXPECT_COUNT=5 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
kast_shader_shot(graphs_f60 "${KAST_SHADER_TEST_PROJECT}" shader_graphs.scene -DFRAMES=60)
kast_shader_shot(graphs_play "${KAST_SHADER_TEST_PROJECT}" shader_graphs.scene PLAY
  "-DEXPECT_OUTPUT=${graphs_compiled}" -DEXPECT_COUNT=5 "-DFORBID_OUTPUT=\\[error\\]|\\[warn\\]|magenta")
kast_shader_shot(graphs_play_f60 "${KAST_SHADER_TEST_PROJECT}" shader_graphs.scene PLAY -DFRAMES=60)
set(KAST_SAMPLE_CROPS_SCENE "wind|480,320,230,140" "character|645,290,75,85" "dissolve|695,340,140,135"
    "toon|730,270,160,130" "pulse|870,370,125,125" "water|800,295,340,100")
set(KAST_SAMPLE_CROPS_GAME "wind|335,255,310,180" "character|600,215,80,105" "dissolve|650,265,210,190"
    "toon|705,190,210,175" "pulse|915,315,195,190" "water|800,240,476,110")
foreach(shot "f120||SCENE" "f60|_f60|SCENE" "play_f120|_play|GAME" "play_f60|_play_f60|GAME")
  string(REPLACE "|" ";" shot "${shot}")
  list(GET shot 0 name)
  list(GET shot 1 suffix)
  list(GET shot 2 view)
  foreach(crop "all|" ${KAST_SAMPLE_CROPS_${view}})
    string(REPLACE "|" ";" crop "${crop}")
    list(GET crop 0 sample)
    list(GET crop 1 rect)
    add_test(NAME shader_graph_samples_${name}_${sample}
      COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/shader_custom${suffix}.png"
              "${CMAKE_CURRENT_BINARY_DIR}/shader_graphs${suffix}.png" 0.00392 ${rect})
    set_tests_properties(shader_graph_samples_${name}_${sample} PROPERTIES
                         FIXTURES_REQUIRED "shader_custom${suffix}_shot;shader_graphs${suffix}_shot")
  endforeach()
endforeach()
# The wind's and the dissolve's crops, shadows included, change between frames 60 and 120 (a mean over 1/255 per
# channel: "diff FAIL"): the grass sways in the Scene view and in Play, the dissolve's threshold moves in Play.
foreach(moving "wind_scene|_f60||480,320,230,140" "wind_play|_play_f60|_play|335,255,310,180"
               "dissolve_play|_play_f60|_play|650,265,210,190")
  string(REPLACE "|" ";" moving "${moving}")
  list(GET moving 0 name)
  list(GET moving 1 a)
  list(GET moving 2 b)
  list(GET moving 3 rect)
  add_test(NAME shader_graph_samples_${name}_animates
    COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/shader_graphs${a}.png"
            "${CMAKE_CURRENT_BINARY_DIR}/shader_graphs${b}.png" 1 ${rect})
  set_tests_properties(shader_graph_samples_${name}_animates PROPERTIES PASS_REGULAR_EXPRESSION "diff FAIL: mean"
                       FIXTURES_REQUIRED "shader_graphs${a}_shot;shader_graphs${b}_shot")
endforeach()
# The player: Build shader_graphs.scene (no shaderc.exe in the game; player_test.cmake checks), its graphs load
# precompiled, and its 957x584 screenshot (the Game view's size in the default layout) matches the editor's Play shot's
# Game view (shown from window pixel 321,44, its left column and bottom row hidden under the panel's edges).
kast_game_project(shader_graphs "Shader Test" shader_graphs.scene FALSE "")
add_test(NAME build_game_shader_graphs
  COMMAND "${CMAKE_COMMAND}" "-DEDITOR=$<TARGET_FILE:kast_editor>" "-DPROJECT=${CMAKE_CURRENT_BINARY_DIR}/game_projects/shader_graphs"
          "-DOUT=${KAST_GAME_OUT}/shader_graphs" "-DEXE=Shader Test.exe" -DEXPECT=0
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_test.cmake")
add_test(NAME player_shader_graphs_screenshot
  COMMAND "${CMAKE_COMMAND}" "-DGAME_DIR=${KAST_GAME_OUT}/shader_graphs" "-DEXE=Shader Test.exe"
          "-DWORK_DIR=${KAST_GAME_OUT}/shader_graphs.run" "-DSCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/player_shader_graphs.png"
          "-DEXTRA_ARGS=--fixed-dt 0.016 --size 957x584" -DEXPECT=0
          "-DEXPECT_OUTPUT=Loaded shader 'shader_graphs/water\\.kshadergraph' \\(precompiled\\)" "-DFORBID_OUTPUT=\\[error\\]|magenta"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/player_test.cmake")
add_test(NAME player_shader_graphs_matches_editor
  COMMAND kast_self_check --diff-png "${CMAKE_CURRENT_BINARY_DIR}/shader_graphs_play.png"
          "${CMAKE_CURRENT_BINARY_DIR}/player_shader_graphs.png" 0.00392 322,44,955,583,1,0)
set_tests_properties(build_game_shader_graphs PROPERTIES FIXTURES_SETUP game_shader_graphs_built)
set_tests_properties(player_shader_graphs_screenshot PROPERTIES FIXTURES_REQUIRED game_shader_graphs_built
                     FIXTURES_SETUP player_shader_graphs_shot TIMEOUT 90)
set_tests_properties(player_shader_graphs_matches_editor PROPERTIES
                     FIXTURES_REQUIRED "player_shader_graphs_shot;shader_graphs_play_shot")
