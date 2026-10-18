#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sol {
class state;
}

// Item definition files (<project>/items/**/*.lua, one item each) for kast.items. The world server loads the same
// files with the same rules (services/world/server/lua/items.cpp), so client and server agree on names, stack sizes
// and qualities. See docs/engine/lua-systems.html#items. Engine-internal: needs sol2.
namespace kast {

constexpr const char* kItemsDir = "items";

struct ItemDefinitions {
    // By id: {"id", "name", "icon", "quality", "maxStack", "sellPrice", "description"}, every field checked.
    std::map<std::string, nlohmann::json> items;
    std::vector<std::string> errors; // one per rejected file, each starting with its path, sorted by path
};

// Loads and checks every .lua file under root/items/ (none if the folder is missing; disk or the mounted pak), each in
// its own sandboxed environment. Duplicate ids reject every file of that id.
ItemDefinitions loadItemDefinitions(sol::state& lua, const std::filesystem::path& root);

} // namespace kast
