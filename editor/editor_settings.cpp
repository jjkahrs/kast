#include "editor_settings.h"

#include "assets.h" // utf8, fromUtf8

#include <files.h>
#include <log.h>

#include <SDL3/SDL.h>

#include <format>
#include <fstream>

std::filesystem::path defaultEditorSettingsFile()
{
    char* dir = SDL_GetPrefPath("Kast", "Editor");
    if (!dir) {
        kast::log::error(std::format("Cannot find the editor settings folder: {}", SDL_GetError()));
        return {};
    }
    const std::filesystem::path file = fromUtf8(dir) / "editor_settings.json";
    SDL_free(dir);
    return file;
}

nlohmann::json loadEditorSettings(const std::filesystem::path& file)
{
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec))
        return nlohmann::json::object();
    try {
        std::ifstream in(file, std::ios::binary);
        nlohmann::json settings = nlohmann::json::parse(in);
        if (settings.is_object())
            return settings;
        kast::log::warn(std::format("Editor settings '{}' are not a JSON object; using the defaults", utf8(file)));
    } catch (const nlohmann::json::exception& e) {
        kast::log::warn(std::format("Editor settings '{}' are invalid ({}); using the defaults", utf8(file), e.what()));
    }
    return nlohmann::json::object();
}

std::string editorSetting(const nlohmann::json& settings, const char* key, std::string fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool saveEditorSetting(const std::filesystem::path& file, const char* key, const std::string& value, std::string& error)
{
    nlohmann::json settings = loadEditorSettings(file);
    settings[key] = value;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (file.empty() || !kast::writeFileAtomic(file, settings.dump(2) + '\n')) {
        error = std::format("Cannot write the editor settings '{}'", utf8(file));
        return false;
    }
    return true;
}
