#pragma once

#include <model.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// The editor's kast::ModelImporter: Assimp, linked only into the editor and the tests (kast_model_import in
// CMakeLists.txt), never into the player, which loads the models Build cooked (kast::cookModel, model.h).
// Imports <root>/<path> (sidecar .bin/.mtl/image files resolve next to it). External texture paths become
// project-relative, embedded ones "<path>#<i>". nullptr + error if it can't be read or parsed.
std::shared_ptr<const kast::Model> importModel(const std::filesystem::path& root, const std::string& path,
                                               std::string& error);
// The same from bytes; hint is the format's extension without the dot (e.g. "gltf"). Embedded textures are "#<i>".
std::shared_ptr<const kast::Model> importModelFromMemory(std::string_view bytes, const char* hint, std::string& error);
// The model cache's version (ModelLibrary::setImporter): the Assimp version, the import flags and this importer's
// revision, so upgrading Assimp or changing the conversion misses the cache.
std::string modelImporterVersion();
