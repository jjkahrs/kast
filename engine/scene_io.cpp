#include "scene_io.h"
#include "asset_ref.h"

#include "components.h"
#include "files.h"
#include "log.h"
#include "project.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
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
    if (value.get<EntityId>() > kMaxFileEntityId) // the high bits name the scene in memory (scene.h scopedId)
        throw Invalid(std::format("\"{}\" must be at most {}", what, kMaxFileEntityId));
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
    std::vector<PrefabHop> hops;             // part of a prefab instance (PrefabLink)
    std::optional<PrefabInstance> instance;  // an instance root or placeholder
    EntityId placeholderParent = 0;          // PlaceholderChild
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

// "lighting" > "lightmaps": every field optional (missing = default), present ones checked.
SceneLightmaps parseLightmaps(const Json& json)
{
    if (!json.is_object())
        throw Invalid("lighting: \"lightmaps\" must be an object");
    SceneLightmaps out;
    const auto number = [&](const char* key, float lo, float hi) {
        const double v = json[key].is_number() ? json[key].get<double>() : std::nan("");
        if (!(v >= lo && v <= hi))
            throw Invalid(hi == FLT_MAX ? std::format("lighting.lightmaps: \"{}\" must be a number >= {}", key, lo)
                                        : std::format("lighting.lightmaps: \"{}\" must be a number between {} and {}",
                                                      key, lo, hi));
        return static_cast<float>(v);
    };
    const auto whole = [&](const char* key, int lo, int hi) {
        const float v = number(key, float(lo), float(hi));
        if (v != std::floor(v))
            throw Invalid(std::format("lighting.lightmaps: \"{}\" must be a whole number", key));
        return static_cast<int>(v);
    };
    const auto flag = [&](const char* key) {
        if (!json[key].is_boolean())
            throw Invalid(std::format("lighting.lightmaps: \"{}\" must be true or false", key));
        return json[key].get<bool>();
    };
    for (const auto& [key, value] : json.items()) {
        if (key == "texelsPerMeter")
            out.texelsPerMeter = number("texelsPerMeter", kMinLightmapTexelsPerMeter, kMaxLightmapTexelsPerMeter);
        else if (key == "bounces")
            out.bounces = whole("bounces", 0, kMaxLightmapBounces);
        else if (key == "samplesPerTexel")
            out.samplesPerTexel = whole("samplesPerTexel", 1, kMaxLightmapSamples);
        else if (key == "probeSpacing")
            out.probeSpacing = number("probeSpacing", kMinProbeSpacing, kMaxProbeSpacing);
        else if (key == "enabled")
            out.enabled = flag("enabled");
        else if (key == "intensity")
            out.intensity = number("intensity", 0.0f, FLT_MAX);
        else if (key == "directional")
            out.directional = flag("directional");
        else if (key == "bakedPath" && value.is_string())
            out.bakedPath = value.get<std::string>();
        else if (key == "bakedPath")
            throw Invalid("lighting.lightmaps: \"bakedPath\" must be a string");
        else
            throw Invalid(std::format("lighting.lightmaps: unknown field \"{}\"", key));
    }
    return out;
}

// "lighting" > "sdfgi": every field optional (missing = default), present ones checked against the ranges in scene.h.
SceneSdfgi parseSdfgi(const Json& json)
{
    if (!json.is_object())
        throw Invalid("lighting: \"sdfgi\" must be an object");
    SceneSdfgi out;
    const auto number = [&](const char* key, float lo, float hi) {
        const double v = json[key].is_number() ? json[key].get<double>() : std::nan("");
        if (!(v >= lo && v <= hi))
            throw Invalid(hi == FLT_MAX ? std::format("lighting.sdfgi: \"{}\" must be a number >= {}", key, lo)
                                        : std::format("lighting.sdfgi: \"{}\" must be a number between {} and {}", key,
                                                      lo, hi));
        return static_cast<float>(v);
    };
    for (const auto& [key, value] : json.items()) {
        if (key == "enabled" && value.is_boolean()) {
            out.enabled = value.get<bool>();
        } else if (key == "enabled") {
            throw Invalid("lighting.sdfgi: \"enabled\" must be true or false");
        } else if (key == "cascades") {
            const float v = number("cascades", 1.0f, float(kMaxSdfgiCascades));
            if (v != std::floor(v))
                throw Invalid("lighting.sdfgi: \"cascades\" must be a whole number");
            out.cascades = static_cast<int>(v);
        } else if (key == "cellSize") {
            out.cellSize = number("cellSize", kMinSdfgiCellSize, kMaxSdfgiCellSize);
        } else if (key == "energy") {
            out.energy = number("energy", 0.0f, FLT_MAX);
        } else if (key == "bounceFeedback") {
            out.bounceFeedback = number("bounceFeedback", 0.0f, 1.0f);
        } else if (key == "quality") {
            constexpr const char* kQualities[] = {"low", "medium", "high"}; // SdfgiQuality order
            const auto* it = value.is_string() ? std::ranges::find(kQualities, value.get<std::string>()) : std::end(kQualities);
            if (it == std::end(kQualities))
                throw Invalid("lighting.sdfgi: \"quality\" must be \"low\", \"medium\" or \"high\"");
            out.quality = static_cast<SdfgiQuality>(it - std::begin(kQualities));
        } else if (key == "yScale") {
            const double v = value.is_number() ? value.get<double>() : std::nan("");
            if (std::ranges::find(kSdfgiYScales, float(v)) == std::end(kSdfgiYScales))
                throw Invalid("lighting.sdfgi: \"yScale\" must be 1, 0.5 or 0.25");
            out.yScale = float(v);
        } else {
            throw Invalid(std::format("lighting.sdfgi: unknown field \"{}\"", key));
        }
    }
    return out;
}

SceneLighting parseLighting(const Json& json)
{
    if (!json.is_object())
        throw Invalid("\"lighting\" must be an object");
    for (const auto& [key, value] : json.items())
        if (key != "ambientColor" && key != "ambientIntensity" && key != "lightmaps" && key != "sdfgi")
            throw Invalid(std::format("lighting: unknown field \"{}\"", key));
    SceneLighting lighting;
    if (json.contains("lightmaps"))
        lighting.lightmaps = parseLightmaps(json["lightmaps"]);
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
    void integer(const char* key, int& out, int lo, int hi) const
    {
        if (!block_.contains(key))
            return;
        const Json& v = block_[key];
        if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi)
            throw Invalid(std::format("postProcessing.{}: \"{}\" must be an integer between {} and {}", name_, key, lo, hi));
        out = v.get<int>();
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
            key != "fog" && key != "ssao" && key != "ssr" && key != "depthOfField" && key != "volumetricFog")
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
    if (const Json* j = block("ssao")) {
        const PostReader r(*j, "ssao");
        r.fields({"enabled", "radius", "intensity", "power", "sliceCount"});
        r.flag("enabled", post.ssao.enabled);
        r.number("radius", post.ssao.radius, 0.05f, 5.0f);
        r.number("intensity", post.ssao.intensity, 0.0f, 4.0f);
        r.number("power", post.ssao.power, 0.5f, 4.0f);
        r.integer("sliceCount", post.ssao.sliceCount, 1, 4);
    }
    if (const Json* j = block("ssr")) {
        const PostReader r(*j, "ssr");
        r.fields({"enabled", "maxDistance", "thickness", "maxRoughness", "intensity", "steps"});
        r.flag("enabled", post.ssr.enabled);
        r.number("maxDistance", post.ssr.maxDistance, 0.5f, 1000.0f);
        r.number("thickness", post.ssr.thickness, 0.01f, 5.0f);
        r.number("maxRoughness", post.ssr.maxRoughness, 0.0f, 1.0f);
        r.number("intensity", post.ssr.intensity, 0.0f, 1.0f);
        r.integer("steps", post.ssr.steps, 8, 128);
    }
    if (const Json* j = block("depthOfField")) {
        const PostReader r(*j, "depthOfField");
        r.fields({"enabled", "focusDistance", "fStop", "focalLength", "maxCoc"});
        r.flag("enabled", post.depthOfField.enabled);
        r.number("focusDistance", post.depthOfField.focusDistance, 0.1f, 10000.0f);
        r.number("fStop", post.depthOfField.fStop, 0.7f, 32.0f);
        r.number("focalLength", post.depthOfField.focalLength, 8.0f, 400.0f);
        r.number("maxCoc", post.depthOfField.maxCoc, 1.0f, 64.0f);
    }
    if (const Json* j = block("volumetricFog")) {
        const PostReader r(*j, "volumetricFog");
        r.fields({"enabled", "density", "albedo", "anisotropy", "heightFalloff", "baseHeight", "maxDistance",
                  "temporalBlend", "lightIntensity", "ambientIntensity"});
        auto& v = post.volumetricFog;
        r.flag("enabled", v.enabled);
        r.number("density", v.density, 0.0f, FLT_MAX);
        r.color("albedo", v.albedo);
        r.number("anisotropy", v.anisotropy, -0.9f, 0.9f);
        r.number("heightFalloff", v.heightFalloff, 0.0f, FLT_MAX);
        r.number("baseHeight", v.baseHeight, -10000.0f, 10000.0f);
        r.number("maxDistance", v.maxDistance, 1.0f, 1000.0f);
        r.number("temporalBlend", v.temporalBlend, 0.0f, 0.99f);
        r.number("lightIntensity", v.lightIntensity, 0.0f, FLT_MAX);
        r.number("ambientIntensity", v.ambientIntensity, 0.0f, FLT_MAX);
    }
    return post;
}

// "sky": every field optional (missing = default), but present ones must be valid. Face paths are not checked here: a
// face that doesn't load falls back to the procedural sky at render time.
SceneSky parseSky(const Json& json)
{
    if (!json.is_object())
        throw Invalid("\"sky\" must be an object");
    SceneSky sky;
    for (const auto& [key, value] : json.items()) {
        if (key == "mode") {
            if (value == "off")
                sky.mode = SkyMode::Off;
            else if (value == "procedural")
                sky.mode = SkyMode::Procedural;
            else if (value == "cubemap")
                sky.mode = SkyMode::Cubemap;
            else
                throw Invalid("sky: \"mode\" must be \"off\", \"procedural\" or \"cubemap\"");
        } else if (key == "top" || key == "horizon" || key == "bottom") {
            const auto channel = [&](const Json& c) {
                const double v = c.is_number() ? c.get<double>() : NAN;
                if (!(v >= 0.0 && v <= 1.0))
                    throw Invalid(std::format("sky: \"{}\" must be an array of 3 numbers between 0 and 1", key));
                return static_cast<float>(v);
            };
            if (!value.is_array() || value.size() != 3)
                throw Invalid(std::format("sky: \"{}\" must be an array of 3 numbers between 0 and 1", key));
            (key == "top" ? sky.top : key == "horizon" ? sky.horizon : sky.bottom) =
                bx::Vec3{channel(value[0]), channel(value[1]), channel(value[2])};
        } else if (key == "faces") {
            if (!value.is_object())
                throw Invalid("sky: \"faces\" must be an object");
            for (const auto& [face, path] : value.items()) {
                const auto it = std::ranges::find(kSkyFaceNames, std::string_view(face));
                if (it == std::end(kSkyFaceNames))
                    throw Invalid(std::format("sky.faces: unknown face \"{}\" (px, nx, py, ny, pz or nz)", face));
                if (!path.is_string())
                    throw Invalid(std::format("sky.faces: \"{}\" must be a string", face));
                sky.faces[it - std::begin(kSkyFaceNames)] = path.get<std::string>();
            }
        } else if (key == "cube") {
            if (!value.is_string())
                throw Invalid("sky: \"cube\" must be a string");
            sky.cube = value.get<std::string>();
        } else {
            throw Invalid(std::format("sky: unknown field \"{}\"", key));
        }
    }
    return sky;
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
    const auto& ao = post.ssao;
    const auto& r = post.ssr;
    const auto& d = post.depthOfField;
    const auto& vf = post.volumetricFog;
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
                     {"start", jsonNumber(f.start)}}},
            {"ssao", {{"enabled", ao.enabled}, {"radius", jsonNumber(ao.radius)}, {"intensity", jsonNumber(ao.intensity)},
                      {"power", jsonNumber(ao.power)}, {"sliceCount", ao.sliceCount}}},
            {"ssr", {{"enabled", r.enabled}, {"maxDistance", jsonNumber(r.maxDistance)},
                     {"thickness", jsonNumber(r.thickness)}, {"maxRoughness", jsonNumber(r.maxRoughness)},
                     {"intensity", jsonNumber(r.intensity)}, {"steps", r.steps}}},
            {"depthOfField", {{"enabled", d.enabled}, {"focusDistance", jsonNumber(d.focusDistance)},
                              {"fStop", jsonNumber(d.fStop)}, {"focalLength", jsonNumber(d.focalLength)},
                              {"maxCoc", jsonNumber(d.maxCoc)}}},
            {"volumetricFog", {{"enabled", vf.enabled}, {"density", jsonNumber(vf.density)}, {"albedo", color(vf.albedo)},
                               {"anisotropy", jsonNumber(vf.anisotropy)}, {"heightFalloff", jsonNumber(vf.heightFalloff)},
                               {"baseHeight", jsonNumber(vf.baseHeight)}, {"maxDistance", jsonNumber(vf.maxDistance)},
                               {"temporalBlend", jsonNumber(vf.temporalBlend)},
                               {"lightIntensity", jsonNumber(vf.lightIntensity)},
                               {"ambientIntensity", jsonNumber(vf.ambientIntensity)}}}};
}

// Roots go under attach (entt::null: scene roots).
void build(const std::vector<Record>& records, Scene& scene, entt::entity attach = entt::null)
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
            if (!record.hops.empty())
                scene.registry().emplace<PrefabLink>(entity, record.hops);
            if (record.instance)
                scene.registry().emplace<PrefabInstance>(entity, *record.instance);
            if (record.placeholderParent)
                scene.registry().emplace<PlaceholderChild>(entity, record.placeholderParent);
            ++created;
            self(self, record.id, entity);
        }
    };
    place(place, std::nullopt, attach);
    // Every entity whose parent chain ends at a root was placed; the rest are in a parent cycle.
    if (created != records.size())
        throw Invalid("entity parent references form a cycle");
}

// Prefabs loaded while one scene or prefab loads: each file once, and the chain being loaded to refuse cycles.
struct LoadContext {
    std::filesystem::path root; // empty: no project, no prefab loads
    std::vector<std::string> loading; // outermost first
    std::map<std::string, std::shared_ptr<const PrefabAsset>> loaded;
    std::map<std::string, std::string> failed; // why
};

void parseScene(const Json& json, Scene& out, LoadContext& context, SceneLoadReport& report, bool prefab);

std::shared_ptr<const PrefabAsset> snapshot(const Scene& scene, const std::string& path)
{
    auto asset = std::make_shared<PrefabAsset>();
    asset->path = path;
    const entt::registry& registry = scene.registry();
    scene.forEachDepthFirst([&](entt::entity e) {
        PrefabAsset::Entity entity;
        entity.id = registry.get<PersistentId>(e).value;
        const entt::entity parent = scene.parent(e);
        entity.parent = parent == entt::null ? 0 : registry.get<PersistentId>(parent).value;
        entity.name = registry.get<Name>(e).value;
        entity.components = entityComponentsToJson(registry, e);
        if (const auto* link = registry.try_get<PrefabLink>(e))
            entity.hops = link->hops;
        if (const auto* instance = registry.try_get<PrefabInstance>(e)) {
            if (instance->asset)
                asset->nested[entity.id] = instance->asset;
            else
                entity.missingPrefab = instance->path;
        }
        asset->entities.push_back(std::move(entity));
    });
    return asset;
}

// Reads and parses a prefab file (the chain in context.loading already holds path).
void readPrefab(LoadContext& context, const std::string& path, Scene& out, SceneLoadReport& report)
{
    if (context.root.empty())
        throw Invalid("no project is open");
    std::string reason;
    const std::optional<std::filesystem::path> file = resolveProjectPath(context.root, path, reason);
    if (!file)
        throw Invalid(reason);
    std::string text;
    if (!readFile(*file, text))
        throw Invalid("cannot open the file");
    Json json = Json::parse(text);
    readAssetRefs(json);
    parseScene(json, out, context, report, true);
}

// The prefab at path, loaded once per context; null + why if it doesn't load. A nested prefab's warnings are added
// to report, prefixed with its path.
std::shared_ptr<const PrefabAsset> resolvePrefab(LoadContext& context, const std::string& path, std::string& why,
                                                 SceneLoadReport& outer)
{
    if (const auto it = context.loaded.find(path); it != context.loaded.end())
        return it->second;
    if (const auto it = context.failed.find(path); it != context.failed.end()) {
        why = it->second;
        return nullptr;
    }
    if (std::ranges::find(context.loading, path) != context.loading.end()) {
        why = "it would contain itself:";
        for (const std::string& link : context.loading)
            why += std::format(" '{}' >", link);
        why += std::format(" '{}'", path);
        return nullptr; // not cached: it depends on the chain
    }
    SceneLoadReport report;
    Scene scene;
    context.loading.push_back(path);
    try {
        readPrefab(context, path, scene, report);
    } catch (const Json::exception& e) {
        why = e.what();
    } catch (const Invalid& e) {
        why = e.what();
    }
    context.loading.pop_back();
    if (!why.empty()) {
        context.failed[path] = why;
        return nullptr;
    }
    for (const std::string& warning : report.warnings)
        outer.warnings.push_back(std::format("prefab '{}': {}", path, warning));
    std::shared_ptr<const PrefabAsset> asset = snapshot(scene, path);
    context.loaded[path] = asset;
    return asset;
}

// A built-in component's overridden value is still valid (fields check each other, e.g. Collider height >= 2 * radius,
// so a prefab change can invalidate an override that was fine). If not, a warning: the prefab's value is used, so a
// prefab edit never stops scenes from loading. Lua components get the drift rules later instead.
bool keepsValid(const std::string& type, const Json& value, const std::string& label, SceneLoadReport& report)
{
    const entt::meta_type meta = findComponentType(type);
    std::string error;
    if (!meta || componentFromJson(meta, value, error))
        return true;
    report.warnings.push_back(std::format("{}: override of \"{}\" dropped ({}); the prefab's value is used", label,
                                          type, error));
    return false;
}

// An instance entry's overrides applied to the prefab entity's name and components.
void applyEntry(const Json& entry, std::string& name, Json& components, const std::string& label, SceneLoadReport& report)
{
    if (entry.contains("name")) {
        if (!entry["name"].is_string())
            throw Invalid("instance: \"name\" must be a string");
        name = entry["name"].get<std::string>();
    }
    if (entry.contains("removed")) {
        if (!entry["removed"].is_array())
            throw Invalid("instance: \"removed\" must be an array of component names");
        for (const Json& removed : entry["removed"]) {
            if (!removed.is_string())
                throw Invalid("instance: \"removed\" must be an array of component names");
            components.erase(removed.get<std::string>());
        }
    }
    if (entry.contains("components")) {
        if (!entry["components"].is_object())
            throw Invalid("instance: \"components\" must be an object");
        for (const auto& [key, fields] : entry["components"].items()) {
            if (!fields.is_object())
                throw Invalid(std::format("instance: \"components\": \"{}\" must be an object of fields", key));
            if (!components.contains(key) || !components[key].is_object()) {
                report.warnings.push_back(std::format("{}: override of \"{}\", which its prefab no longer has, ignored",
                                                      label, key));
                continue;
            }
            Json merged = components[key];
            for (const auto& [fieldName, value] : fields.items())
                merged[fieldName] = value;
            if (keepsValid(key, merged, label, report))
                components[key] = std::move(merged);
        }
    }
    if (entry.contains("added")) {
        if (!entry["added"].is_object())
            throw Invalid("instance: \"added\" must be an object");
        for (const auto& [key, value] : entry["added"].items())
            if (keepsValid(key, value, label, report))
                components[key] = value;
    }
}

// Records for an instance of asset with root id rootId: the prefab's entities with entries' overrides (by source; the
// root's entry is rootEntry) and ids (else fresh ones). entries null: a new instance, no overrides.
void expandAsset(const std::shared_ptr<const PrefabAsset>& asset, EntityId rootId, const Json& parent,
                 const std::string& rootName, const std::map<EntityId, const Json*>& entries, const Json* rootEntry,
                 const std::function<EntityId()>& fresh, std::vector<Record>& records, SceneLoadReport& report)
{
    std::map<EntityId, EntityId> sceneIds; // prefab-local -> scene
    std::set<EntityId> skipped;            // deleted, with their subtrees
    for (const PrefabAsset::Entity& base : asset->entities) {
        const bool root = &base == &asset->entities.front();
        const Json* entry = root ? rootEntry : nullptr;
        if (const auto it = entries.find(base.id); !root && it != entries.end() && it->second != rootEntry)
            entry = it->second;
        if (!root && (skipped.contains(base.parent) || (entry && entry->contains("deleted")))) {
            skipped.insert(base.id);
            continue;
        }
        const EntityId id = root ? rootId : entry ? (*entry)["id"].get<EntityId>() : fresh();
        sceneIds[base.id] = id;
        std::string name = root ? rootName : base.name;
        Json components = base.components;
        if (entry)
            applyEntry(*entry, name, components, std::format("Entity '{}' (id {})", name, id), report);
        Record record = parseEntity(Json{{"id", id}, {"name", name},
                                         {"parent", root ? parent : Json(sceneIds.at(base.parent))},
                                         {"components", std::move(components)}},
                                    report);
        record.hops.push_back({rootId, base.id});
        for (const PrefabHop& hop : base.hops)
            record.hops.push_back({sceneIds.at(hop.root), hop.local});
        if (root)
            record.instance = PrefabInstance{asset->path, asset, nullptr};
        else if (const auto nested = asset->nested.find(base.id); nested != asset->nested.end())
            record.instance = PrefabInstance{nested->second->path, nested->second, nullptr};
        else if (!base.missingPrefab.empty())
            record.instance = PrefabInstance{base.missingPrefab, nullptr, nullptr};
        records.push_back(std::move(record));
    }
}

// An instance record ("prefab" + "instance" entries): its entities' records, or a placeholder whose entry ids become
// aliases of it (children saved under them load under the placeholder).
void parseInstance(const Json& json, LoadContext& context, const std::function<EntityId()>& fresh,
                   std::vector<Record>& records, std::map<EntityId, EntityId>& aliases, SceneLoadReport& report)
{
    const EntityId id = entityId(field(json, "id"), "id");
    const Json& name = field(json, "name");
    if (!name.is_string())
        throw Invalid("\"name\" must be a string");
    const Json& parent = field(json, "parent");
    if (!parent.is_null())
        entityId(parent, "parent");
    const Json& prefab = field(json, "prefab");
    if (!prefab.is_string() || prefab.get<std::string>().empty())
        throw Invalid("\"prefab\" must be a non-empty string");
    const std::string path = prefab.get<std::string>();
    const Json& entries = field(json, "instance");
    if (!entries.is_array())
        throw Invalid("\"instance\" must be an array");

    std::map<EntityId, const Json*> bySource;
    const Json* rootEntry = nullptr; // the one with the record's id
    for (const Json& entry : entries) {
        if (!entry.is_object())
            throw Invalid("\"instance\" entries must be objects");
        const bool deleted = entry.contains("deleted");
        if (deleted && entry["deleted"] != true)
            throw Invalid("instance: \"deleted\" must be true");
        const bool isRoot = !deleted && entityId(field(entry, "id"), "id") == id;
        if (isRoot)
            rootEntry = &entry;
        if (!isRoot || entry.contains("source")) {
            const EntityId source = entityId(field(entry, "source"), "source");
            if (!bySource.emplace(source, &entry).second)
                throw Invalid(std::format("instance: two entries for source {}", source));
        }
    }

    const std::string label = std::format("Entity '{}' (id {})", name.get<std::string>(), id);
    std::string why;
    if (const std::shared_ptr<const PrefabAsset> asset = resolvePrefab(context, path, why, report)) {
        expandAsset(asset, id, parent, name.get<std::string>(), bySource, rootEntry, fresh, records, report);
        return;
    }
    report.warnings.push_back(std::format("{}: prefab '{}' did not load ({}); kept as a placeholder", label, path, why));
    Json transform = componentToJson(entt::meta_any(Transform{}));
    if (rootEntry && rootEntry->contains("components") && (*rootEntry)["components"].contains("Transform"))
        transform = (*rootEntry)["components"]["Transform"];
    Record record = parseEntity(Json{{"id", id}, {"name", std::filesystem::path(path).stem().string()},
                                     {"parent", parent}, {"components", {{"Transform", transform}}}},
                                report);
    record.instance = PrefabInstance{path, nullptr, json};
    for (const Json& entry : entries)
        if (entry.contains("id") && entry["id"].get<EntityId>() != id)
            aliases[entry["id"].get<EntityId>()] = id;
    records.push_back(std::move(record));
}

// Throws Invalid / Json::exception. prefab: exactly one root, which is not an instance; no scene-level blocks needed.
void parseScene(const Json& json, Scene& out, LoadContext& context, SceneLoadReport& report, bool prefab)
{
    const Json& version = field(json, "version");
    if (!version.is_number_integer() || version.get<int>() != kSceneFileVersion)
        throw Invalid(std::format("unsupported \"version\" {} (expected {}; older files are not migrated)", version.dump(),
                                  kSceneFileVersion));

    const Json& entities = field(json, "entities");
    if (!entities.is_array())
        throw Invalid("\"entities\" must be an array");
    // Entities new in a prefab since an instance was saved get ids past every id the file names.
    EntityId maxId = 0;
    const auto see = [&](const Json& object) {
        if (object.is_object() && object.contains("id") && object["id"].is_number_unsigned())
            maxId = std::max(maxId, object["id"].get<EntityId>());
    };
    for (const Json& entity : entities) {
        see(entity);
        if (entity.is_object() && entity.contains("instance") && entity["instance"].is_array())
            for (const Json& entry : entity["instance"])
                see(entry);
    }
    EntityId next = maxId + 1;
    const std::function<EntityId()> fresh = [&] { return next++; };

    std::vector<Record> records;
    std::map<EntityId, EntityId> aliases; // a placeholder's unloaded entity ids -> the placeholder
    for (size_t i = 0; i < entities.size(); ++i) {
        try {
            if (entities[i].is_object() && entities[i].contains("prefab"))
                parseInstance(entities[i], context, fresh, records, aliases, report);
            else
                records.push_back(parseEntity(entities[i], report));
        } catch (const Invalid& e) {
            throw Invalid(std::format("entities[{}]: {}", i, e.what()));
        }
    }
    for (Record& record : records) {
        if (const auto alias = record.parent ? aliases.find(*record.parent) : aliases.end(); alias != aliases.end()) {
            record.placeholderParent = *record.parent;
            record.parent = alias->second;
        }
    }

    Scene scene;
    build(records, scene);
    scene.reserveId(std::max(maxId, next - 1));
    if (prefab) {
        if (scene.children(entt::null).size() != 1)
            throw Invalid("a prefab must have exactly one root entity");
        if (scene.registry().all_of<PrefabInstance>(scene.children(entt::null).front()))
            throw Invalid("the root of a prefab can't itself be a prefab instance");
    }
    if (json.contains("editor")) {
        if (!json["editor"].is_object())
            throw Invalid("\"editor\" must be an object");
        scene.editorData = json["editor"];
    }
    if (json.contains("lighting")) {
        scene.lighting = parseLighting(json["lighting"]);
        // A bad "sdfgi" block doesn't fail the load: it is dropped (SDFGI off) with a warning.
        if (std::string error; json["lighting"].contains("sdfgi") &&
                               !sdfgiFromJson(json["lighting"]["sdfgi"], scene.lighting.sdfgi, error))
            report.warnings.push_back(std::format("{}; SDFGI left off", error));
    }
    if (json.contains("postProcessing"))
        scene.postProcessing = parsePostProcessing(json["postProcessing"]);
    if (json.contains("sky"))
        scene.sky = parseSky(json["sky"]);
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
}

EntityId idOf(const Scene& scene, entt::entity entity) { return scene.registry().get<PersistentId>(entity).value; }
EntityId fileIdOf(const Scene& scene, entt::entity entity) { return fileEntityId(idOf(scene, entity)); } // as saved

Json parentJson(const Scene& scene, entt::entity entity)
{
    const entt::entity parent = scene.parent(entity);
    if (parent == entt::null)
        return nullptr;
    const auto* child = scene.registry().try_get<PlaceholderChild>(entity);
    return child && isMissingPrefab(scene, parent) ? child->parent : fileIdOf(scene, parent);
}

Json instanceToJson(const Scene& scene, entt::entity root, const PrefabInstance& instance)
{
    const entt::registry& registry = scene.registry();
    const EntityId rootId = idOf(scene, root);
    std::map<EntityId, entt::entity> linked; // prefab-local -> entity
    auto collect = [&](auto& self, entt::entity entity) -> void {
        if (const auto* link = registry.try_get<PrefabLink>(entity); link && link->hops.front().root == rootId)
            linked[link->hops.front().local] = entity;
        for (entt::entity child : scene.children(entity))
            self(self, child);
    };
    collect(collect, root);

    Json entries = Json::array();
    std::set<EntityId> absent;
    for (const PrefabAsset::Entity& base : instance.asset->entities) {
        if (absent.contains(base.parent)) {
            absent.insert(base.id);
            continue;
        }
        const auto it = linked.find(base.id);
        if (it == linked.end()) {
            absent.insert(base.id);
            entries.push_back({{"source", base.id}, {"deleted", true}});
            continue;
        }
        Json entry = {{"source", base.id}, {"id", fileIdOf(scene, it->second)}};
        const Json overrides = diffPrefabEntity(registry.get<Name>(it->second).value,
                                                entityComponentsToJson(registry, it->second), base, it->second == root);
        for (const auto& [key, value] : overrides.items())
            entry[key] = value;
        entries.push_back(std::move(entry));
    }
    return {{"id", fileEntityId(rootId)}, {"name", registry.get<Name>(root).value}, {"parent", parentJson(scene, root)},
            {"prefab", instance.path}, {"instance", std::move(entries)}};
}

// slot: only that loaded scene's entities.
Json entitiesToJson(const Scene& scene, std::optional<SceneSlot> slot = std::nullopt)
{
    const entt::registry& registry = scene.registry();
    Json entities = Json::array();
    scene.forEachDepthFirst([&](entt::entity entity) {
        if (slot && scene.sceneOf(entity) != *slot)
            return;
        const EntityId id = idOf(scene, entity);
        const auto* link = registry.try_get<PrefabLink>(entity);
        if (link && link->hops.front().root != id)
            return; // in its instance root's record
        if (const auto* instance = registry.try_get<PrefabInstance>(entity)) {
            if (instance->asset) {
                entities.push_back(instanceToJson(scene, entity, *instance));
                return;
            }
            Json json = instance->placeholder;
            if (!json.is_object()) // a nested placeholder that an Unpack made top-level: only its root's state is known
                json = {{"id", fileEntityId(id)}, {"name", registry.get<Name>(entity).value}, {"parent", nullptr},
                        {"prefab", instance->path},
                        {"instance", Json::array({{{"id", fileEntityId(id)},
                                                   {"components", {{"Transform", componentToJson(
                                                                        registry, entity, ComponentType(entt::resolve<Transform>()))}}}}})}};
            json["id"] = fileEntityId(id);
            json["parent"] = parentJson(scene, entity);
            entities.push_back(std::move(json));
            return;
        }
        Json json;
        json["id"] = fileEntityId(id);
        json["name"] = registry.get<Name>(entity).value;
        json["parent"] = parentJson(scene, entity);
        json["components"] = entityComponentsToJson(registry, entity);
        entities.push_back(std::move(json));
    });
    return entities;
}

} // namespace

Json entityComponentsToJson(const entt::registry& registry, entt::entity entity)
{
    Json components = Json::object();
    for (const ComponentType& type : allComponentTypes())
        if (hasComponent(registry, entity, type))
            components[type.name()] = componentToJson(registry, entity, type);
    if (const auto* preserved = registry.try_get<PreservedComponents>(entity))
        for (const auto& [name, value] : preserved->byName)
            components[name] = value;
    return components;
}

Json diffPrefabEntity(const std::string& name, const Json& components, const PrefabAsset::Entity& base, bool root)
{
    Json out = Json::object();
    if (!root && name != base.name)
        out["name"] = name;
    Json changed = Json::object();
    Json added = Json::object();
    Json removed = Json::array();
    for (const auto& [key, baseValue] : base.components.items()) {
        if (!components.contains(key)) {
            removed.push_back(key);
            continue;
        }
        const Json& value = components[key];
        if (root && key == entt::resolve<Transform>().name()) {
            changed[key] = value; // the instance's own
        } else if (value.is_object() && baseValue.is_object()) {
            Json fields = Json::object();
            for (const auto& [field, fieldValue] : value.items())
                if (!baseValue.contains(field) || baseValue[field] != fieldValue)
                    fields[field] = fieldValue;
            if (!fields.empty())
                changed[key] = std::move(fields);
        } else if (value != baseValue) {
            added[key] = value;
        }
    }
    for (const auto& [key, value] : components.items())
        if (!base.components.contains(key))
            added[key] = value;
    if (!changed.empty())
        out["components"] = std::move(changed);
    if (!added.empty())
        out["added"] = std::move(added);
    if (!removed.empty())
        out["removed"] = std::move(removed);
    return out;
}

entt::entity instantiatePrefab(Scene& scene, const std::shared_ptr<const PrefabAsset>& asset, entt::entity parent,
                               std::string& error)
{
    // Checked first: build would stop at the limit with part of the instance created.
    std::map<EntityId, int> depth; // in the prefab, root = 1
    int height = 0;
    for (const PrefabAsset::Entity& entity : asset->entities)
        height = std::max(height, depth[entity.id] = entity.parent ? depth[entity.parent] + 1 : 1);
    if (parent != entt::null && scene.depth(parent) + height > kMaxHierarchyDepth) {
        error = std::format("the instance would be deeper than {} levels (the hierarchy limit)", kMaxHierarchyDepth);
        return entt::null;
    }
    try {
        std::vector<Record> records;
        SceneLoadReport report;
        // Expanded with file ids (what the scene file checks allow), then scoped to parent's scene.
        const auto fresh = [&] { return fileEntityId(scene.newId(parent)); };
        expandAsset(asset, fresh(), nullptr, asset->entities.front().name, {}, nullptr, fresh, records, report);
        const SceneSlot slot = parent != entt::null ? scene.sceneOf(parent) : scene.activeSlot();
        for (Record& record : records) {
            record.id = scopedId(slot, record.id);
            if (record.parent)
                record.parent = scopedId(slot, *record.parent);
            for (PrefabHop& hop : record.hops)
                hop.root = scopedId(slot, hop.root);
        }
        build(records, scene, parent);
        return scene.children(parent).back();
    } catch (const Invalid& e) {
        error = e.what();
    }
    return entt::null;
}

double jsonNumber(float value)
{
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double result = 0.0;
    std::from_chars(buffer, written.ptr, result);
    return result;
}

bool sceneFromJson(const Json& json, Scene& out, std::string& error, SceneLoadReport* report,
                   const std::filesystem::path& projectRoot)
{
    SceneLoadReport local;
    LoadContext context{projectRoot};
    try {
        parseScene(json, out, context, local, false);
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

bool loadScene(const std::string& path, Scene& out, std::string& error, SceneLoadReport* report,
               const std::filesystem::path& projectRoot)
{
    std::string text;
    if (!readFile(path, text)) {
        error = std::format("Cannot open scene '{}'", path);
        return false;
    }
    std::string reason;
    try {
        SceneLoadReport local;
        Json json = Json::parse(text);
        readAssetRefs(json);
        if (sceneFromJson(json, out, reason, &local, projectRoot)) {
            out.activeScene().path = path;
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

bool loadPrefab(const std::filesystem::path& projectRoot, const std::string& path, std::shared_ptr<const PrefabAsset>& out,
                std::string& error, SceneLoadReport* report)
{
    LoadContext context{projectRoot};
    SceneLoadReport local;
    std::string why;
    std::shared_ptr<const PrefabAsset> asset = resolvePrefab(context, path, why, local);
    if (!asset) {
        error = std::format("Invalid prefab '{}': {}", path, why);
        return false;
    }
    for (const std::string& warning : local.warnings)
        log::warn(warning);
    out = std::move(asset);
    if (report)
        *report = std::move(local);
    return true;
}

bool loadPrefabScene(const std::filesystem::path& projectRoot, const std::string& path, Scene& out, std::string& error,
                     SceneLoadReport* report)
{
    LoadContext context{projectRoot, {path}}; // an instance of itself inside is a cycle
    SceneLoadReport local;
    std::string reason;
    try {
        readPrefab(context, path, out, local);
        for (const std::string& warning : local.warnings)
            log::warn(std::format("Prefab '{}': {}", path, warning));
        if (report)
            *report = std::move(local);
        return true;
    } catch (const Json::exception& e) {
        reason = e.what();
    } catch (const Invalid& e) {
        reason = e.what();
    }
    error = std::format("Invalid prefab '{}': {}", path, reason);
    return false;
}

Json skyToJson(const SceneSky& sky)
{
    constexpr const char* kModes[] = {"off", "procedural", "cubemap"}; // SkyMode order
    const auto color = [](const bx::Vec3& c) { return Json::array({jsonNumber(c.x), jsonNumber(c.y), jsonNumber(c.z)}); };
    Json faces = Json::object();
    for (size_t i = 0; i < sky.faces.size(); ++i)
        faces[kSkyFaceNames[i]] = sky.faces[i];
    Json json = {{"mode", kModes[static_cast<int>(sky.mode)]}, {"top", color(sky.top)}, {"horizon", color(sky.horizon)},
                 {"bottom", color(sky.bottom)}, {"faces", std::move(faces)}};
    if (!sky.cube.empty()) // older scenes stay byte-identical on save
        json["cube"] = sky.cube;
    return json;
}

Json lightmapsToJson(const SceneLightmaps& lightmaps)
{
    return {{"texelsPerMeter", jsonNumber(lightmaps.texelsPerMeter)}, {"bounces", lightmaps.bounces},
            {"samplesPerTexel", lightmaps.samplesPerTexel}, {"probeSpacing", jsonNumber(lightmaps.probeSpacing)},
            {"enabled", lightmaps.enabled}, {"intensity", jsonNumber(lightmaps.intensity)},
            {"directional", lightmaps.directional}, {"bakedPath", lightmaps.bakedPath}};
}

bool lightmapsFromJson(const Json& json, SceneLightmaps& out, std::string& error)
{
    try {
        out = parseLightmaps(json);
        return true;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

Json sdfgiToJson(const SceneSdfgi& sdfgi)
{
    constexpr const char* kQualities[] = {"low", "medium", "high"}; // SdfgiQuality order
    return {{"enabled", sdfgi.enabled}, {"cascades", sdfgi.cascades}, {"cellSize", jsonNumber(sdfgi.cellSize)},
            {"energy", jsonNumber(sdfgi.energy)}, {"bounceFeedback", jsonNumber(sdfgi.bounceFeedback)},
            {"quality", kQualities[static_cast<int>(sdfgi.quality)]}, {"yScale", jsonNumber(sdfgi.yScale)}};
}

bool sdfgiFromJson(const Json& json, SceneSdfgi& out, std::string& error)
{
    try {
        out = parseSdfgi(json);
        return true;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

Json postBlockToJson(const ScenePostProcessing& post, const char* name)
{
    return postProcessingToJson(post)[name];
}

bool postBlockFromJson(const Json& json, const char* name, ScenePostProcessing& out, std::string& error)
{
    Json all = postProcessingToJson(out);
    all[name] = json;
    try {
        out = parsePostProcessing(all);
        return true;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

bool skyFromJson(const Json& json, SceneSky& out, std::string& error)
{
    try {
        out = parseSky(json);
        return true;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return false;
}

Json sceneToJson(const Scene& scene) { return sceneToJson(scene, scene.activeSlot()); }

Json sceneToJson(const Scene& scene, SceneSlot slot)
{
    const SceneSettings& settings = scene.settingsOf(slot);
    Json root;
    root["version"] = kSceneFileVersion;
    root["entities"] = entitiesToJson(scene, slot);
    root["systems"] = settings.systems;
    const SceneLighting& lighting = settings.lighting;
    root["lighting"] = {{"ambientColor", Json::array({jsonNumber(lighting.ambientColor.x), jsonNumber(lighting.ambientColor.y),
                                                      jsonNumber(lighting.ambientColor.z)})},
                        {"ambientIntensity", jsonNumber(lighting.ambientIntensity)}};
    if (lighting.lightmaps != SceneLightmaps{}) // scenes without lightmaps stay byte-identical on save
        root["lighting"]["lightmaps"] = lightmapsToJson(lighting.lightmaps);
    if (lighting.sdfgi != SceneSdfgi{}) // nor without SDFGI settings
        root["lighting"]["sdfgi"] = sdfgiToJson(lighting.sdfgi);
    root["postProcessing"] = postProcessingToJson(settings.postProcessing);
    root["sky"] = skyToJson(settings.sky);
    const bx::Vec3& gravity = settings.physics.gravity;
    root["physics"] = {{"gravity", Json::array({jsonNumber(gravity.x), jsonNumber(gravity.y), jsonNumber(gravity.z)})}};
    root["editor"] = settings.editorData;
    return root;
}

SceneSlot appendScene(Scene& into, Scene&& from, std::string path, std::optional<SceneSlot> slot)
{
    const SceneSlot added = into.addScene(std::move(path), std::move(static_cast<SceneSettings&>(from)), slot);
    const entt::registry& source = from.registry();
    entt::registry& target = into.registry();
    const auto scoped = [added](EntityId id) { return scopedId(added, fileEntityId(id)); };
    const auto copy = [&](auto& self, entt::entity entity, entt::entity parent) -> void {
        const entt::entity created = into.createWithId(scoped(idOf(from, entity)), source.get<Name>(entity).value, parent);
        copyComponents(source, entity, target, created);
        if (const auto* link = source.try_get<PrefabLink>(entity)) {
            PrefabLink scopedLink = *link;
            for (PrefabHop& hop : scopedLink.hops)
                hop.root = scoped(hop.root);
            target.emplace<PrefabLink>(created, std::move(scopedLink));
        }
        if (const auto* instance = source.try_get<PrefabInstance>(entity))
            target.emplace<PrefabInstance>(created, *instance);
        if (const auto* child = source.try_get<PlaceholderChild>(entity))
            target.emplace<PlaceholderChild>(created, *child);
        for (entt::entity grandchild : from.children(entity))
            self(self, grandchild, created);
    };
    for (entt::entity root : from.children(entt::null))
        copy(copy, root, entt::null);
    from = Scene{};
    return added;
}

bool loadSceneAdditive(const std::string& path, Scene& into, SceneSlot& slot, std::string& error, SceneLoadReport* report,
              const std::filesystem::path& projectRoot)
{
    if (into.findScene(path)) {
        error = std::format("Scene '{}' is already loaded", path);
        return false;
    }
    Scene loaded;
    if (!loadScene(path, loaded, error, report, projectRoot))
        return false;
    slot = appendScene(into, std::move(loaded), path);
    return true;
}

Json scenesToJson(const Scene& scene)
{
    Json scenes = Json::array();
    for (const LoadedScene& loaded : scene.loadedScenes())
        scenes.push_back({{"slot", loaded.slot}, {"path", loaded.path}, {"scene", sceneToJson(scene, loaded.slot)}});
    return {{"active", scene.activeSlot()}, {"scenes", std::move(scenes)}};
}

bool scenesFromJson(const Json& json, Scene& out, std::string& error, SceneLoadReport* report,
                    const std::filesystem::path& projectRoot)
{
    try {
        Scene result;
        bool first = true;
        for (const Json& entry : json.at("scenes")) {
            const SceneSlot slot = entry.at("slot").get<SceneSlot>();
            Scene loaded;
            SceneLoadReport local;
            if (!sceneFromJson(entry.at("scene"), loaded, error, &local, projectRoot))
                return false;
            if (report)
                report->warnings.insert(report->warnings.end(), local.warnings.begin(), local.warnings.end());
            if (first && slot == 0) {
                result = std::move(loaded);
                result.activeScene().path = entry.at("path").get<std::string>();
            } else {
                appendScene(result, std::move(loaded), entry.at("path").get<std::string>(), slot);
            }
            first = false;
        }
        result.setActiveScene(json.at("active").get<SceneSlot>());
        if (result.loadedScenes().front().slot == 0 && json.at("scenes").front().at("slot") != 0)
            result.removeScene(0); // the empty one Scene starts with
        out = std::move(result);
        return true;
    } catch (const Json::exception& e) {
        error = e.what();
    }
    return false;
}

Json prefabToJson(const Scene& scene) { return {{"version", kSceneFileVersion}, {"entities", entitiesToJson(scene)}}; }

bool saveScene(const Scene& scene, const std::string& path, std::string& error)
{
    return saveScene(scene, scene.activeSlot(), path, error);
}

bool saveScene(const Scene& scene, SceneSlot slot, const std::string& path, std::string& error)
{
    Json json = sceneToJson(scene, slot);
    writeAssetRefs(json);
    if (!writeFileAtomic(path, json.dump(2) + '\n')) {
        error = std::format("Cannot write scene '{}'", path);
        return false;
    }
    return true;
}

bool savePrefab(const Scene& scene, const std::filesystem::path& projectRoot, const std::string& path, std::string& error)
{
    const std::vector<entt::entity>& roots = scene.children(entt::null);
    if (roots.size() != 1) {
        error = std::format("Cannot save prefab '{}': it must have exactly one root entity (it has {})", path, roots.size());
        return false;
    }
    if (scene.registry().all_of<PrefabInstance>(roots.front())) {
        error = std::format("Cannot save prefab '{}': its root can't itself be a prefab instance", path);
        return false;
    }
    if (std::string cycle = prefabCycleError(scene, path); !cycle.empty()) {
        error = std::format("Cannot save prefab '{}': {}", path, cycle);
        return false;
    }
    std::string reason;
    const std::optional<std::filesystem::path> file = resolveProjectPath(projectRoot, path, reason);
    std::error_code ec;
    if (file)
        std::filesystem::create_directories(file->parent_path(), ec); // a failed write below says so
    Json json = prefabToJson(scene);
    writeAssetRefs(json);
    if (!file || !writeFileAtomic(*file, json.dump(2) + '\n')) {
        error = std::format("Cannot write prefab '{}'{}", path, file ? "" : ": " + reason);
        return false;
    }
    return true;
}

} // namespace kast
