#pragma once

#include "prefab.h"
#include "scene.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// .scene and .prefab files: JSON, see docs/engine/coordinates-and-scenes.html.
namespace kast {

constexpr int kSceneFileVersion = 5; // the only version that loads (.prefab files too); older files are not migrated

// What a successful load changed or kept. Built-in components are strict (any problem fails the load); Lua components
// get the drift rules (migrateLuaComponent), components of unknown type are kept verbatim (PreservedComponents) and a
// prefab instance whose prefab doesn't load becomes a placeholder.
struct SceneLoadReport {
    std::vector<std::string> warnings; // one per drift fix, unknown component or missing prefab, naming the entity
    bool migrated = false;             // some Lua component data was changed by the drift rules
};

// Replaces out only on success; on failure out is untouched and error says why. Logs each report warning, prefixed
// with the path. Prefab instances load their .prefab from projectRoot (empty: no project, every instance is a
// missing-prefab placeholder).
bool loadScene(const std::string& path, Scene& out, std::string& error, SceneLoadReport* report = nullptr,
               const std::filesystem::path& projectRoot = {});

// Deterministic output: entities depth-first in child order, fixed key order, shortest round-trip floats.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);

// The same, in memory (Play mode snapshots). sceneFromJson's error does not include a path.
nlohmann::ordered_json sceneToJson(const Scene& scene);
bool sceneFromJson(const nlohmann::ordered_json& json, Scene& out, std::string& error, SceneLoadReport* report = nullptr,
                   const std::filesystem::path& projectRoot = {});

// .prefab files: {"version", "entities"} holding exactly one root entity, which isn't itself a prefab instance.
// loadPrefab resolves nested instances (one that contains the prefab itself is a missing-prefab placeholder).
bool loadPrefab(const std::filesystem::path& projectRoot, const std::string& path, std::shared_ptr<const PrefabAsset>& out,
                std::string& error, SceneLoadReport* report = nullptr);
// The prefab as an editable scene (prefab edit mode, Apply): entity ids are the prefab-local ids.
bool loadPrefabScene(const std::filesystem::path& projectRoot, const std::string& path, Scene& out, std::string& error,
                     SceneLoadReport* report = nullptr);
nlohmann::ordered_json prefabToJson(const Scene& scene);
// Writes scene as the prefab projectRoot / path. False + error if it hasn't exactly one root, its root is an instance,
// or it holds an instance of path itself (prefabCycleError).
bool savePrefab(const Scene& scene, const std::filesystem::path& projectRoot, const std::string& path, std::string& error);

// Every component of entity as the scene file saves it: type name -> JSON, built-ins, Lua types, then preserved ones.
nlohmann::ordered_json entityComponentsToJson(const entt::registry& registry, entt::entity entity);
// An instance entry's overrides: an entity's current name and components (as entityComponentsToJson) against base.
// root: the instance root, whose Name is not compared and whose Transform is always included whole.
nlohmann::ordered_json diffPrefabEntity(const std::string& name, const nlohmann::ordered_json& components,
                                        const PrefabAsset::Entity& base, bool root);

// A float as the JSON number with the fewest digits that reads back as the same float.
double jsonNumber(float value);

} // namespace kast
