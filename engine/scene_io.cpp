#include "scene_io.h"

#include "components.h"
#include "files.h"
#include "log.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <format>
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
    std::vector<std::pair<std::string, Json>> luaComponents; // canonical, after the drift rules
    std::map<std::string, Json> preserved;                   // unknown types, verbatim
};

Record parseEntity(const Json& json, SceneLoadReport& report)
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
    const std::string label = std::format("Entity '{}' (id {})", record.name, record.id);
    for (const auto& [key, value] : components.items()) {
        const ComponentType type = findAnyComponentType(key);
        if (type.meta())
            continue; // below, strictly
        if (type.lua()) {
            std::vector<std::string> fixes;
            record.luaComponents.emplace_back(key, migrateLuaComponent(*type.lua(), value, fixes));
            for (const std::string& fix : fixes)
                report.warnings.push_back(std::format("{}: {}: {}", label, key, fix));
            report.migrated = report.migrated || !fixes.empty();
        } else {
            record.preserved[key] = value;
            report.warnings.push_back(std::format("{}: unknown component \"{}\" (no built-in or valid Lua definition); "
                                                  "its data is kept as-is", label, key));
        }
    }
    if (!components.contains(entt::resolve<Transform>().name()))
        throw Invalid("missing \"Transform\" component");
    for (const entt::meta_type& type : componentTypes()) {
        if (!components.contains(type.name()))
            continue;
        std::string error;
        Json value = components[type.name()];
        // MeshRenderer "color" predates materials: dropped, so older scenes still load.
        if (type == entt::resolve<MeshRenderer>() && value.is_object() && value.contains("color")) {
            value.erase("color");
            if (!value.contains("material"))
                value["material"] = "";
            report.warnings.push_back(std::format("{}: MeshRenderer: \"color\" is no longer supported (use a material); "
                                                  "ignored", label));
        }
        entt::meta_any component = componentFromJson(type, value, error);
        if (!component)
            throw Invalid(std::format("\"{}\": {}", type.name(), error));
        record.components.push_back(std::move(component));
    }
    return record;
}

// A finite JSON number >= 0 and, if unit, <= 1.
float lightingNumber(const Json& value, const char* what, bool unit)
{
    const double v = value.is_number() ? value.get<double>() : -1.0;
    if (!(v >= 0.0 && v <= (unit ? 1.0 : FLT_MAX)))
        throw Invalid(std::format("lighting: \"{}\" must be {}", what, unit ? "numbers between 0 and 1" : "a number >= 0"));
    return static_cast<float>(v);
}

SceneLighting parseLighting(const Json& json)
{
    if (!json.is_object())
        throw Invalid("\"lighting\" must be an object");
    for (const auto& [key, value] : json.items())
        if (key != "ambientColor" && key != "ambientIntensity")
            throw Invalid(std::format("lighting: unknown field \"{}\"", key));
    SceneLighting lighting;
    const Json& color = field(json, "ambientColor");
    if (!color.is_array() || color.size() != 3)
        throw Invalid("lighting: \"ambientColor\" must be an array of 3 numbers");
    lighting.ambientColor = {lightingNumber(color[0], "ambientColor", true), lightingNumber(color[1], "ambientColor", true),
                             lightingNumber(color[2], "ambientColor", true)};
    lighting.ambientIntensity = lightingNumber(field(json, "ambientIntensity"), "ambientIntensity", false);
    return lighting;
}

// "postProcessing": every block and field optional (missing = default), but present ones must be valid.
class PostReader {
public:
    PostReader(const Json& block, const char* name) : block_(block), name_(name)
    {
        if (!block.is_object())
            throw Invalid(std::format("postProcessing: \"{}\" must be an object", name));
    }
    void fields(std::initializer_list<std::string_view> known) const
    {
        for (const auto& [key, value] : block_.items())
            if (std::ranges::find(known, std::string_view(key)) == known.end())
                throw Invalid(std::format("postProcessing.{}: unknown field \"{}\"", name_, key));
    }
    void flag(const char* key, bool& out) const
    {
        if (!block_.contains(key))
            return;
        if (!block_[key].is_boolean())
            throw Invalid(std::format("postProcessing.{}: \"{}\" must be true or false", name_, key));
        out = block_[key].get<bool>();
    }
    void number(const char* key, float& out, float lo, float hi) const
    {
        if (block_.contains(key))
            out = checked(block_[key], key, lo, hi);
    }
    void color(const char* key, bx::Vec3& out) const
    {
        if (!block_.contains(key))
            return;
        const Json& c = block_[key];
        if (!c.is_array() || c.size() != 3)
            throw Invalid(std::format("postProcessing.{}: \"{}\" must be an array of 3 numbers", name_, key));
        out = {checked(c[0], key, 0.0f, 1.0f), checked(c[1], key, 0.0f, 1.0f), checked(c[2], key, 0.0f, 1.0f)};
    }
    void tonemap(const char* key, Tonemap& out) const
    {
        if (!block_.contains(key))
            return;
        const Json& v = block_[key];
        if (v == "None")
            out = Tonemap::None;
        else if (v == "ACES")
            out = Tonemap::Aces;
        else
            throw Invalid(std::format("postProcessing.{}: \"{}\" must be \"None\" or \"ACES\"", name_, key));
    }

private:
    float checked(const Json& value, const char* key, float lo, float hi) const
    {
        const double v = value.is_number() ? value.get<double>() : std::nan("");
        if (!(v >= lo && v <= hi)) // NaN fails too
            throw Invalid(hi == FLT_MAX ? std::format("postProcessing.{}: \"{}\" must be a number >= {}", name_, key, lo)
                                        : std::format("postProcessing.{}: \"{}\" must be a number between {} and {}",
                                                      name_, key, lo, hi));
        return static_cast<float>(v);
    }

    const Json& block_;
    const char* name_;
};

ScenePostProcessing parsePostProcessing(const Json& json)
{
    if (!json.is_object())
        throw Invalid("\"postProcessing\" must be an object");
    ScenePostProcessing post;
    const auto block = [&](const char* name) { return json.contains(name) ? &json[name] : nullptr; };
    for (const auto& [key, value] : json.items())
        if (key != "tonemapping" && key != "bloom" && key != "colorGrading" && key != "vignette" && key != "fxaa" &&
            key != "fog")
            throw Invalid(std::format("postProcessing: unknown field \"{}\"", key));
    if (const Json* j = block("tonemapping")) {
        const PostReader r(*j, "tonemapping");
        r.fields({"enabled", "exposure", "operator"});
        r.flag("enabled", post.tonemapping.enabled);
        r.number("exposure", post.tonemapping.exposure, -10.0f, 10.0f);
        r.tonemap("operator", post.tonemapping.op);
    }
    if (const Json* j = block("bloom")) {
        const PostReader r(*j, "bloom");
        r.fields({"enabled", "threshold", "intensity", "radius"});
        r.flag("enabled", post.bloom.enabled);
        r.number("threshold", post.bloom.threshold, 0.0f, FLT_MAX);
        r.number("intensity", post.bloom.intensity, 0.0f, FLT_MAX);
        r.number("radius", post.bloom.radius, 0.0f, 1.0f);
    }
    if (const Json* j = block("colorGrading")) {
        const PostReader r(*j, "colorGrading");
        r.fields({"enabled", "contrast", "saturation", "colorFilter", "temperature"});
        r.flag("enabled", post.colorGrading.enabled);
        r.number("contrast", post.colorGrading.contrast, 0.0f, 2.0f);
        r.number("saturation", post.colorGrading.saturation, 0.0f, 2.0f);
        r.color("colorFilter", post.colorGrading.colorFilter);
        r.number("temperature", post.colorGrading.temperature, -1.0f, 1.0f);
    }
    if (const Json* j = block("vignette")) {
        const PostReader r(*j, "vignette");
        r.fields({"enabled", "intensity", "smoothness"});
        r.flag("enabled", post.vignette.enabled);
        r.number("intensity", post.vignette.intensity, 0.0f, 1.0f);
        r.number("smoothness", post.vignette.smoothness, 0.01f, 1.0f);
    }
    if (const Json* j = block("fxaa")) {
        const PostReader r(*j, "fxaa");
        r.fields({"enabled"});
        r.flag("enabled", post.fxaa.enabled);
    }
    if (const Json* j = block("fog")) {
        const PostReader r(*j, "fog");
        r.fields({"enabled", "color", "density", "start"});
        r.flag("enabled", post.fog.enabled);
        r.color("color", post.fog.color);
        r.number("density", post.fog.density, 0.0f, FLT_MAX);
        r.number("start", post.fog.start, 0.0f, FLT_MAX);
    }
    return post;
}

constexpr double kMaxGravity = 1000.0; // m/s^2 per axis

// "physics": every field optional (missing = default), but present ones must be valid.
ScenePhysics parsePhysics(const Json& json)
{
    if (!json.is_object())
        throw Invalid("\"physics\" must be an object");
    for (const auto& [key, value] : json.items())
        if (key != "gravity")
            throw Invalid(std::format("physics: unknown field \"{}\"", key));
    ScenePhysics physics;
    if (json.contains("gravity")) {
        const Json& g = json["gravity"];
        const auto axis = [&](size_t i) {
            const double v = g[i].is_number() ? g[i].get<double>() : NAN;
            if (!(v >= -kMaxGravity && v <= kMaxGravity))
                throw Invalid(std::format("physics: \"gravity\" must be 3 numbers between {} and {}", -kMaxGravity, kMaxGravity));
            return static_cast<float>(v);
        };
        if (!g.is_array() || g.size() != 3)
            throw Invalid("physics: \"gravity\" must be an array of 3 numbers");
        physics.gravity = {axis(0), axis(1), axis(2)};
    }
    return physics;
}

Json postProcessingToJson(const ScenePostProcessing& post)
{
    const auto color = [](const bx::Vec3& c) { return Json::array({jsonNumber(c.x), jsonNumber(c.y), jsonNumber(c.z)}); };
    const auto& t = post.tonemapping;
    const auto& b = post.bloom;
    const auto& g = post.colorGrading;
    const auto& v = post.vignette;
    const auto& f = post.fog;
    return {{"tonemapping", {{"enabled", t.enabled}, {"exposure", jsonNumber(t.exposure)},
                             {"operator", t.op == Tonemap::Aces ? "ACES" : "None"}}},
            {"bloom", {{"enabled", b.enabled}, {"threshold", jsonNumber(b.threshold)},
                       {"intensity", jsonNumber(b.intensity)}, {"radius", jsonNumber(b.radius)}}},
            {"colorGrading", {{"enabled", g.enabled}, {"contrast", jsonNumber(g.contrast)},
                              {"saturation", jsonNumber(g.saturation)}, {"colorFilter", color(g.colorFilter)},
                              {"temperature", jsonNumber(g.temperature)}}},
            {"vignette", {{"enabled", v.enabled}, {"intensity", jsonNumber(v.intensity)},
                          {"smoothness", jsonNumber(v.smoothness)}}},
            {"fxaa", {{"enabled", post.fxaa.enabled}}},
            {"fog", {{"enabled", f.enabled}, {"color", color(f.color)}, {"density", jsonNumber(f.density)},
                     {"start", jsonNumber(f.start)}}}};
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
            if (entity == entt::null) // also bounds this recursion
                throw Invalid(std::format("entity hierarchy is deeper than {} levels", kMaxHierarchyDepth));
            for (const entt::meta_any& component : record.components)
                setComponent(scene.registry(), entity, component);
            for (const auto& [name, value] : record.luaComponents)
                scene.registry().storage<LuaComponentData>(luaStorageId(name)).emplace(entity, LuaComponentData{value});
            if (!record.preserved.empty())
                scene.registry().emplace<PreservedComponents>(entity, record.preserved);
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

bool sceneFromJson(const Json& json, Scene& out, std::string& error, SceneLoadReport* report)
{
    SceneLoadReport local;
    try {
        const Json& version = field(json, "version");
        if (!version.is_number_integer() || version.get<int>() != kSceneFileVersion)
            throw Invalid(std::format("unsupported \"version\" {} (expected {}; older scenes are not migrated)",
                                      version.dump(), kSceneFileVersion));

        const Json& entities = field(json, "entities");
        if (!entities.is_array())
            throw Invalid("\"entities\" must be an array");
        std::vector<Record> records;
        for (size_t i = 0; i < entities.size(); ++i) {
            try {
                records.push_back(parseEntity(entities[i], local));
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
        if (json.contains("lighting"))
            scene.lighting = parseLighting(json["lighting"]);
        if (json.contains("postProcessing"))
            scene.postProcessing = parsePostProcessing(json["postProcessing"]);
        if (json.contains("physics"))
            scene.physics = parsePhysics(json["physics"]);
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
        if (report)
            *report = std::move(local);
        return true;
    } catch (const Json::exception& e) {
        error = e.what();
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

bool loadScene(const std::string& path, Scene& out, std::string& error, SceneLoadReport* report)
{
    std::string text;
    if (!readFile(path, text)) {
        error = std::format("Cannot open scene '{}'", path);
        return false;
    }
    std::string reason;
    try {
        SceneLoadReport local;
        if (sceneFromJson(Json::parse(text), out, reason, &local)) {
            for (const std::string& warning : local.warnings)
                log::warn(std::format("Scene '{}': {}", path, warning));
            if (report)
                *report = std::move(local);
            return true;
        }
    } catch (const Json::exception& e) {
        reason = e.what();
    }
    error = std::format("Invalid scene '{}': {}", path, reason);
    return false;
}

Json sceneToJson(const Scene& scene)
{
    const entt::registry& registry = scene.registry();
    const std::vector<ComponentType> types = allComponentTypes();
    Json entities = Json::array();
    scene.forEachDepthFirst([&](entt::entity entity) {
        const entt::entity parent = scene.parent(entity);
        Json json;
        json["id"] = registry.get<PersistentId>(entity).value;
        json["name"] = registry.get<Name>(entity).value;
        json["parent"] = parent == entt::null ? Json(nullptr) : Json(registry.get<PersistentId>(parent).value);
        Json& components = json["components"] = Json::object();
        for (const ComponentType& type : types)
            if (hasComponent(registry, entity, type))
                components[type.name()] = componentToJson(registry, entity, type);
        if (const auto* preserved = registry.try_get<PreservedComponents>(entity))
            for (const auto& [name, value] : preserved->byName)
                components[name] = value;
        entities.push_back(std::move(json));
    });

    Json root;
    root["version"] = kSceneFileVersion;
    root["entities"] = std::move(entities);
    root["systems"] = scene.systems;
    const SceneLighting& lighting = scene.lighting;
    root["lighting"] = {{"ambientColor", Json::array({jsonNumber(lighting.ambientColor.x), jsonNumber(lighting.ambientColor.y),
                                                      jsonNumber(lighting.ambientColor.z)})},
                        {"ambientIntensity", jsonNumber(lighting.ambientIntensity)}};
    root["postProcessing"] = postProcessingToJson(scene.postProcessing);
    const bx::Vec3& gravity = scene.physics.gravity;
    root["physics"] = {{"gravity", Json::array({jsonNumber(gravity.x), jsonNumber(gravity.y), jsonNumber(gravity.z)})}};
    root["editor"] = scene.editorData;
    return root;
}

bool saveScene(const Scene& scene, const std::string& path, std::string& error)
{
    if (!writeFileAtomic(path, sceneToJson(scene).dump(2) + '\n')) {
        error = std::format("Cannot write scene '{}'", path);
        return false;
    }
    return true;
}

} // namespace kast
