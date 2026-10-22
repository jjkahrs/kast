#include "material.h"

#include "files.h"
#include "log.h"
#include "project.h"
#include "scene_io.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::array<std::pair<AlphaMode, const char*>, 4> kAlphaModes = {
    {{AlphaMode::Opaque, "opaque"}, {AlphaMode::Mask, "mask"}, {AlphaMode::Blend, "blend"}, {AlphaMode::Additive, "additive"}}};
constexpr std::array<std::pair<CullMode, const char*>, 3> kCullModes = {
    {{CullMode::None, "none"}, {CullMode::Back, "back"}, {CullMode::Front, "front"}}};

// The enum value named by json in names. False + error otherwise.
template <typename Enum, size_t N>
bool enumName(const Json& json, const std::array<std::pair<Enum, const char*>, N>& names, Enum& out)
{
    const auto it = std::ranges::find_if(names, [&](const auto& n) { return json.is_string() && json.get<std::string>() == n.second; });
    if (it != names.end())
        out = it->first;
    return it != names.end();
}

template <typename Enum, size_t N>
const char* nameOf(Enum value, const std::array<std::pair<Enum, const char*>, N>& names)
{
    return std::ranges::find(names, value, &std::pair<Enum, const char*>::first)->second;
}

// N numbers, each in 0..1. False + error naming the field otherwise.
template <size_t N>
bool unitArray(std::string_view name, const Json& json, std::array<float, N>& out, std::string& error)
{
    bool ok = json.is_array() && json.size() == N;
    for (size_t i = 0; ok && i < N; ++i) {
        ok = json[i].is_number() && json[i].get<double>() >= 0.0 && json[i].get<double>() <= 1.0;
        if (ok)
            out[i] = json[i].get<float>();
    }
    if (!ok)
        error = std::format("\"{}\" must be an array of {} numbers between 0 and 1", name, N);
    return ok;
}

bool unitNumber(std::string_view name, const Json& json, float& out, std::string& error)
{
    if (!json.is_number() || !(json.get<double>() >= 0.0 && json.get<double>() <= 1.0)) {
        error = std::format("\"{}\" must be a number between 0 and 1", name);
        return false;
    }
    out = json.get<float>();
    return true;
}

bool finiteNumber(std::string_view name, const Json& json, float& out, std::string& error)
{
    if (!json.is_number() || !std::isfinite(json.get<double>())) {
        error = std::format("\"{}\" must be a finite number", name);
        return false;
    }
    out = json.get<float>();
    return true;
}

bool finitePair(std::string_view name, const Json& json, std::array<float, 2>& out, std::string& error)
{
    const bool ok = json.is_array() && json.size() == 2 &&
                    std::ranges::all_of(json, [](const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); });
    if (ok)
        out = {json[0].get<float>(), json[1].get<float>()};
    else
        error = std::format("\"{}\" must be an array of 2 finite numbers", name);
    return ok;
}

bool flag(std::string_view name, const Json& json, bool& out, std::string& error)
{
    if (!json.is_boolean()) {
        error = std::format("\"{}\" must be true or false", name);
        return false;
    }
    out = json.get<bool>();
    return true;
}

bool texturePath(std::string_view name, const Json& json, std::string& out, std::string& error)
{
    if (!json.is_string()) {
        error = std::format("\"{}\" must be a string (a project-relative path, or empty for none)", name);
        return false;
    }
    out = json.get<std::string>();
    return true;
}

// "params": an object of param values (ShaderParamValue). Which param takes which is the shader's to say.
bool shaderParams(const Json& json, std::map<std::string, ShaderParamValue>& out, std::string& error)
{
    if (!json.is_object()) {
        error = "\"params\" must be an object (param name: value)";
        return false;
    }
    std::map<std::string, ShaderParamValue> params;
    for (const auto& [name, value] : json.items()) {
        ShaderParamValue& param = params[name];
        const auto finite = [](const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); };
        if (finite(value)) {
            param.numbers = {value.get<float>()};
        } else if (value.is_array() && value.size() >= 2 && value.size() <= 4 && std::ranges::all_of(value, finite)) {
            for (const Json& v : value)
                param.numbers.push_back(v.get<float>());
        } else if (value.is_string()) {
            param.texture = value.get<std::string>();
        } else {
            error = std::format("\"params\": \"{}\" must be a finite number, an array of 2 to 4 finite numbers or a texture "
                                "path", name);
            return false;
        }
    }
    out = std::move(params);
    return true;
}

Json numbers(std::initializer_list<float> values)
{
    Json out = Json::array();
    for (float v : values)
        out.push_back(jsonNumber(v));
    return out;
}

// .mat files from before emissiveEnabled / emissiveIntensity have only "emissive" (any value >= 0): emission is on if
// it is non-black, and channels above 1 move into the intensity (color / max channel, intensity = max channel), so
// they render as before. Invalid colors are left for setMaterialField to reject.
Json migrateLegacyEmissive(Json json)
{
    if (json.contains("emissiveEnabled") || json.contains("emissiveIntensity") || !json.contains("emissive"))
        return json;
    Json& color = json["emissive"];
    if (!color.is_array() || color.size() != 3 || !std::ranges::all_of(color, [](const Json& c) { return c.is_number() && c.get<double>() >= 0.0; }))
        return json;
    const double max = std::max({color[0].get<double>(), color[1].get<double>(), color[2].get<double>()});
    if (max > 1.0)
        for (Json& c : color)
            c = c.get<double>() / max;
    // Adding keys last: ordered_json keeps them in a vector, so an insert can invalidate `color`.
    json["emissiveEnabled"] = max > 0.0;
    if (max > 1.0)
        json["emissiveIntensity"] = max;
    return json;
}

} // namespace

bx::Vec3 emissiveRadiance(const Material& material)
{
    return material.emissiveEnabled ? bx::mul(material.emissive, material.emissiveIntensity) : bx::Vec3{0.0f, 0.0f, 0.0f};
}

const Material& defaultMaterial()
{
    static const Material material;
    return material;
}

const Material& errorMaterial()
{
    static const Material material = [] {
        Material m;
        m.baseColor = {0.0f, 0.0f, 0.0f, 1.0f};
        m.roughness = 1.0f;
        m.emissiveEnabled = true;
        m.emissive = {1.0f, 0.0f, 1.0f};
        return m;
    }();
    return material;
}

const std::vector<std::string>& materialFieldNames()
{
    static const std::vector<std::string> names = {
        "baseColor", "baseColorTexture", "metallic", "roughness", "metallicRoughnessTexture", "normalTexture", "normalScale",
        "occlusionTexture", "occlusionStrength", "emissiveEnabled", "emissive", "emissiveIntensity", "emissiveTexture",
        "uvScale", "uvOffset", "alphaMode", "alphaCutoff", "cullMode", "unlit", "shader", "params"};
    return names;
}

bool setMaterialField(Material& material, std::string_view name, const Json& value, std::string& error)
{
    Material m = material; // all-or-nothing
    bool ok = false;
    if (name == "baseColor") {
        ok = unitArray(name, value, m.baseColor, error);
    } else if (name == "baseColorTexture") {
        ok = texturePath(name, value, m.baseColorTexture, error);
    } else if (name == "metallic") {
        ok = unitNumber(name, value, m.metallic, error);
    } else if (name == "roughness") {
        ok = unitNumber(name, value, m.roughness, error);
    } else if (name == "metallicRoughnessTexture") {
        ok = texturePath(name, value, m.metallicRoughnessTexture, error);
    } else if (name == "normalTexture") {
        ok = texturePath(name, value, m.normalTexture, error);
    } else if (name == "normalScale") {
        ok = finiteNumber(name, value, m.normalScale, error);
    } else if (name == "occlusionTexture") {
        ok = texturePath(name, value, m.occlusionTexture, error);
    } else if (name == "occlusionStrength") {
        ok = unitNumber(name, value, m.occlusionStrength, error);
    } else if (name == "emissiveEnabled") {
        ok = flag(name, value, m.emissiveEnabled, error);
    } else if (name == "emissive") {
        std::array<float, 3> v{};
        ok = unitArray(name, value, v, error);
        m.emissive = {v[0], v[1], v[2]};
    } else if (name == "emissiveIntensity") {
        ok = value.is_number() && value.get<double>() >= 0.0 && std::isfinite(value.get<double>());
        if (ok)
            m.emissiveIntensity = value.get<float>();
        else
            error = "\"emissiveIntensity\" must be a number >= 0";
    } else if (name == "emissiveTexture") {
        ok = texturePath(name, value, m.emissiveTexture, error);
    } else if (name == "uvScale") {
        ok = finitePair(name, value, m.uvScale, error);
    } else if (name == "uvOffset") {
        ok = finitePair(name, value, m.uvOffset, error);
    } else if (name == "alphaMode") {
        ok = enumName(value, kAlphaModes, m.alphaMode);
        if (!ok)
            error = "\"alphaMode\" must be \"opaque\", \"mask\", \"blend\" or \"additive\"";
    } else if (name == "alphaCutoff") {
        ok = unitNumber(name, value, m.alphaCutoff, error);
    } else if (name == "cullMode") {
        ok = enumName(value, kCullModes, m.cullMode);
        if (!ok)
            error = "\"cullMode\" must be \"none\", \"back\" or \"front\"";
    } else if (name == "unlit") {
        ok = flag(name, value, m.unlit, error);
    } else if (name == "shader") {
        ok = value.is_string();
        if (ok)
            m.shader = value.get<std::string>();
        else
            error = "\"shader\" must be a string (a project-relative .kshader path, or empty for the built-in shader)";
    } else if (name == "params") {
        ok = shaderParams(value, m.params, error);
    } else {
        error = std::format("unknown field \"{}\"", name);
    }
    if (ok)
        material = std::move(m);
    return ok;
}

std::optional<Json> materialField(const Material& m, std::string_view name)
{
    if (name == "baseColor")
        return numbers({m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3]});
    if (name == "baseColorTexture")
        return m.baseColorTexture;
    if (name == "metallic")
        return jsonNumber(m.metallic);
    if (name == "roughness")
        return jsonNumber(m.roughness);
    if (name == "metallicRoughnessTexture")
        return m.metallicRoughnessTexture;
    if (name == "normalTexture")
        return m.normalTexture;
    if (name == "normalScale")
        return jsonNumber(m.normalScale);
    if (name == "occlusionTexture")
        return m.occlusionTexture;
    if (name == "occlusionStrength")
        return jsonNumber(m.occlusionStrength);
    if (name == "emissiveEnabled")
        return m.emissiveEnabled;
    if (name == "emissive")
        return numbers({m.emissive.x, m.emissive.y, m.emissive.z});
    if (name == "emissiveIntensity")
        return jsonNumber(m.emissiveIntensity);
    if (name == "emissiveTexture")
        return m.emissiveTexture;
    if (name == "uvScale")
        return numbers({m.uvScale[0], m.uvScale[1]});
    if (name == "uvOffset")
        return numbers({m.uvOffset[0], m.uvOffset[1]});
    if (name == "alphaMode")
        return nameOf(m.alphaMode, kAlphaModes);
    if (name == "alphaCutoff")
        return jsonNumber(m.alphaCutoff);
    if (name == "cullMode")
        return nameOf(m.cullMode, kCullModes);
    if (name == "unlit")
        return m.unlit;
    if (name == "shader")
        return m.shader;
    if (name == "params") {
        Json params = Json::object();
        for (const auto& [param, value] : m.params) {
            if (value.numbers.empty())
                params[param] = value.texture;
            else if (value.numbers.size() == 1)
                params[param] = jsonNumber(value.numbers[0]);
            else
                for (float v : value.numbers)
                    params[param].push_back(jsonNumber(v));
        }
        return params;
    }
    return std::nullopt;
}

bool materialFromJson(const Json& json, Material& out, std::string& error)
{
    if (!json.is_object()) {
        error = "must be a JSON object";
        return false;
    }
    // Versions 2 and 3 only added fields: an older file is a version 3 file with those at their defaults.
    if (!json.contains("version") ||
        (json["version"] != 1 && json["version"] != 2 && json["version"] != kMaterialFileVersion)) {
        error = std::format("\"version\" must be 1, 2 or {}", kMaterialFileVersion);
        return false;
    }
    Material material;
    const Json fields = migrateLegacyEmissive(json);
    for (const auto& [key, value] : fields.items())
        if (key != "version" && !setMaterialField(material, key, value, error))
            return false;
    out = std::move(material);
    return true;
}

Json materialToJson(const Material& material)
{
    Json json = {{"version", kMaterialFileVersion}};
    for (const std::string& name : materialFieldNames())
        json[name] = *materialField(material, name);
    return json;
}

bool loadMaterial(const std::filesystem::path& file, Material& out, std::string& error)
{
    std::string text;
    if (!readFile(file, text)) {
        error = "cannot read the file";
        return false;
    }
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        error = "invalid JSON";
        return false;
    }
    return materialFromJson(json, out, error);
}

std::vector<size_t> backToFront(const std::vector<bx::Vec3>& centers, const bx::Vec3& eye)
{
    std::vector<size_t> order(centers.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::ranges::stable_sort(order, std::greater{}, [&](size_t i) { return bx::distanceSq(centers[i], eye); });
    return order;
}

void warnOnce(const std::string& message)
{
    // ponytail: grows by one string per distinct message; fine for per-path asset errors.
    static std::unordered_set<std::string> seen;
    if (seen.insert(message).second)
        log::warn(message);
}

std::optional<std::filesystem::file_time_type> modifiedTime(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    return ec ? std::nullopt : std::optional(time);
}

void MaterialLibrary::setRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        entries_.clear();
    }
}

const Material& MaterialLibrary::get(const std::string& path)
{
    if (path.empty())
        return defaultMaterial();
    auto [it, added] = entries_.try_emplace(path);
    if (added)
        load(path, it->second);
    return it->second.material;
}

void MaterialLibrary::reloadChanged()
{
    // ponytail: one stat per cached material per call (per frame); watch the folder if projects grow to thousands.
    for (auto& [path, entry] : entries_)
        if (!root_.empty() && modifiedTime(root_ / path) != entry.mtime)
            load(path, entry);
}

void MaterialLibrary::load(const std::string& path, Entry& entry)
{
    std::string error;
    const std::optional<std::filesystem::path> file = root_.empty() ? std::nullopt : resolveProjectPath(root_, path, error);
    if (root_.empty())
        error = "no project";
    entry.mtime = file ? modifiedTime(*file) : std::nullopt;
    if (file && loadMaterial(*file, entry.material, error))
        return;
    entry.material = errorMaterial();
    warnOnce(std::format("Material '{}': {}; rendering it magenta", path, error));
}

} // namespace kast
