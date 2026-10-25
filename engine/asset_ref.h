#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

// Asset references in project files (.scene, .prefab, .mat, project.kast, maps/<map>/map.json and objects/*.json):
// {"id": "<uuid>", "path": "<project-relative path>"}, or a plain path string (the legacy form, still read). The engine,
// player and Lua load by path: ids are the editor's (editor/asset_ids.h), which plugs them in through the hooks.
namespace kast {

struct AssetRefHooks {
    // The path to load for a ref read from a file (path: its hint). Unset: the hint.
    std::function<std::string(const std::string& id, const std::string& path)> resolve;
    // The id of the asset at a project-relative path, if it has one. Unset: none, every ref is written as its path.
    std::function<std::optional<std::string>(const std::string& path)> idOf;
};
// ponytail: one global set, like the component registry; the editor works on one project at a time.
void setAssetRefHooks(AssetRefHooks hooks);
const AssetRefHooks& assetRefHooks();

// json is a ref: an object of exactly a string "id" and a string "path".
template <typename Json>
bool isAssetRef(const Json& json)
{
    if (!json.is_object() || json.size() != 2)
        return false;
    const auto id = json.find("id"), path = json.find("path");
    return id != json.end() && id->is_string() && path != json.end() && path->is_string();
}

// After parsing a project file: replaces every ref in json by the path to load (its hint, or the resolve hook's).
template <typename Json>
void readAssetRefs(Json& json)
{
    if (isAssetRef(json)) {
        const AssetRefHooks& hooks = assetRefHooks();
        std::string path = json["path"].template get<std::string>();
        json = hooks.resolve ? hooks.resolve(json["id"].template get<std::string>(), path) : std::move(path);
    } else if (json.is_structured()) {
        for (auto& element : json)
            readAssetRefs(element);
    }
}

// Before writing a project file: replaces every string value in json that names an asset with an id (the idOf hook)
// by its ref. Keys stay as they are.
template <typename Json>
void writeAssetRefs(Json& json)
{
    const auto& idOf = assetRefHooks().idOf;
    if (!idOf)
        return;
    if (json.is_string()) {
        std::string path = json.template get<std::string>();
        if (std::optional<std::string> id = idOf(path))
            json = Json{{"id", std::move(*id)}, {"path", std::move(path)}};
    } else if (json.is_structured()) {
        for (auto& element : json)
            writeAssetRefs(element);
    }
}

} // namespace kast
