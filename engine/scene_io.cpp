#include "scene_io.h"

#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::pair<Primitive, const char*> kPrimitiveNames[] = {
    {Primitive::Cube, "cube"}, {Primitive::Plane, "plane"}, {Primitive::Sphere, "sphere"}};

struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const Json& field(const Json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key))
        throw Invalid(std::format("missing \"{}\"", key));
    return object[key];
}

float number(const Json& value, const char* what)
{
    if (!value.is_number())
        throw Invalid(std::format("\"{}\" must be a number", what));
    return value.get<float>();
}

template <size_t N>
std::array<float, N> numbers(const Json& value, const char* what)
{
    if (!value.is_array() || value.size() != N)
        throw Invalid(std::format("\"{}\" must be an array of {} numbers", what, N));
    std::array<float, N> out{};
    for (size_t i = 0; i < N; ++i)
        out[i] = number(value[i], what);
    return out;
}

bx::Vec3 vec3(const Json& object, const char* key)
{
    const auto v = numbers<3>(field(object, key), key);
    return {v[0], v[1], v[2]};
}

EntityId entityId(const Json& value, const char* what)
{
    if (!value.is_number_unsigned() || value.get<EntityId>() == 0)
        throw Invalid(std::format("\"{}\" must be a positive integer", what));
    return value.get<EntityId>();
}

Json toJson(const bx::Vec3& v) { return Json::array({jsonNumber(v.x), jsonNumber(v.y), jsonNumber(v.z)}); }

Json toJson(const bx::Quaternion& q)
{
    return Json::array({jsonNumber(q.x), jsonNumber(q.y), jsonNumber(q.z), jsonNumber(q.w)});
}

// One parsed entity, before it is placed in the hierarchy.
struct Record {
    EntityId id = 0;
    std::optional<EntityId> parent;
    std::string name;
    Transform transform;
    std::optional<MeshRenderer> mesh;
    std::optional<DirectionalLight> light;
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

    record.transform.position = vec3(json, "position");
    const auto r = numbers<4>(field(json, "rotation"), "rotation");
    record.transform.rotation = {r[0], r[1], r[2], r[3]};
    if (bx::dot(record.transform.rotation, record.transform.rotation) < 1e-6f)
        throw Invalid("\"rotation\" must be a nonzero quaternion");
    record.transform.scale = vec3(json, "scale");

    if (json.contains("mesh")) {
        const Json& mesh = json["mesh"];
        const Json& primitive = field(mesh, "primitive");
        MeshRenderer renderer;
        bool known = false;
        for (const auto& [value, label] : kPrimitiveNames)
            if (primitive.is_string() && primitive.get<std::string>() == label) {
                renderer.primitive = value;
                known = true;
            }
        if (!known)
            throw Invalid(std::format("unknown mesh primitive {} (expected cube, plane or sphere)", primitive.dump()));
        renderer.color = vec3(mesh, "color");
        record.mesh = renderer;
    }
    if (json.contains("light")) {
        const Json& light = json["light"];
        record.light = DirectionalLight{vec3(light, "color"), number(field(light, "intensity"), "intensity")};
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
            scene.registry().replace<Transform>(entity, record.transform);
            if (record.mesh)
                scene.registry().emplace<MeshRenderer>(entity, *record.mesh);
            if (record.light)
                scene.registry().emplace<DirectionalLight>(entity, *record.light);
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

bool loadScene(const std::string& path, Scene& out, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::format("Cannot open scene '{}'", path);
        return false;
    }
    try {
        const Json json = Json::parse(file);
        const Json& version = field(json, "version");
        if (!version.is_number_integer() || version.get<int>() != kSceneFileVersion)
            throw Invalid(std::format("unsupported \"version\" {} (expected {})", version.dump(), kSceneFileVersion));

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
        out = std::move(scene);
        return true;
    } catch (const Json::exception& e) {
        error = std::format("Invalid scene '{}': {}", path, e.what());
    } catch (const Invalid& e) {
        error = std::format("Invalid scene '{}': {}", path, e.what());
    }
    return false;
}

bool saveScene(const Scene& scene, const std::string& path, std::string& error)
{
    const entt::registry& registry = scene.registry();
    Json entities = Json::array();
    scene.forEachDepthFirst([&](entt::entity entity) {
        const Transform& transform = registry.get<Transform>(entity);
        const entt::entity parent = scene.parent(entity);
        Json json;
        json["id"] = registry.get<PersistentId>(entity).value;
        json["name"] = registry.get<Name>(entity).value;
        json["parent"] = parent == entt::null ? Json(nullptr) : Json(registry.get<PersistentId>(parent).value);
        json["position"] = toJson(transform.position);
        json["rotation"] = toJson(transform.rotation);
        json["scale"] = toJson(transform.scale);
        if (const auto* mesh = registry.try_get<MeshRenderer>(entity)) {
            const char* primitive = "";
            for (const auto& [value, label] : kPrimitiveNames)
                if (value == mesh->primitive)
                    primitive = label;
            json["mesh"] = {{"primitive", primitive}, {"color", toJson(mesh->color)}};
        }
        if (const auto* light = registry.try_get<DirectionalLight>(entity))
            json["light"] = {{"color", toJson(light->color)}, {"intensity", jsonNumber(light->intensity)}};
        entities.push_back(std::move(json));
    });

    Json root;
    root["version"] = kSceneFileVersion;
    root["entities"] = std::move(entities);
    root["editor"] = scene.editorData;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << root.dump(2) << '\n';
    file.close();
    if (!file) {
        error = std::format("Cannot write scene '{}'", path);
        return false;
    }
    return true;
}

} // namespace kast
