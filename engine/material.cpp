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

constexpr std::array<std::pair<AlphaMode, const char*>, 3> kAlphaModes = {
    {{AlphaMode::Opaque, "opaque"}, {AlphaMode::Mask, "mask"}, {AlphaMode::Blend, "blend"}}};

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

bool texturePath(std::string_view name, const Json& json, std::string& out, std::string& error)
{
    if (!json.is_string()) {
        error = std::format("\"{}\" must be a string (a project-relative path, or empty for none)", name);
        return false;
    }
    out = json.get<std::string>();
    return true;
}

Json numbers(std::initializer_list<float> values)
{
    Json out = Json::array();
    for (float v : values)
        out.push_back(jsonNumber(v));
    return out;
}

} // namespace

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
        m.emissive = {1.0f, 0.0f, 1.0f};
        return m;
    }();
    return material;
}

const std::vector<std::string>& materialFieldNames()
{
    static const std::vector<std::string> names = {"baseColor", "baseColorTexture", "metallic", "roughness",
                                                   "metallicRoughnessTexture", "emissive", "alphaMode", "alphaCutoff"};
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
    } else if (name == "emissive") {
        std::array<float, 3> v{};
        ok = unitArray(name, value, v, error);
        m.emissive = {v[0], v[1], v[2]};
    } else if (name == "alphaMode") {
        const auto it = std::ranges::find_if(kAlphaModes, [&](const auto& mode) {
            return value.is_string() && value.get<std::string>() == mode.second;
        });
        ok = it != kAlphaModes.end();
        if (ok)
            m.alphaMode = it->first;
        else
            error = "\"alphaMode\" must be \"opaque\", \"mask\" or \"blend\"";
    } else if (name == "alphaCutoff") {
        ok = unitNumber(name, value, m.alphaCutoff, error);
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
    if (name == "emissive")
        return numbers({m.emissive.x, m.emissive.y, m.emissive.z});
    if (name == "alphaMode")
        return std::ranges::find(kAlphaModes, m.alphaMode, &std::pair<AlphaMode, const char*>::first)->second;
    if (name == "alphaCutoff")
        return jsonNumber(m.alphaCutoff);
    return std::nullopt;
}

bool materialFromJson(const Json& json, Material& out, std::string& error)
{
    if (!json.is_object()) {
        error = "must be a JSON object";
        return false;
    }
    if (!json.contains("version") || json["version"] != kMaterialFileVersion) {
        error = std::format("\"version\" must be {}", kMaterialFileVersion);
        return false;
    }
    Material material;
    for (const auto& [key, value] : json.items())
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
