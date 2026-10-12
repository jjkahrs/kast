#pragma once

#include "scene.h"

#include <string>

// .scene files: JSON, see README "Scene file format".
namespace kast {

constexpr int kSceneFileVersion = 3;
constexpr int kOldestSceneFileVersion = 2; // still loads; version 2 files have no "systems"

// Replaces out only on success; on failure out is untouched and error says why.
bool loadScene(const std::string& path, Scene& out, std::string& error);

// Deterministic output: entities depth-first in child order, fixed key order, shortest round-trip floats.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);

// The same, in memory (Play mode snapshots). sceneFromJson's error does not include a path.
nlohmann::ordered_json sceneToJson(const Scene& scene);
bool sceneFromJson(const nlohmann::ordered_json& json, Scene& out, std::string& error);

// A float as the JSON number with the fewest digits that reads back as the same float.
double jsonNumber(float value);

} // namespace kast
