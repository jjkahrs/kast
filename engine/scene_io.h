#pragma once

#include "prefab.h"
#include "scene.h"

#include <filesystem>
#include <memory>
#include <optional>
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

// Deterministic output: entities depth-first in child order, fixed key order, shortest round-trip floats. Writes one
// loaded scene (additive scenes, scene.h): its entities with their file ids and its own settings; without slot, the
// active one. out's (only) loaded scene gets path as its path.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);
bool saveScene(const Scene& scene, SceneSlot slot, const std::string& path, std::string& error);

// The same, in memory. sceneFromJson's error does not include a path.
nlohmann::ordered_json sceneToJson(const Scene& scene);
nlohmann::ordered_json sceneToJson(const Scene& scene, SceneSlot slot);
bool sceneFromJson(const nlohmann::ordered_json& json, Scene& out, std::string& error, SceneLoadReport* report = nullptr,
                   const std::filesystem::path& projectRoot = {});

// Additive scenes (docs/engine/additive-scenes.html). appendScene moves from's entities and settings into into as one
// more loaded scene named path (last, not active; see Scene::addScene for slot) and returns its slot; from is left
// empty. loadSceneAdditive loads the file path that way; false + error (into unchanged) if into already has a scene
// with that path or the load fails.
SceneSlot appendScene(Scene& into, Scene&& from, std::string path, std::optional<SceneSlot> slot = std::nullopt);
bool loadSceneAdditive(const std::string& path, Scene& into, SceneSlot& slot, std::string& error,
                       SceneLoadReport* report = nullptr, const std::filesystem::path& projectRoot = {});
// Every loaded scene with its slot and path, and the active one: Play mode snapshots, prefab instance reloads.
// scenesFromJson rebuilds the same slots, paths and active scene; out is replaced only on success.
nlohmann::ordered_json scenesToJson(const Scene& scene);
bool scenesFromJson(const nlohmann::ordered_json& json, Scene& out, std::string& error, SceneLoadReport* report = nullptr,
                    const std::filesystem::path& projectRoot = {});

// A scene's "lighting" > "lightmaps" block (kast.lightmaps uses these too): every field, and back (missing fields
// default). lightmapsFromJson replaces out only on success; error says why not.
nlohmann::ordered_json lightmapsToJson(const SceneLightmaps& lightmaps);
bool lightmapsFromJson(const nlohmann::ordered_json& json, SceneLightmaps& out, std::string& error);
// The "lighting" > "sdfgi" block (kast.sdfgi uses these too), likewise. A scene file with a bad one loads with it
// dropped (SDFGI off) and a warning naming the field.
nlohmann::ordered_json sdfgiToJson(const SceneSdfgi& sdfgi);
bool sdfgiFromJson(const nlohmann::ordered_json& json, SceneSdfgi& out, std::string& error);
// A scene's "sky" block (kast.sky uses these too). skyFromJson replaces out only on success; error says why not.
nlohmann::ordered_json skyToJson(const SceneSky& sky);
bool skyFromJson(const nlohmann::ordered_json& json, SceneSky& out, std::string& error);
// One "postProcessing" block, e.g. "ssao" (kast.ssao uses these): every field of post's, and json back into out's
// through the scene file's validation (missing fields default). postBlockFromJson changes out only on success; error
// says why not.
nlohmann::ordered_json postBlockToJson(const ScenePostProcessing& post, const char* name);
bool postBlockFromJson(const nlohmann::ordered_json& json, const char* name, ScenePostProcessing& out, std::string& error);

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
