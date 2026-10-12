#include "scene_io.h"

#include "components.h"

#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const Json& field(const Json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key))
        throw Invalid(std::format("missing \"{}\"", key));
    return object[key];
}

EntityId entityId(const Json& value, const char* what)
{
    if (!value.is_number_unsigned() || value.get<EntityId>() == 0)
        throw Invalid(std::format("\"{}\" must be a positive integer", what));
    return value.get<EntityId>();
}

// One parsed entity, before it is placed in the hierarchy.
struct Record {
    EntityId id = 0;
    std::optional<EntityId> parent;
    std::string name;
    std::vector<entt::meta_any> components; // registration order
};

Record parseEntity(const Json& json)
{
    if (!json.is_object())
        throw Invalid("must be an object");
    Record record;
    record.id = entityId(field(json, "id"), "id");
    const Json& name = field(json, "name");
    if (!name.is_string())
        throw Invalid("\"name\" must be a string");
    record.name = name.get<std::string>();
    if (const Json& parent = field(json, "parent"); !parent.is_null())
        record.parent = entityId(parent, "parent");

    const Json& components = field(json, "components");
    if (!components.is_object())
        throw Invalid("\"components\" must be an object");
    for (const auto& [key, value] : components.items())
        if (!findComponentType(key))
            throw Invalid(std::format("unknown component \"{}\"", key));
    if (!components.contains(entt::resolve<Transform>().name()))
        throw Invalid("missing \"Transform\" component");
    for (const entt::meta_type& type : componentTypes()) {
        if (!components.contains(type.name()))
            continue;
        std::string error;
        entt::meta_any component = componentFromJson(type, components[type.name()], error);
        if (!component)
            throw Invalid(std::format("\"{}\": {}", type.name(), error));
        record.components.push_back(std::move(component));
    }
    return record;
}

void build(const std::vector<Record>& records, Scene& scene)
{
    std::map<EntityId, size_t> byId;
    std::map<std::optional<EntityId>, std::vector<size_t>> childrenOf; // file order; nullopt = roots
    for (size_t i = 0; i < records.size(); ++i) {
        if (!byId.emplace(records[i].id, i).second)
            throw Invalid(std::format("duplicate entity id {}", records[i].id));
        childrenOf[records[i].parent].push_back(i);
    }
    for (const Record& record : records)
        if (record.parent && !byId.contains(*record.parent))
            throw Invalid(std::format("entity {} has parent {}, which does not exist", record.id, *record.parent));

    size_t created = 0;
    auto place = [&](auto& self, std::optional<EntityId> parentId, entt::entity parent) -> void {
        for (size_t index : childrenOf[parentId]) {
            const Record& record = records[index];
            const entt::entity entity = scene.createWithId(record.id, record.name, parent);
            for (const entt::meta_any& component : record.components)
                setComponent(scene.registry(), entity, component);
            ++created;
            self(self, record.id, entity);
        }
    };
    place(place, std::nullopt, entt::null);
    // Every entity whose parent chain ends at a root was placed; the rest are in a parent cycle.
    if (created != records.size())
        throw Invalid("entity parent references form a cycle");
}

} // namespace

double jsonNumber(float value)
{
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double result = 0.0;
    std::from_chars(buffer, written.ptr, result);
    return result;
}

bool sceneFromJson(const Json& json, Scene& out, std::string& error)
{
    try {
        const Json& version = field(json, "version");
        if (!version.is_number_integer() || version.get<int>() < kOldestSceneFileVersion ||
            version.get<int>() > kSceneFileVersion)
            throw Invalid(std::format("unsupported \"version\" {} (expected {} to {})", version.dump(),
                                      kOldestSceneFileVersion, kSceneFileVersion));

        const Json& entities = field(json, "entities");
        if (!entities.is_array())
            throw Invalid("\"entities\" must be an array");
        std::vector<Record> records;
        for (size_t i = 0; i < entities.size(); ++i) {
            try {
                records.push_back(parseEntity(entities[i]));
            } catch (const Invalid& e) {
                throw Invalid(std::format("entities[{}]: {}", i, e.what()));
            }
        }

        Scene scene;
        build(records, scene);
        if (json.contains("editor")) {
            if (!json["editor"].is_object())
                throw Invalid("\"editor\" must be an object");
            scene.editorData = json["editor"];
        }
        if (json.contains("systems")) {
            const Json& systems = json["systems"];
            if (!systems.is_array())
                throw Invalid("\"systems\" must be an array");
            for (size_t i = 0; i < systems.size(); ++i) {
                if (!systems[i].is_string() || systems[i].get<std::string>().empty())
                    throw Invalid(std::format("systems[{}]: must be a non-empty string", i));
                scene.systems.push_back(systems[i].get<std::string>());
            }
        }
        out = std::move(scene);
        return true;
    } catch (const Json::exception& e) {
        error = e.what();
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

bool loadScene(const std::string& path, Scene& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::format("Cannot open scene '{}'", path);
        return false;
    }
    std::string reason;
    try {
        if (sceneFromJson(Json::parse(file), out, reason))
            return true;
    } catch (const Json::exception& e) {
        reason = e.what();
    }
    error = std::format("Invalid scene '{}': {}", path, reason);
    return false;
}

Json sceneToJson(const Scene& scene)
{
    const entt::registry& registry = scene.registry();
    Json entities = Json::array();
    scene.forEachDepthFirst([&](entt::entity entity) {
        const entt::entity parent = scene.parent(entity);
        Json json;
        json["id"] = registry.get<PersistentId>(entity).value;
        json["name"] = registry.get<Name>(entity).value;
        json["parent"] = parent == entt::null ? Json(nullptr) : Json(registry.get<PersistentId>(parent).value);
        Json& components = json["components"] = Json::object();
        for (const entt::meta_type& type : componentTypes())
            if (hasComponent(registry, entity, type))
                components[type.name()] = componentToJson(getComponent(registry, entity, type));
        entities.push_back(std::move(json));
    });

    Json root;
    root["version"] = kSceneFileVersion;
    root["entities"] = std::move(entities);
    root["systems"] = scene.systems;
    root["editor"] = scene.editorData;
    return root;
}

bool saveScene(const Scene& scene, const std::string& path, std::string& error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << sceneToJson(scene).dump(2) << '\n';
    file.close();
    if (!file) {
        error = std::format("Cannot write scene '{}'", path);
        return false;
    }
    return true;
}

} // namespace kast
