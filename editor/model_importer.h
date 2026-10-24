#pragma once

#include <model.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// The editor's kast::ModelImporter: Assimp, linked only into the editor and the tests (kast_model_import in
// CMakeLists.txt), never into the player, which loads the models Build cooked (kast::cookModel, model.h).
// Imports <root>/<path> (sidecar .bin/.mtl/image files resolve next to it). External texture paths become
// project-relative, embedded ones "<path>#<i>". LODs follow its import settings (kast::modelImportSettingsPath; a
// malformed file warns and uses the defaults): authored "<name>_LOD<n>" nodes or meshes, else generated ones.
// nullptr + error if it can't be read or parsed.
std::shared_ptr<const kast::Model> importModel(const std::filesystem::path& root, const std::string& path,
                                               std::string& error);
// The same from bytes, with the default LOD settings; hint is the format's extension without the dot (e.g. "gltf").
// Embedded textures are "#<i>".
std::shared_ptr<const kast::Model> importModelFromMemory(std::string_view bytes, const char* hint, std::string& error);
// The model cache's version (ModelLibrary::setImporter): the Assimp version, the import flags and this importer's
// revision, so upgrading Assimp or changing the conversion misses the cache.
std::string modelImporterVersion();

// A Unity-style authored LOD name, "<base>_LOD<n>" (any case of "LOD"): true with its base and level n.
bool authoredLodName(std::string_view name, std::string& base, int& level);
// Fills mesh.lods with settings.ratios' levels after LOD0 (none if ratios has one), each an index-only
// meshopt_simplifyWithAttributes of LOD0 (normals and UVs weighted, borders locked) over its unchanged vertices.
void generateLods(kast::ModelMesh& mesh, const kast::ModelLodSettings& settings);
