# Stable asset ids end to end, on a copy of SOURCE (tests/project: legacy plain-string refs) without its .meta files:
# 1. the legacy project opens in the editor (shot legacy_editor.png) and builds and plays (shot legacy_player.png);
# 2. --save-scene upgrades models.scene and models_material.scene to {id, path} refs;
# 3. the editor's Assets rename (--assets-rename, with another scene open) renames models/cube.gltf, the material
#    materials/gold_r05.mat and the startScene models_material.scene: the rename pass rewrites the unopened scenes and
#    project.kast;
# 4. the renamed scenes open with no missing model or material (shots after_renamed_model.png, after_renamed_material.png);
# 5. a model renamed in Explorer with its .meta (no save since), then Build: game.pak ships fresh hints and the player
#    shows the start scene (shot after_build_player.png).
# Usage: cmake -DEDITOR=<exe> -DSOURCE=<project dir> -DWORK=<dir> -P asset_ids_test.cmake
set(P "${WORK}/project")
set(SHOTS "${WORK}/shots")
file(REMOVE_RECURSE "${WORK}")
file(COPY "${SOURCE}/" DESTINATION "${P}")
file(MAKE_DIRECTORY "${SHOTS}")
file(GLOB_RECURSE metas "${P}/*.meta")
if(metas)
  file(REMOVE ${metas})
endif()
# Build compiles every .kshader and cooks every model: drop the deliberately broken ones (as kast_game_project does).
file(REMOVE "${P}/shaders/broken.kshader" "${P}/shaders/bad_param.kshader" "${P}/models/corrupt.glb")
file(READ "${P}/project.kast" project)
string(REGEX REPLACE "\"startScene\": \"[^\"]*\"" "\"startScene\": \"models_material.scene\"" project "${project}")
file(WRITE "${P}/project.kast" "${project}")

# Runs the editor (output in `output`); it must exit 0.
function(editor)
  execute_process(COMMAND "${EDITOR}" --projects-file "${WORK}/projects.json" --settings-file "${WORK}/editor_settings.json"
                          ${ARGN}
                  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  message("${out}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "kast_editor ${ARGN} exited with ${result}")
  endif()
  set(output "${out}" PARENT_SCOPE)
endfunction()
# Runs the built game in dir (only Windows on PATH, as player_test.cmake); it must exit 0.
function(player dir shot)
  set(path "$ENV{PATH}")
  set(ENV{PATH} "$ENV{SystemRoot}/System32;$ENV{SystemRoot}")
  execute_process(COMMAND "${dir}/Switch Test.exe" --frames 120 --screenshot "${shot}" WORKING_DIRECTORY "${dir}" TIMEOUT 60
                  RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
  set(ENV{PATH} "${path}")
  message("${out}")
  if(NOT result EQUAL 0 OR NOT EXISTS "${shot}")
    message(FATAL_ERROR "the built game exited with ${result}")
  endif()
  set(output "${out}" PARENT_SCOPE)
endfunction()
macro(expect text)
  string(FIND "${output}" "${text}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Output lacks '${text}'")
  endif()
endmacro()
macro(forbid regex)
  if(output MATCHES "${regex}")
    message(FATAL_ERROR "Output has the forbidden '${CMAKE_MATCH_0}'")
  endif()
endmacro()
macro(file_has file regex)
  file(READ "${P}/${file}" text)
  if(NOT text MATCHES "${regex}")
    message(FATAL_ERROR "${file} does not match '${regex}'")
  endif()
endmacro()
set(missing "cannot read the file|Missing asset|rendering it magenta")
set(id "\"id\": \"[0-9a-f-]+\",[ \n]*")

# 1. Legacy refs, no .meta: the editor creates the .meta files and opens the scene; Build and the player load it.
editor(--project "${P}" --frames 120 --screenshot "${SHOTS}/legacy_editor.png")
expect("asset id file(s) (.meta)")
expect("Opened scene")
forbid("${missing}")
editor(--project "${P}" --build "${WORK}/legacy_out")
player("${WORK}/legacy_out" "${SHOTS}/legacy_player.png")
forbid("${missing}")

# 2. Save upgrades the refs.
foreach(scene models.scene models_material.scene)
  editor(--project "${P}" --scene "${P}/${scene}" --save-scene "${P}/${scene}")
endforeach()
file_has(models.scene "\"model\": {[ \n]*${id}\"path\": \"models/cube.gltf\"")
file_has(models_material.scene "${id}\"path\": \"materials/gold_r05.mat\"")

# 3. Renames in the Assets panel while another scene is open.
foreach(rename models/cube.gltf=cube_renamed.gltf materials/gold_r05.mat=gold_renamed.mat models_material.scene=hero.scene)
  editor(--project "${P}" --scene "${P}/scene_a.scene" --focus-panel Assets --frames 20 --assets-rename ${rename}
         --screenshot "${WORK}/rename.png")
  expect("Updated asset references in '")
endforeach()
file_has(models.scene "${id}\"path\": \"models/cube_renamed.gltf\"")
file_has(hero.scene "${id}\"path\": \"materials/gold_renamed.mat\"")
file_has(project.kast "\"startScene\": \"hero.scene\"") # legacy: a plain string until project.kast is saved
file(READ "${P}/models.scene" text)
if(text MATCHES "\"models/cube.gltf\"")
  message(FATAL_ERROR "models.scene still names models/cube.gltf")
endif()

# 4. The renamed scenes open with their model and material.
editor(--project "${P}" --scene "${P}/models.scene" --frames 120 --screenshot "${SHOTS}/after_renamed_model.png")
forbid("${missing}")
editor(--project "${P}" --scene "${P}/hero.scene" --frames 120 --screenshot "${SHOTS}/after_renamed_material.png")
forbid("${missing}")

# 5. Explorer rename with the .meta, then Build: the shipped hero.scene names the model's new path.
file(RENAME "${P}/models/female-128-anim.glb" "${P}/models/heroine.glb")
file(RENAME "${P}/models/female-128-anim.glb.meta" "${P}/models/heroine.glb.meta")
editor(--project "${P}" --build "${WORK}/out")
expect("Shipping 'hero.scene' with its asset references' current paths")
file_has(hero.scene "\"path\": \"models/female-128-anim.glb\"") # the source stays as it was
player("${WORK}/out" "${SHOTS}/after_build_player.png")
forbid("${missing}")
message(STATUS "OK: shots in ${SHOTS}")
