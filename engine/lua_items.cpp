#include "lua_items.h"

#include "files.h"
#include "lua_sandbox.h"

#include <algorithm>
#include <format>

namespace kast {
namespace {

using Invalid = InvalidDefinition;
using Json = nlohmann::json;

constexpr size_t kMaxIdBytes = 64;
constexpr size_t kMaxTextBytes = 255; // name, icon
constexpr size_t kMaxDescriptionBytes = 1024;
constexpr int64_t kMaxStack = 1000;
constexpr int64_t kMaxSellPrice = 1000000000;
constexpr const char* kQualities[] = {"poor", "common", "uncommon", "rare", "epic"};

void text(const Json& item, const char* key, size_t minBytes, size_t maxBytes, const std::string& what)
{
    if (!item.contains(key) || !item[key].is_string() || item[key].get<std::string>().size() < minBytes ||
        item[key].get<std::string>().size() > maxBytes)
        throw Invalid(minBytes ? std::format("{}\"{}\" must be a non-empty string of at most {} bytes", what, key, maxBytes)
                               : std::format("{}\"{}\" must be a string of at most {} bytes", what, key, maxBytes));
}

void integer(const Json& item, const char* key, int64_t lo, int64_t hi, const std::string& what)
{
    if (!item.contains(key) || !item[key].is_number_integer() || item[key].get<int64_t>() < lo || item[key].get<int64_t>() > hi)
        throw Invalid(std::format("{}\"{}\" must be an integer in [{}, {}]", what, key, lo, hi));
}

Json parseItem(sol::state& lua, const std::filesystem::path& file, const std::string& path, std::string& declared)
{
    std::string error;
    sol::protected_function body = loadSandboxed(lua, file, "@" + path, sandboxEnvironment(lua), error);
    if (!body.valid())
        throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid())
        throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    const std::string what = path + ": ";
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(what + "must return exactly one table {id = ..., name = ..., icon = ..., quality = ..., maxStack = ..., "
                             "sellPrice = ..., description = ...}");
    const sol::table table = result;
    checkPlainData(table, what);
    checkKeys(table, {"id", "name", "icon", "quality", "maxStack", "sellPrice", "description"}, what);
    Json item;
    if (!luaToJson(table, item, error))
        throw Invalid(what + error);
    text(item, "id", 1, kMaxIdBytes, what);
    declared = item["id"];
    if (!std::ranges::all_of(declared, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
        throw Invalid(what + "\"id\" may only hold a-z, 0-9 and _");
    text(item, "name", 1, kMaxTextBytes, what);
    text(item, "icon", 1, kMaxTextBytes, what);
    text(item, "description", 0, kMaxDescriptionBytes, what);
    if (!item.contains("quality") || !item["quality"].is_string() ||
        std::ranges::none_of(kQualities, [&](const char* q) { return item["quality"] == q; }))
        throw Invalid(what + "\"quality\" must be one of \"poor\", \"common\", \"uncommon\", \"rare\", \"epic\"");
    integer(item, "maxStack", 1, kMaxStack, what);
    integer(item, "sellPrice", 0, kMaxSellPrice, what);
    return item;
}

} // namespace

ItemDefinitions loadItemDefinitions(sol::state& lua, const std::filesystem::path& root)
{
    struct Parsed {
        std::string path, id, error; // id: as declared, even if the file was rejected later
        Json item;
    };
    std::vector<Parsed> parsed;
    for (const std::filesystem::path& file : listFiles(root / kItemsDir)) {
        if (file.extension() != ".lua")
            continue;
        Parsed p;
        p.path = file.lexically_relative(root).generic_string();
        try {
            p.item = parseItem(lua, file, p.path, p.id);
        } catch (const Invalid& e) {
            p.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            p.error = std::format("{}: {}", p.path, e.what());
        }
        parsed.push_back(std::move(p));
    }
    // Id clashes reject every file of an id, so the result doesn't depend on scan order.
    for (Parsed& p : parsed) {
        if (!p.error.empty())
            continue;
        std::string others;
        for (const Parsed& o : parsed)
            if (&o != &p && o.id == p.id)
                others += std::format("{}{}", others.empty() ? "" : ", ", o.path);
        if (!others.empty())
            p.error = std::format("{}: id \"{}\" is also declared in {}", p.path, p.id, others);
    }
    ItemDefinitions out;
    for (Parsed& p : parsed) {
        if (p.error.empty())
            out.items.emplace(p.id, std::move(p.item));
        else
            out.errors.push_back(std::move(p.error));
    }
    return out;
}

} // namespace kast
