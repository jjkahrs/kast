#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

// The per-user editor settings file (editor_settings.json, shared by every project): one JSON object whose keys belong
// to the features using them ("externalEditor", "onlineEmail", "composeFolder", "worldOverride"). Never holds
// passwords or tokens. No ImGui.

// <SDL_GetPrefPath("Kast", "Editor")>/editor_settings.json (%APPDATA%/Kast/Editor). Empty if SDL can't make that folder.
std::filesystem::path defaultEditorSettingsFile();
// The settings object; {} if file is missing, or (with a warning naming "Editor settings") not a JSON object.
nlohmann::json loadEditorSettings(const std::filesystem::path& file);
// settings[key] if it is a string, else fallback.
std::string editorSetting(const nlohmann::json& settings, const char* key, std::string fallback = {});
// Sets key to value in file, keeping its other keys, creating file's folder if needed. False + error on failure.
bool saveEditorSetting(const std::filesystem::path& file, const char* key, const std::string& value, std::string& error);
