# Shader Graph panels (docs/editor/shader-graph.html), included from CMakeLists.txt: --open-graph and the --graph-* flags
# run the panels' edits on tests/project/shaders/graph_demo.kshadergraph (node ids in that file), screenshots of the
# canvas and its prompts; edits that write go to a fresh copy of the project. The edit API itself is checked by
# kast_self_check --shaders (checkShaderGraphEdits).
set(KAST_GRAPH_ARGS --project ${KAST_TEST_PROJECT} --size 1600x900 --open-graph shaders/graph_demo.kshadergraph)

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
