#pragma once

#include "scene.h"

#include <string>
#include <vector>

// .scene files: JSON, see docs/engine/coordinates-and-scenes.html.
namespace kast {

constexpr int kSceneFileVersion = 4; // the only version that loads; older files are not migrated

// What a successful load changed or kept. Built-in components are strict (any problem fails the load); Lua components
// get the drift rules (migrateLuaComponent) and components of unknown type are kept verbatim (PreservedComponents).
struct SceneLoadReport {
    std::vector<std::string> warnings; // one per drift fix or unknown component, naming the entity
    bool migrated = false;             // some Lua component data was changed by the drift rules
};

// Replaces out only on success; on failure out is untouched and error says why. Logs each report warning, prefixed
// with the path.
bool loadScene(const std::string& path, Scene& out, std::string& error, SceneLoadReport* report = nullptr);

// Deterministic output: entities depth-first in child order, fixed key order, shortest round-trip floats.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);

// The same, in memory (Play mode snapshots). sceneFromJson's error does not include a path.
nlohmann::ordered_json sceneToJson(const Scene& scene);
bool sceneFromJson(const nlohmann::ordered_json& json, Scene& out, std::string& error, SceneLoadReport* report = nullptr);

// A float as the JSON number with the fewest digits that reads back as the same float.
double jsonNumber(float value);

} // namespace kast
