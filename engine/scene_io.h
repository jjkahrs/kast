#pragma once

#include "scene.h"

#include <string>

// .scene files: JSON, see README "Scene file format".
namespace kast {

constexpr int kSceneFileVersion = 1;

// Replaces out only on success; on failure out is untouched and error says why.
bool loadScene(const std::string& path, Scene& out, std::string& error);

// Deterministic output: entities depth-first in child order, fixed key order, shortest round-trip floats.
bool saveScene(const Scene& scene, const std::string& path, std::string& error);

// A float as the JSON number with the fewest digits that reads back as the same float.
double jsonNumber(float value);

} // namespace kast
