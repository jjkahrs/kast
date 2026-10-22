#include "shader_graph.h"

#include "scene_io.h" // jsonNumber

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <set>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

std::string_view pinTypeName(PinType type)
{
    constexpr std::string_view kNames[] = {"float", "vec2", "vec3", "vec4", "texture2D", "number"};
    return kNames[static_cast<int>(type)];
}

int width(PinType type) // 1..4 for numbers
{
    return type == PinType::Texture2D ? 0 : type == PinType::Dynamic ? 1 : static_cast<int>(type) + 1;
}

PinType numberType(int components) { return static_cast<PinType>(std::clamp(components, 1, 4) - 1); }

std::string stageNames(unsigned stages)
{
    std::string out;
    for (const auto& [bit, name] : {std::pair{kStageVertex, "vertex()"}, {kStageFragment, "fragment()"}, {kStageLight, "light()"}})
        if (stages & bit)
            out += (out.empty() ? "" : ", ") + std::string(name);
    return out;
}

// A float as GLSL source: always with a '.' or an exponent, so it is never an int.
std::string glslFloat(float v)
{
    std::string s = std::format("{}", v);
    if (s.find_first_of(".e") == std::string::npos)
        s += ".0";
    return s;
}

std::string literal(const std::vector<float>& values)
{
    if (values.size() == 1)
        return glslFloat(values[0]);
    std::string out = std::format("vec{}(", values.size());
    for (size_t i = 0; i < values.size(); ++i)
        out += (i ? ", " : "") + glslFloat(values[i]);
    return out + ")";
}

// code of type from as type to: a float splats, a narrower vector pads with zeros, a wider one is truncated.
std::string convert(const std::string& code, PinType from, PinType to)
{
    const int f = width(from), t = width(to);
    if (from == to || to == PinType::Texture2D || from == PinType::Texture2D || f == t)
        return code;
    if (f == 1)
        return std::format("vec{}_splat({})", t, code);
    if (f > t)
        return std::format("({}).{}", code, std::string_view("xyzw").substr(0, static_cast<size_t>(t)));
    std::string out = std::format("vec{}({}", t, code);
    for (int i = f; i < t; ++i)
        out += ", 0.0";
    return out + ")";
}

// The graph's nodes by id and its links by the input they go into.
struct Index {
    std::map<int, const GraphNode*> nodes;
    std::map<std::pair<int, std::string>, const GraphLink*> into;
    explicit Index(const ShaderGraph& graph)
    {
        for (const GraphNode& node : graph.nodes)
            nodes.emplace(node.id, &node);
        for (const GraphLink& link : graph.links)
            into.emplace(std::pair{link.to.node, link.to.pin}, &link);
    }
    const GraphNode* node(int id) const
    {
        const auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : it->second;
    }
    const GraphLink* linkInto(int node, std::string_view pin) const
    {
        const auto it = into.find(std::pair{node, std::string(pin)});
        return it == into.end() ? nullptr : it->second;
    }
};

const GraphPinDef* findPin(const std::vector<GraphPinDef>& pins, std::string_view name)
{
    const auto it = std::ranges::find(pins, name, &GraphPinDef::name);
    return it == pins.end() ? nullptr : &*it;
}

const GraphPinDef* findOutput(const GraphNodeDef& def, std::string_view name)
{
    const auto it = std::ranges::find_if(def.outputs, [&](const auto& o) { return o.first.name == name; });
    return it == def.outputs.end() ? nullptr : &it->first;
}

const ShaderParam* nodeParam(const ShaderGraph& graph, const GraphNode& node)
{
    const auto it = node.settings.find("param");
    if (it == node.settings.end() || !it->second.is_string())
        return nullptr;
    const auto p = std::ranges::find(graph.params, it->second.get<std::string>(), &ShaderParam::name);
    return p == graph.params.end() ? nullptr : &*p;
}

// Output pin types, Dynamic resolved: a Param's from its param, a math node's the widest of its dynamic inputs (linked
// outputs' types, constants' sizes, else float).
class Types {
public:
    Types(const ShaderGraph& graph, const Index& index) : graph_(graph), index_(index) {}

    PinType output(const GraphNode& node, const GraphPinDef& pin)
    {
        if (pin.type != PinType::Dynamic)
            return pin.type;
        if (node.type == "param") {
            const ShaderParam* param = nodeParam(graph_, node);
            if (param && param->type == ShaderParamType::Texture2D)
                return PinType::Texture2D;
            return numberType(param ? componentCount(param->type) : 1);
        }
        if (node.type == "swizzle") {
            const auto mask = node.settings.find("mask");
            return numberType(mask != node.settings.end() && mask->second.is_string()
                                  ? static_cast<int>(mask->second.get<std::string>().size())
                                  : 1);
        }
        return dynamic(node);
    }
    PinType output(const GraphPin& pin)
    {
        const GraphNode* node = index_.node(pin.node);
        const GraphNodeDef* def = node ? graphNodeDef(*node) : nullptr;
        const GraphPinDef* out = def ? findOutput(*def, pin.pin) : nullptr;
        return out ? output(*node, *out) : PinType::Dynamic;
    }
    // The type a dynamic input of node is: linked, its source's; a constant, its size; else float.
    PinType input(const GraphNode& node, const GraphPinDef& pin)
    {
        if (const GraphLink* link = index_.linkInto(node.id, pin.name))
            return output(link->from);
        const auto it = node.inputs.find(std::string(pin.name));
        return it != node.inputs.end() ? numberType(static_cast<int>(it->second.size()))
               : pin.type == PinType::Dynamic ? PinType::Float
                                              : pin.type;
    }
    PinType dynamic(const GraphNode& node)
    {
        if (const auto it = memo_.find(node.id); it != memo_.end())
            return it->second;
        memo_[node.id] = PinType::Float; // a cycle (only in a graph that failed to load) ends here
        int widest = 1;
        if (const GraphNodeDef* def = graphNodeDef(node))
            for (const GraphPinDef& pin : def->inputs)
                if (pin.type == PinType::Dynamic)
                    widest = std::max(widest, width(input(node, pin)));
        return memo_[node.id] = numberType(widest);
    }

private:
    const ShaderGraph& graph_;
    const Index& index_;
    std::map<int, PinType> memo_;
};

// True if to's node is upstream of from's node (or the same): a link from -> to would close a cycle.
bool reaches(const Index& index, int upstream, int node)
{
    std::set<int> seen;
    std::vector<int> stack{node};
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        if (id == upstream)
            return true;
        if (!seen.insert(id).second)
            continue;
        for (const auto& [key, link] : index.into)
            if (key.first == id)
                stack.push_back(link->from.node);
    }
    return false;
}

// --- JSON ---

bool fail(std::string& error, std::string_view path, std::string_view message)
{
    error = std::format("{}: {}", path, message);
    return false;
}

bool onlyKeys(const Json& json, std::string_view path, std::initializer_list<std::string_view> keys, std::string& error)
{
    if (!json.is_object())
        return fail(error, path, "must be an object");
    for (const auto& [key, value] : json.items())
        if (std::ranges::find(keys, key) == keys.end())
            return fail(error, path.empty() ? key : std::format("{}.{}", path, key), "unknown key");
    return true;
}

// lo..hi finite numbers: a number (when lo is 1) or an array.
bool readFloats(const Json& json, const std::string& path, size_t lo, size_t hi, std::vector<float>& out, std::string& error)
{
    const auto finite = [](const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); };
    out.clear();
    if (lo == 1 && finite(json)) {
        out.push_back(json.get<float>());
        return true;
    }
    if (json.is_array() && json.size() >= lo && json.size() <= hi && std::ranges::all_of(json, finite)) {
        for (const Json& v : json)
            out.push_back(v.get<float>());
        return true;
    }
    const std::string count = lo == hi ? std::to_string(lo) : std::format("{} to {}", std::max<size_t>(lo, 2), hi);
    return fail(error, path,
                lo == 1 && hi == 1 ? "must be a finite number"
                : lo == 1          ? std::format("must be a finite number or an array of {} finite numbers", count)
                                   : std::format("must be an array of {} finite numbers", count));
}

Json numbersJson(const std::vector<float>& values)
{
    if (values.size() == 1)
        return jsonNumber(values[0]);
    Json out = Json::array();
    for (float v : values)
        out.push_back(jsonNumber(v));
    return out;
}

template <size_t N>
bool readArray(const Json& json, const std::string& path, std::array<float, N>& out, std::string& error)
{
    std::vector<float> values;
    if (!readFloats(json, path, N, N, values, error))
        return false;
    std::ranges::copy(values, out.begin());
    return true;
}

constexpr std::pair<TextureDefault, std::string_view> kTextureDefaults[] = {
    {TextureDefault::White, "white"}, {TextureDefault::Black, "black"}, {TextureDefault::Normal, "normal"}};

std::string_view textureDefaultName(TextureDefault value)
{
    return std::ranges::find(kTextureDefaults, value, &std::pair<TextureDefault, std::string_view>::first)->second;
}

bool readParam(const Json& json, const std::string& path, ShaderParam& param, std::string& error)
{
    if (!onlyKeys(json, path, {"name", "type", "default", "range", "srgb"}, error))
        return false;
    if (!json.contains("name") || !json["name"].is_string())
        return fail(error, path + ".name", "must be a string");
    param.name = json["name"].get<std::string>();
    const std::optional<ShaderParamType> type =
        json.contains("type") && json["type"].is_string() ? shaderParamTypeFromName(json["type"].get<std::string>()) : std::nullopt;
    if (!type)
        return fail(error, path + ".type", "must be \"float\", \"vec2\", \"vec3\", \"vec4\", \"color\" or \"texture2D\"");
    param.type = *type;
    param.value = param.type == ShaderParamType::Color ? std::array{1.0f, 1.0f, 1.0f, 1.0f} : std::array<float, 4>{};
    const bool texture = param.type == ShaderParamType::Texture2D;
    if (json.contains("range") && param.type != ShaderParamType::Float)
        return fail(error, path + ".range", "is only for float params");
    if (json.contains("srgb") && !texture)
        return fail(error, path + ".srgb", "is only for texture2D params");
    if (json.contains("default")) {
        const Json& value = json["default"];
        if (texture) {
            const auto it = std::ranges::find_if(kTextureDefaults, [&](const auto& d) { return value.is_string() && value.get<std::string>() == d.second; });
            if (it == std::end(kTextureDefaults))
                return fail(error, path + ".default", "must be \"white\", \"black\" or \"normal\"");
            param.texture = it->first;
        } else {
            const auto count = static_cast<size_t>(componentCount(param.type));
            std::vector<float> values;
            if (!readFloats(value, path + ".default", param.type == ShaderParamType::Color ? 3 : count, count, values, error))
                return false;
            std::ranges::copy(values, param.value.begin()); // a color's alpha stays 1 without a 4th number
        }
    }
    if (json.contains("range")) {
        std::array<float, 2> range{};
        if (!readArray(json["range"], path + ".range", range, error))
            return false;
        if (!(range[0] < range[1]))
            return fail(error, path + ".range", "needs min < max");
        param.range = range;
    }
    if (json.contains("srgb")) {
        if (!json["srgb"].is_boolean())
            return fail(error, path + ".srgb", "must be true or false");
        param.srgb = json["srgb"].get<bool>();
    }
    return true;
}

Json paramJson(const ShaderParam& param)
{
    Json json = {{"name", param.name}, {"type", std::string(shaderParamTypeName(param.type))}};
    if (param.type == ShaderParamType::Texture2D) {
        json["default"] = std::string(textureDefaultName(param.texture));
        json["srgb"] = param.srgb;
        return json;
    }
    const int count = param.type == ShaderParamType::Color ? 4 : componentCount(param.type);
    json["default"] = numbersJson(std::vector<float>(param.value.begin(), param.value.begin() + count));
    if (param.range)
        json["range"] = numbersJson({(*param.range)[0], (*param.range)[1]});
    return json;
}

// A setting's value when a node doesn't give it: its fallback string, or zeros.
Json defaultSetting(const GraphSettingDef& setting)
{
    return setting.count == 0 ? Json(std::string(setting.fallback))
                              : numbersJson(std::vector<float>(static_cast<size_t>(setting.count), 0.0f));
}

bool readNode(const Json& json, const std::string& path, GraphNode& node, std::string& error)
{
    if (!onlyKeys(json, path, {"id", "type", "position", "settings", "inputs"}, error))
        return false;
    if (!json.contains("id") || !json["id"].is_number_integer() || json["id"].get<int64_t>() <= 0 ||
        json["id"].get<int64_t>() > std::numeric_limits<int>::max())
        return fail(error, path + ".id", "must be a whole number > 0");
    node.id = json["id"].get<int>();
    const GraphNodeDef* def = json.contains("type") && json["type"].is_string() ? findGraphNodeDef(json["type"].get<std::string>()) : nullptr;
    if (!def)
        return fail(error, path + ".type", json.contains("type") ? std::format("unknown node type {}", json["type"].dump())
                                                                 : "missing (a node type)");
    node.type = def->type;
    if (json.contains("position") && !readArray(json["position"], path + ".position", node.position, error))
        return false;
    const Json settings = json.value("settings", Json::object());
    if (!settings.is_object())
        return fail(error, path + ".settings", "must be an object");
    for (const auto& [key, value] : settings.items())
        if (std::ranges::none_of(def->settings, [&](const GraphSettingDef& s) { return s.name == key; }))
            return fail(error, std::format("{}.settings.{}", path, key), std::format("unknown setting for {}", def->title));
    for (const GraphSettingDef& settingDef : def->settings) {
        const int count = settingDef.count;
        const std::string name(settingDef.name), at = std::format("{}.settings.{}", path, name);
        Json& setting = node.settings[name] = defaultSetting(settingDef);
        if (!settings.contains(name))
            continue;
        std::vector<float> values;
        if (count == 0 && !settings[name].is_string())
            return fail(error, at, "must be a string");
        if (count == 0)
            setting = settings[name];
        else if (!readFloats(settings[name], at, static_cast<size_t>(count), static_cast<size_t>(count), values, error))
            return false;
        else
            setting = numbersJson(values);
    }
    if (const auto [setting, message] = graphNodeSettingsError(node); !setting.empty())
        return fail(error, std::format("{}.settings.{}", path, setting), message);
    def = graphNodeDef(node); // a Custom Expression's pins come from its settings
    const Json inputs = json.value("inputs", Json::object());
    if (!inputs.is_object())
        return fail(error, path + ".inputs", "must be an object");
    for (const auto& [key, value] : inputs.items()) {
        const std::string at = std::format("{}.inputs.{}", path, key);
        const GraphPinDef* pin = findPin(def->inputs, key);
        if (!pin)
            return fail(error, at, std::format("{} has no input '{}'", def->title, key));
        if (def->output || pin->type == PinType::Texture2D)
            return fail(error, at, "this input takes a link, not a constant");
        const size_t lo = pin->type == PinType::Dynamic ? 1 : static_cast<size_t>(width(pin->type));
        const size_t hi = pin->type == PinType::Dynamic ? 4 : lo;
        if (!readFloats(value, at, lo, hi, node.inputs[key], error))
            return false;
    }
    return true;
}

bool readPin(const Json& json, const std::string& path, GraphPin& pin, std::string& error)
{
    if (!json.is_array() || json.size() != 2 || !json[0].is_number_integer() || !json[1].is_string())
        return fail(error, path, "must be [node id, \"pin\"]");
    pin = {json[0].get<int>(), json[1].get<std::string>()};
    return true;
}

// The id of a node on a cycle of links, or 0.
int cycleNode(const ShaderGraph& graph, const Index& index)
{
    std::map<int, int> state; // 1 on the path, 2 done
    const std::function<int(int)> visit = [&](int id) -> int {
        int& s = state[id];
        if (s == 1)
            return id;
        if (s == 2)
            return 0;
        s = 1;
        for (const auto& [key, link] : index.into)
            if (key.first == id)
                if (const int found = visit(link->from.node))
                    return found;
        state[id] = 2;
        return 0;
    };
    for (const GraphNode& node : graph.nodes)
        if (const int found = visit(node.id))
            return found;
    return 0;
}

// A setting's value as code: a string as it is (a param's name), numbers as a literal.
std::string settingCode(const GraphNode& node, const GraphNodeDef& def, const std::string& name)
{
    const auto it = node.settings.find(name);
    const auto setting = std::ranges::find(def.settings, name, &GraphSettingDef::name);
    const Json value = it != node.settings.end()       ? it->second
                       : setting == def.settings.end() ? Json("")
                                                        : defaultSetting(*setting);
    if (value.is_string())
        return value.get<std::string>();
    return literal(value.is_array() ? value.get<std::vector<float>>() : std::vector{value.get<float>()});
}

} // namespace

const std::vector<GraphNodeDef>& shaderGraphNodeDefs()
{
    using enum PinType;
    constexpr unsigned V = kStageVertex, F = kStageFragment, L = kStageLight;
    // Shorthands: a node of one output "out" from code; a math node of dynamic inputs (unlinked: their fallbacks) and
    // one dynamic output.
    const auto input = [](std::string_view type, std::string_view title, std::string_view category, unsigned stages,
                          PinType out, std::string_view code) {
        return GraphNodeDef{.type = type, .title = title, .category = category, .stages = stages, .outputs = {{{"out", out}, code}}};
    };
    const auto math = [](std::string_view type, std::string_view title, std::vector<GraphPinDef> inputs, std::string_view code) {
        return GraphNodeDef{.type = type, .title = title, .category = "Math", .inputs = std::move(inputs),
                            .outputs = {{{"out", Dynamic}, code}}};
    };
    // Value and gradient noise in 0..1 (the Noise node), 3D; the 2D kinds at z = 0. Dave Hoskins' sine-free hashes.
    // Every name starts with kast: params are macros and may not, so none can rename these.
    constexpr std::string_view noise = R"(float kastNoiseHash(vec3 kastP)
{
    kastP = fract(kastP * 0.1031);
    kastP += dot(kastP, kastP.zyx + 31.32);
    return fract((kastP.x + kastP.y) * kastP.z);
}

// Value noise: the hash at a cell corner. Gradient noise: a gradient there, dotted with the offset from it.
float kastNoiseCorner(vec3 kastCell, vec3 kastAt, vec3 kastCorner, bool kastGradient)
{
    if (!kastGradient)
        return kastNoiseHash(kastCell + kastCorner);
    vec3 kastP = fract((kastCell + kastCorner) * vec3(0.1031, 0.1030, 0.0973));
    kastP += dot(kastP, kastP.yxz + 33.33);
    return dot(fract((kastP.xxy + kastP.yxx) * kastP.zyx) * 2.0 - 1.0, kastAt - kastCorner);
}

float kastNoise(vec3 kastP, bool kastGradient)
{
    vec3 kastCell = floor(kastP);
    vec3 kastAt = fract(kastP);
    vec3 kastU = kastAt * kastAt * (3.0 - 2.0 * kastAt);
    float kastX00 = mix(kastNoiseCorner(kastCell, kastAt, vec3(0.0, 0.0, 0.0), kastGradient),
                        kastNoiseCorner(kastCell, kastAt, vec3(1.0, 0.0, 0.0), kastGradient), kastU.x);
    float kastX10 = mix(kastNoiseCorner(kastCell, kastAt, vec3(0.0, 1.0, 0.0), kastGradient),
                        kastNoiseCorner(kastCell, kastAt, vec3(1.0, 1.0, 0.0), kastGradient), kastU.x);
    float kastX01 = mix(kastNoiseCorner(kastCell, kastAt, vec3(0.0, 0.0, 1.0), kastGradient),
                        kastNoiseCorner(kastCell, kastAt, vec3(1.0, 0.0, 1.0), kastGradient), kastU.x);
    float kastX11 = mix(kastNoiseCorner(kastCell, kastAt, vec3(0.0, 1.0, 1.0), kastGradient),
                        kastNoiseCorner(kastCell, kastAt, vec3(1.0, 1.0, 1.0), kastGradient), kastU.x);
    float kastN = mix(mix(kastX00, kastX10, kastU.y), mix(kastX01, kastX11, kastU.y), kastU.z);
    return kastGradient ? saturate(0.5 + 0.5 * kastN) : kastN;
}

float kastNoise_value_2d(vec3 kastP) { return kastNoise(vec3(kastP.xy, 0.0), false); }
float kastNoise_value_3d(vec3 kastP) { return kastNoise(kastP, false); }
float kastNoise_gradient_2d(vec3 kastP) { return kastNoise(vec3(kastP.xy, 0.0), true); }
float kastNoise_gradient_3d(vec3 kastP) { return kastNoise(kastP, true); }
)";
    static const std::vector<GraphNodeDef> defs = {
        // Outputs
        {.type = "output_vertex", .title = "Vertex Output", .category = "Output", .stages = V,
         .inputs = {{"VERTEX", Vec3}, {"NORMAL", Vec3}, {"UV", Vec2}, {"COLOR", Vec4}}, .output = V},
        {.type = "output_fragment", .title = "Fragment Output", .category = "Output", .stages = F,
         .inputs = {{"ALBEDO", Vec3}, {"ALPHA", Float}, {"METALLIC", Float}, {"ROUGHNESS", Float}, {"EMISSION", Vec3},
                    {"NORMAL_MAP", Vec3}, {"NORMAL_MAP_DEPTH", Float}, {"AO", Float}},
         .output = F},
        {.type = "output_light", .title = "Light Output", .category = "Output", .stages = L,
         .inputs = {{"DIFFUSE_LIGHT", Vec3}, {"SPECULAR_LIGHT", Vec3}}, .output = L},
        // Constants and params
        {.type = "constant_float", .title = "Float", .category = "Constant", .outputs = {{{"out", Float}, "{value}"}},
         .settings = {{"value", 1}}},
        {.type = "constant_vec2", .title = "Vector 2", .category = "Constant", .outputs = {{{"out", Vec2}, "{value}"}},
         .settings = {{"value", 2}}},
        {.type = "constant_vec3", .title = "Vector 3", .category = "Constant", .outputs = {{{"out", Vec3}, "{value}"}},
         .settings = {{"value", 3}}},
        {.type = "constant_vec4", .title = "Vector 4", .category = "Constant", .outputs = {{{"out", Vec4}, "{value}"}},
         .settings = {{"value", 4}}},
        {.type = "constant_color", .title = "Color", .category = "Constant", .outputs = {{{"out", Vec4}, "{value}"}},
         .settings = {{"value", 4}}},
        {.type = "param", .title = "Param", .category = "Input", .outputs = {{{"out", Dynamic}, "{param}"}},
         .settings = {{"param"}}},
        // Inputs: the .kshader built-ins
        input("uv", "UV", "Input", V | F, Vec2, "UV"),
        input("time", "Time", "Input", kStageAll, Float, "TIME"),
        input("vertex_color", "Vertex Color", "Input", V | F, Vec4, "COLOR"),
        input("normal", "Normal (World)", "Input", F | L, Vec3, "NORMAL"),
        input("tangent", "Tangent (World)", "Input", F, Vec4, "TANGENT"),
        input("world_position", "World Position", "Input", F, Vec3, "WORLD_POSITION"),
        input("view_position", "View Position", "Input", F, Vec3, "VIEW_POSITION"),
        input("view_direction", "View Direction", "Input", F, Vec3, "normalize(CAMERA_POSITION - WORLD_POSITION)"),
        input("camera_position", "Camera Position", "Input", kStageAll, Vec3, "CAMERA_POSITION"),
        input("screen_uv", "Screen UV", "Input", F, Vec2, "SCREEN_UV"),
        input("frag_coord", "Frag Coord", "Input", F, Vec4, "FRAG_COORD"),
        input("screen_texture", "Screen Texture", "Input", F | L, Texture2D, "SCREEN_TEXTURE"),
        input("depth_texture", "Depth Texture", "Input", F | L, Texture2D, "DEPTH_TEXTURE"),
        // vertex() only
        input("vertex_position", "Vertex Position (Object)", "Vertex", V, Vec3, "VERTEX"),
        input("vertex_normal", "Vertex Normal (Object)", "Vertex", V, Vec3, "NORMAL"),
        input("instance_id", "Instance ID", "Vertex", V, Float, "float(INSTANCE_ID)"),
        // v (w 1: a point, 0: a direction) from object to world: MODEL_MATRIX, which WORLD_MATRIX also names
        {.type = "model_matrix", .title = "Model Matrix", .category = "Vertex", .stages = V,
         .inputs = {{"v", Vec3, "vec3_splat(0.0)"}, {"w", Float, "1.0"}},
         .outputs = {{{"out", Vec3}, "mul(MODEL_MATRIX, vec4({v}, {w})).xyz"}}},
        // The material's own values and textures, through the template's helpers
        {.type = "material", .title = "Material Values", .category = "Material",
         .outputs = {{{"base_color", Vec4}, "MATERIAL_BASE_COLOR"}, {{"metallic", Float}, "MATERIAL_METALLIC"},
                     {{"roughness", Float}, "MATERIAL_ROUGHNESS"}, {{"emission", Vec3}, "MATERIAL_EMISSION"},
                     {{"alpha_cutoff", Float}, "MATERIAL_ALPHA_CUTOFF"}}},
        {.type = "material_base_color", .title = "Material Base Color", .category = "Material", .stages = F,
         .inputs = {{"uv", Vec2, "UV"}},
         .outputs = {{{"rgba", Vec4}, "MATERIAL_BASE_COLOR * sampleBaseColor({uv})"}, {{"rgb", Vec3}, "{rgba}.rgb"},
                     {{"a", Float}, "{rgba}.a"}}},
        {.type = "material_metallic_roughness", .title = "Material Metallic Roughness", .category = "Material", .stages = F,
         .inputs = {{"uv", Vec2, "UV"}},
         .outputs = {{{"metallic", Float}, "MATERIAL_METALLIC * sampleMetallicRoughness({uv}).b"},
                     {{"roughness", Float}, "MATERIAL_ROUGHNESS * sampleMetallicRoughness({uv}).g"}}},
        {.type = "material_emission", .title = "Material Emission", .category = "Material", .stages = F,
         .inputs = {{"uv", Vec2, "UV"}}, .outputs = {{{"rgb", Vec3}, "MATERIAL_EMISSION * sampleEmissive({uv}).rgb"}}},
        {.type = "material_occlusion", .title = "Material Occlusion", .category = "Material", .stages = F,
         .inputs = {{"uv", Vec2, "UV"}}, .outputs = {{{"ao", Float}, "sampleOcclusion({uv}).r"}}},
        {.type = "material_normal_map", .title = "Material Normal Map", .category = "Material", .stages = F,
         .inputs = {{"uv", Vec2, "UV"}}, .outputs = {{{"normal_map", Vec3}, "sampleNormal({uv}).xyz"}}},
        // What fragment() starts from, and light()'s surface inputs
        {.type = "surface", .title = "Surface", .category = "Material", .stages = F | L,
         .outputs = {{{"albedo", Vec3}, "ALBEDO"}, {{"metallic", Float}, "METALLIC"}, {{"roughness", Float}, "ROUGHNESS"}}},
        // light() only
        input("light_direction", "Light Direction", "Light", L, Vec3, "LIGHT_DIRECTION"),
        input("light_color", "Light Color", "Light", L, Vec3, "LIGHT_COLOR"),
        input("attenuation", "Attenuation", "Light", L, Float, "ATTENUATION"),
        input("view", "View", "Light", L, Vec3, "VIEW"),
        // Math
        math("add", "Add", {{"a", Dynamic, "0.0"}, {"b", Dynamic, "0.0"}}, "{a} + {b}"),
        math("subtract", "Subtract", {{"a", Dynamic, "0.0"}, {"b", Dynamic, "0.0"}}, "{a} - {b}"),
        math("multiply", "Multiply", {{"a", Dynamic, "1.0"}, {"b", Dynamic, "1.0"}}, "{a} * {b}"),
        math("divide", "Divide", {{"a", Dynamic, "1.0"}, {"b", Dynamic, "1.0"}}, "{a} / {b}"),
        math("lerp", "Lerp", {{"a", Dynamic, "0.0"}, {"b", Dynamic, "1.0"}, {"t", Dynamic, "0.5"}}, "mix({a}, {b}, {t})"),
        math("clamp", "Clamp", {{"x", Dynamic, "0.0"}, {"min", Dynamic, "0.0"}, {"max", Dynamic, "1.0"}},
             "clamp({x}, {min}, {max})"),
        math("saturate", "Saturate", {{"x", Dynamic, "0.0"}}, "saturate({x})"),
        math("step", "Step", {{"edge", Dynamic, "0.5"}, {"x", Dynamic, "0.0"}}, "step({edge}, {x})"),
        math("smoothstep", "Smoothstep", {{"edge0", Dynamic, "0.0"}, {"edge1", Dynamic, "1.0"}, {"x", Dynamic, "0.0"}},
             "smoothstep({edge0}, {edge1}, {x})"),
        math("pow", "Power", {{"x", Dynamic, "0.0"}, {"exponent", Dynamic, "1.0"}}, "pow({x}, {exponent})"),
        math("sin", "Sine", {{"x", Dynamic, "0.0"}}, "sin({x})"),
        math("cos", "Cosine", {{"x", Dynamic, "0.0"}}, "cos({x})"),
        math("abs", "Absolute", {{"x", Dynamic, "0.0"}}, "abs({x})"),
        math("fract", "Fraction", {{"x", Dynamic, "0.0"}}, "fract({x})"),
        math("floor", "Floor", {{"x", Dynamic, "0.0"}}, "floor({x})"),
        math("min", "Minimum", {{"a", Dynamic, "0.0"}, {"b", Dynamic, "0.0"}}, "min({a}, {b})"),
        math("max", "Maximum", {{"a", Dynamic, "0.0"}, {"b", Dynamic, "0.0"}}, "max({a}, {b})"),
        math("one_minus", "One Minus", {{"x", Dynamic, "0.0"}}, "1.0 - {x}"),
        math("remap", "Remap",
             {{"x", Dynamic, "0.0"}, {"in_min", Dynamic, "0.0"}, {"in_max", Dynamic, "1.0"}, {"out_min", Dynamic, "0.0"},
              {"out_max", Dynamic, "1.0"}},
             "{out_min} + ({x} - {in_min}) * ({out_max} - {out_min}) / ({in_max} - {in_min})"),
        // Vector
        {.type = "dot", .title = "Dot Product", .category = "Vector", .inputs = {{"a", Dynamic, "0.0"}, {"b", Dynamic, "0.0"}},
         .outputs = {{{"out", Float}, "dot({a}, {b})"}}},
        {.type = "cross", .title = "Cross Product", .category = "Vector",
         .inputs = {{"a", Vec3, "vec3(1.0, 0.0, 0.0)"}, {"b", Vec3, "vec3(0.0, 1.0, 0.0)"}},
         .outputs = {{{"out", Vec3}, "cross({a}, {b})"}}},
        {.type = "normalize", .title = "Normalize", .category = "Vector", .inputs = {{"x", Dynamic, "1.0"}},
         .outputs = {{{"out", Dynamic}, "normalize({x})"}}},
        {.type = "length", .title = "Length", .category = "Vector", .inputs = {{"x", Dynamic, "0.0"}},
         .outputs = {{{"out", Float}, "length({x})"}}},
        {.type = "split", .title = "Split", .category = "Vector", .inputs = {{"v", Vec4, "vec4_splat(0.0)"}},
         .outputs = {{{"x", Float}, "{v}.x"}, {{"y", Float}, "{v}.y"}, {{"z", Float}, "{v}.z"}, {{"w", Float}, "{v}.w"}}},
        {.type = "combine", .title = "Combine", .category = "Vector",
         .inputs = {{"x", Float, "0.0"}, {"y", Float, "0.0"}, {"z", Float, "0.0"}, {"w", Float, "0.0"}},
         .outputs = {{{"xyzw", Vec4}, "vec4({x}, {y}, {z}, {w})"}, {{"xyz", Vec3}, "{xyzw}.xyz"}, {{"xy", Vec2}, "{xyzw}.xy"}}},
        // out: as wide as the mask
        {.type = "swizzle", .title = "Swizzle", .category = "Vector", .inputs = {{"v", Vec4, "vec4_splat(0.0)"}},
         .outputs = {{{"out", Dynamic}, "{v}.{mask}"}}, .settings = {{"mask", 0, "xyz"}}},
        // Texture and UV
        {.type = "sample_texture", .title = "Sample Texture", .category = "Texture", .stages = F,
         .inputs = {{"texture", Texture2D}, {"uv", Vec2, "UV"}},
         .outputs = {{{"rgba", Vec4}, "texture2D({texture}, {uv})"}, {{"rgb", Vec3}, "{rgba}.rgb"}, {{"r", Float}, "{rgba}.r"},
                     {{"g", Float}, "{rgba}.g"}, {{"b", Float}, "{rgba}.b"}, {{"a", Float}, "{rgba}.a"}}},
        // normal_map: as stored (0..1), what NORMAL_MAP takes; normal: unpacked to -1..1, tangent space
        {.type = "sample_normal_map", .title = "Sample Normal Map", .category = "Texture", .stages = F,
         .inputs = {{"texture", Texture2D}, {"uv", Vec2, "UV"}},
         .outputs = {{{"normal_map", Vec3}, "texture2D({texture}, {uv}).xyz"}, {{"normal", Vec3}, "{normal_map} * 2.0 - 1.0"}}},
        {.type = "tiling_offset", .title = "Tiling And Offset", .category = "Texture", .stages = V | F,
         .inputs = {{"uv", Vec2, "UV"}, {"tiling", Vec2, "vec2_splat(1.0)"}, {"offset", Vec2, "vec2_splat(0.0)"}},
         .outputs = {{{"out", Vec2}, "{uv} * {tiling} + {offset}"}}},
        {.type = "panner", .title = "Panner", .category = "Texture", .stages = V | F,
         .inputs = {{"uv", Vec2, "UV"}, {"speed", Vec2, "vec2(0.1, 0.0)"}, {"time", Float, "TIME"}},
         .outputs = {{{"out", Vec2}, "{uv} + {speed} * {time}"}}},
        // angle: radians, counterclockwise about center
        {.type = "rotate_uv", .title = "Rotate UV", .category = "Texture", .stages = V | F,
         .inputs = {{"uv", Vec2, "UV"}, {"center", Vec2, "vec2_splat(0.5)"}, {"angle", Float, "0.0"}},
         .outputs = {{{"out", Vec2},
                      "{center} + vec2(cos({angle}) * ({uv} - {center}).x - sin({angle}) * ({uv} - {center}).y, "
                      "sin({angle}) * ({uv} - {center}).x + cos({angle}) * ({uv} - {center}).y)"}}},
        // Effects
        {.type = "fresnel", .title = "Fresnel", .category = "Effect", .stages = F,
         .inputs = {{"normal", Vec3, "NORMAL"}, {"view", Vec3, "normalize(CAMERA_POSITION - WORLD_POSITION)"},
                    {"power", Float, "5.0"}},
         .outputs = {{{"out", Float}, "pow(1.0 - saturate(dot({normal}, {view})), {power})"}}},
        // kind: value_2d, value_3d, gradient_2d or gradient_3d (2D: of p.xy); out 0..1
        {.type = "noise", .title = "Noise", .category = "Effect", .stages = V | F,
         .inputs = {{"p", Vec3, "vec3(UV, 0.0)"}, {"scale", Float, "1.0"}},
         .outputs = {{{"out", Float}, "kastNoise_{kind}({p} * {scale})"}}, .settings = {{"kind", 0, "value_2d"}},
         .helper = noise},
        // depth: meters of opaque scene behind the surface; fade: depth over distance, saturated
        {.type = "depth_fade", .title = "Depth Fade", .category = "Effect", .stages = F, .inputs = {{"distance", Float, "1.0"}},
         .outputs = {{{"depth", Float}, "linearDepth(texture2D(DEPTH_TEXTURE, SCREEN_UV).r) + VIEW_POSITION.z"},
                     {{"fade", Float}, "saturate({depth} / {distance})"}}},
        {.type = "scene_color", .title = "Scene Color", .category = "Effect", .stages = F,
         .inputs = {{"offset", Vec2, "vec2_splat(0.0)"}},
         .outputs = {{{"rgb", Vec3}, "texture2D(SCREEN_TEXTURE, SCREEN_UV + {offset}).rgb"}}},
        {.type = "ndotl", .title = "N dot L", .category = "Effect", .stages = L,
         .inputs = {{"normal", Vec3, "NORMAL"}, {"light", Vec3, "LIGHT_DIRECTION"}},
         .outputs = {{{"out", Float}, "saturate(dot({normal}, {light}))"}}},
        // x in bands steps, rounded up (softness 0: ceil(x * bands) / bands); softness > 0 ramps up each step over that
        // fraction of a band
        {.type = "toon_bands", .title = "Toon Bands", .category = "Effect",
         .inputs = {{"x", Float, "0.0"}, {"bands", Float, "3.0"}, {"softness", Float, "0.0"}},
         .outputs = {{{"out", Float},
                      "({softness} > 0.0 ? floor({x} * {bands}) + smoothstep(0.0, {softness}, fract({x} * {bands})) "
                      ": ceil({x} * {bands})) / {bands}"}}},
        // Pins from its settings (graphNodeDef); generateKShader writes its code.
        {.type = kCustomExpression, .title = "Custom Expression", .category = "Custom",
         .settings = {{"inputs"}, {"outputs"}, {"code"}}},
    };
    return defs;
}

const GraphNodeDef* findGraphNodeDef(std::string_view type)
{
    const auto it = std::ranges::find(shaderGraphNodeDefs(), type, &GraphNodeDef::type);
    return it == shaderGraphNodeDefs().end() ? nullptr : &*it;
}

namespace {

std::string stringSetting(const GraphNode& node, const std::string& name)
{
    const auto it = node.settings.find(name);
    return it != node.settings.end() && it->second.is_string() ? it->second.get<std::string>() : "";
}

// A Custom Expression's pin list ("vec3 a, float b") as {type, name}; error if malformed. taken: names already used.
bool customPins(std::string_view list, bool inputs, std::vector<std::pair<PinType, std::string>>& out,
                std::set<std::string>& taken, std::string& error)
{
    static const std::regex pin(R"(^\s*(\w+)\s+([A-Za-z_]\w*)\s*$)");
    static const std::regex temp(R"(^n\d+_)"); // generateKShader's temp variables
    if (list.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return true; // no pins
    for (size_t start = 0; start < list.size() + 1;) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string item(list.substr(start, end - start));
        start = end + 1;
        std::smatch m;
        if (!std::regex_match(item, m, pin)) {
            error = std::format("'{}' is not '<type> <name>' (e.g. \"vec3 color, float amount\")", item);
            return false;
        }
        const std::string type = m[1].str(), name = m[2].str();
        constexpr std::string_view kTypes[] = {"float", "vec2", "vec3", "vec4", "texture2D"};
        const auto it = std::ranges::find(kTypes, type);
        if (it == std::end(kTypes) || (!inputs && type == "texture2D")) {
            error = std::format("'{}' is not a type: {}", type, inputs ? "float, vec2, vec3, vec4 or texture2D" : "float, vec2, vec3 or vec4");
            return false;
        }
        if (std::ranges::find(shaderBuiltinNames(), name) != shaderBuiltinNames().end() || std::regex_search(name, temp)) {
            error = std::format("'{}' is a reserved name (a built-in, or a generated n<id>_ temp)", name);
            return false;
        }
        if (!taken.insert(name).second) {
            error = std::format("'{}' is already a pin of this node", name);
            return false;
        }
        out.emplace_back(static_cast<PinType>(it - std::begin(kTypes)), name);
    }
    return true;
}

} // namespace

std::pair<std::string, std::string> graphNodeSettingsError(const GraphNode& node)
{
    if (node.type == "swizzle") {
        static const std::regex mask(R"(^([xyzw]{1,4}|[rgba]{1,4})$)");
        if (!std::regex_match(stringSetting(node, "mask"), mask))
            return {"mask", "must be 1 to 4 of x, y, z, w (or of r, g, b, a), e.g. \"zyx\""};
    } else if (node.type == "noise") {
        const std::string kind = stringSetting(node, "kind");
        if (kind != "value_2d" && kind != "value_3d" && kind != "gradient_2d" && kind != "gradient_3d")
            return {"kind", "must be \"value_2d\", \"value_3d\", \"gradient_2d\" or \"gradient_3d\""};
    } else if (node.type == kCustomExpression) {
        std::vector<std::pair<PinType, std::string>> pins;
        std::set<std::string> taken;
        std::string error;
        for (const char* setting : {"inputs", "outputs"})
            if (!customPins(stringSetting(node, setting), setting[0] == 'i', pins, taken, error))
                return {setting, error};
    }
    return {};
}

const GraphNodeDef* graphNodeDef(const GraphNode& node)
{
    const GraphNodeDef* def = findGraphNodeDef(node.type);
    if (!def || node.type != kCustomExpression)
        return def;
    // A def per pin signature, built once: pin names point into its strings.
    // ponytail: never evicted; one entry per distinct signature typed in a session, small.
    struct Custom {
        std::deque<std::string> strings;
        GraphNodeDef def;
    };
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<Custom>> cache;
    const std::string inputs = stringSetting(node, "inputs"), outputs = stringSetting(node, "outputs");
    const std::lock_guard lock(mutex);
    auto& entry = cache[inputs + '\n' + outputs];
    if (entry)
        return &entry->def;
    std::vector<std::pair<PinType, std::string>> in, out;
    std::set<std::string> taken;
    std::string error;
    if (!customPins(inputs, true, in, taken, error) || !customPins(outputs, false, out, taken, error))
        return nullptr;
    entry = std::make_unique<Custom>();
    entry->def = *def;
    const auto keep = [&](std::string text) -> std::string_view { return entry->strings.emplace_back(std::move(text)); };
    for (const auto& [type, name] : in) // unlinked: zero
        entry->def.inputs.push_back({keep(name), type,
                                     type == PinType::Texture2D ? ""
                                     : type == PinType::Float   ? "0.0"
                                                                : keep(std::format("vec{}_splat(0.0)", width(type)))});
    for (const auto& [type, name] : out)
        entry->def.outputs.push_back({{keep(name), type}, ""});
    return &entry->def;
}

bool shaderGraphFromJson(const Json& json, ShaderGraph& out, std::string& error)
{
    if (!json.is_object()) {
        error = "must be a JSON object";
        return false;
    }
    if (!onlyKeys(json, "", {"version", "params", "nodes", "links", "comments"}, error))
        return false;
    if (!json.contains("version") || json["version"] != kShaderGraphVersion)
        return fail(error, "version", std::format("must be {}", kShaderGraphVersion));
    ShaderGraph graph;
    for (const char* list : {"params", "nodes", "links", "comments"})
        if (json.contains(list) && !json[list].is_array())
            return fail(error, list, "must be an array");
    const Json empty = Json::array();
    const auto list = [&](const char* name) -> const Json& { return json.contains(name) ? json[name] : empty; };
    for (size_t i = 0; i < list("params").size(); ++i)
        if (!readParam(list("params")[i], std::format("params[{}]", i), graph.params.emplace_back(), error))
            return false;
    std::set<int> ids;
    for (size_t i = 0; i < list("nodes").size(); ++i) {
        const std::string path = std::format("nodes[{}]", i);
        GraphNode& node = graph.nodes.emplace_back();
        if (!readNode(list("nodes")[i], path, node, error))
            return false;
        if (!ids.insert(node.id).second)
            return fail(error, path + ".id", std::format("{} is already another node's id", node.id));
    }
    const Index nodes(graph);
    std::set<std::pair<int, std::string>> linked;
    for (size_t i = 0; i < list("links").size(); ++i) {
        const std::string path = std::format("links[{}]", i);
        const Json& link = list("links")[i];
        GraphLink& added = graph.links.emplace_back();
        if (!onlyKeys(link, path, {"from", "to"}, error) || !readPin(link.value("from", Json()), path + ".from", added.from, error) ||
            !readPin(link.value("to", Json()), path + ".to", added.to, error))
            return false;
        for (const auto& [pin, end, output] : {std::tuple{&added.from, ".from", true}, std::tuple{&added.to, ".to", false}}) {
            const GraphNode* node = nodes.node(pin->node);
            if (!node)
                return fail(error, path + end, std::format("no node {}", pin->node));
            const GraphNodeDef& def = *graphNodeDef(*node);
            if (output ? !findOutput(def, pin->pin) : !findPin(def.inputs, pin->pin))
                return fail(error, path + end, std::format("{} (node {}) has no {} '{}'", def.title, node->id,
                                                           output ? "output" : "input", pin->pin));
        }
        if (!linked.insert({added.to.node, added.to.pin}).second)
            return fail(error, path + ".to", "that input already has a link");
    }
    if (const int node = cycleNode(graph, Index(graph)))
        return fail(error, std::format("node {}", node), "the links form a cycle");
    for (size_t i = 0; i < list("comments").size(); ++i) {
        const std::string path = std::format("comments[{}]", i);
        const Json& comment = list("comments")[i];
        GraphComment& c = graph.comments.emplace_back();
        if (!onlyKeys(comment, path, {"title", "rect"}, error))
            return false;
        if (comment.contains("title") && !comment["title"].is_string())
            return fail(error, path + ".title", "must be a string");
        c.title = comment.value("title", "");
        if (comment.contains("rect") && !readArray(comment["rect"], path + ".rect", c.rect, error))
            return false;
        if (c.rect[2] < 0.0f || c.rect[3] < 0.0f)
            return fail(error, path + ".rect", "width and height must be >= 0");
    }
    out = std::move(graph);
    return true;
}

bool parseShaderGraph(std::string_view text, ShaderGraph& out, std::string& error)
{
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        error = "invalid JSON";
        return false;
    }
    return shaderGraphFromJson(json, out, error);
}

Json shaderGraphToJson(const ShaderGraph& graph)
{
    Json json = {{"version", kShaderGraphVersion}, {"params", Json::array()}, {"nodes", Json::array()},
                 {"links", Json::array()}, {"comments", Json::array()}};
    for (const ShaderParam& param : graph.params)
        json["params"].push_back(paramJson(param));
    for (const GraphNode& node : graph.nodes) {
        Json settings = Json::object(), inputs = Json::object();
        for (const auto& [name, value] : node.settings)
            settings[name] = value;
        for (const auto& [name, values] : node.inputs)
            inputs[name] = numbersJson(values);
        json["nodes"].push_back({{"id", node.id}, {"type", node.type},
                                 {"position", numbersJson({node.position[0], node.position[1]})},
                                 {"settings", settings}, {"inputs", inputs}});
    }
    for (const GraphLink& link : graph.links)
        json["links"].push_back({{"from", Json::array({link.from.node, link.from.pin})},
                                 {"to", Json::array({link.to.node, link.to.pin})}});
    for (const GraphComment& comment : graph.comments)
        json["comments"].push_back(
            {{"title", comment.title}, {"rect", numbersJson({comment.rect.begin(), comment.rect.end()})}});
    return json;
}

std::string shaderGraphText(const ShaderGraph& graph)
{
    return shaderGraphToJson(graph).dump(2) + "\n";
}

PinType outputPinType(const ShaderGraph& graph, const GraphPin& pin)
{
    const Index index(graph);
    return Types(graph, index).output(pin);
}

LinkCheck canLink(const ShaderGraph& graph, const GraphPin& from, const GraphPin& to)
{
    const Index index(graph);
    const GraphNode* fromNode = index.node(from.node);
    const GraphNode* toNode = index.node(to.node);
    const GraphNodeDef* fromDef = fromNode ? graphNodeDef(*fromNode) : nullptr;
    const GraphNodeDef* toDef = toNode ? graphNodeDef(*toNode) : nullptr;
    const GraphPinDef* input = toDef ? findPin(toDef->inputs, to.pin) : nullptr;
    if (!fromDef || !findOutput(*fromDef, from.pin) || !input)
        return {.message = "no such pins"};
    const PinType type = Types(graph, index).output(from);
    if ((type == PinType::Texture2D) != (input->type == PinType::Texture2D))
        return {.message = std::format("a {} can't go into a {} input", pinTypeName(type), pinTypeName(input->type))};
    if (reaches(index, to.node, from.node))
        return {.message = "the link would close a cycle"};
    if (input->type != PinType::Dynamic && width(type) > width(input->type))
        return {.allowed = true, .warning = true,
                .message = std::format("{} truncated to {}", pinTypeName(type), pinTypeName(input->type))};
    return {.allowed = true};
}

std::string generateKShader(const ShaderGraph& graph, std::vector<GraphIssue>& issues)
{
    issues.clear();
    const Index index(graph);
    Types types(graph, index);
    const auto error = [&](int node, std::string message) { issues.push_back({node, std::move(message)}); };

    // The output node of each stage that has a linked input, and the nodes upstream of it (by id, so node order in the
    // file doesn't matter).
    std::map<unsigned, const GraphNode*> outputs;
    for (const auto& [id, node] : index.nodes) {
        const GraphNodeDef* def = graphNodeDef(*node);
        if (const auto [setting, message] = graphNodeSettingsError(*node); !setting.empty())
            error(id, std::format("{} {}", setting, message));
        else if (!def)
            error(id, std::format("unknown node type '{}'", node->type));
        else if (def->output && !outputs.emplace(def->output, node).second)
            error(id, std::format("a second {}: a graph has at most one", def->title));
    }
    std::map<int, unsigned> stagesOf; // node -> the stages it is emitted in
    std::map<unsigned, std::set<int>> emitted;
    for (const auto& [stage, output] : outputs) {
        std::vector<int> stack;
        for (const auto& [key, link] : index.into)
            if (key.first == output->id)
                stack.push_back(output->id);
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            if (!index.node(id) || !emitted[stage].insert(id).second)
                continue;
            stagesOf[id] |= stage;
            for (const auto& [key, link] : index.into)
                if (key.first == id)
                    stack.push_back(link->from.node);
        }
    }

    // Each emitted node once: where it may run, its inputs, its param.
    for (const auto& [id, stages] : stagesOf) {
        const GraphNode& node = *index.node(id);
        const GraphNodeDef* def = graphNodeDef(node);
        if (!def)
            continue;
        if (stages & ~def->stages)
            error(id, std::format("{} is not available in {} (only in {})", def->title, stageNames(stages & ~def->stages),
                                  stageNames(def->stages)));
        if (node.type == kCustomExpression) // its pins are locals; a param is a macro that would rename one
            for (const ShaderParam& param : graph.params)
                if (findPin(def->inputs, param.name) || findOutput(*def, param.name))
                    error(id, std::format("pin '{}' has the name of a param", param.name));
        if (node.type == "param" && !nodeParam(graph, node)) {
            const auto it = node.settings.find("param");
            const std::string name = it != node.settings.end() && it->second.is_string() ? it->second.get<std::string>() : "";
            error(id, name.empty() ? "no param chosen" : std::format("the graph has no param '{}'", name));
        }
        for (const GraphPinDef& pin : def->inputs) {
            const GraphLink* link = index.linkInto(id, pin.name);
            if (!link) {
                if (pin.fallback.empty() && !def->output && !node.inputs.contains(std::string(pin.name)))
                    error(id, std::format("input '{}' needs a link", pin.name));
                continue;
            }
            const PinType from = types.output(link->from);
            if (from == PinType::Dynamic)
                error(id, std::format("input '{}': node {} has no output '{}'", pin.name, link->from.node, link->from.pin));
            else if ((from == PinType::Texture2D) != (pin.type == PinType::Texture2D))
                error(id, std::format("input '{}': a {} can't go into a {} input", pin.name, pinTypeName(from),
                                      pinTypeName(pin.type)));
            else if (pin.type != PinType::Dynamic && width(from) > width(pin.type))
                issues.push_back({id, std::format("input '{}': {} truncated to {}", pin.name, pinTypeName(from),
                                                  pinTypeName(pin.type)),
                                  true});
        }
    }
    if (std::ranges::any_of(issues, [](const GraphIssue& i) { return !i.warning; }))
        return {};

    // The code of an output pin, by node and pin: its temp variable (a texture's: the param itself).
    std::map<std::pair<int, std::string>, std::string> code;
    const auto inputCode = [&](const GraphNode& node, const GraphPinDef& pin, PinType target) {
        if (const GraphLink* link = index.linkInto(node.id, pin.name))
            return convert(code.at({link->from.node, link->from.pin}), types.output(link->from), target);
        if (const auto it = node.inputs.find(std::string(pin.name)); it != node.inputs.end())
            return convert(literal(it->second), numberType(static_cast<int>(it->second.size())), target);
        return convert(std::string(pin.fallback), pin.type == PinType::Dynamic ? PinType::Float : pin.type, target);
    };
    const auto emitNode = [&](const GraphNode& node, unsigned stage) {
        const GraphNodeDef& def = *graphNodeDef(node);
        std::string out = std::format("    // @node {} {}\n", node.id, node.type);
        if (def.output) {
            for (const GraphPinDef& pin : def.inputs)
                if (index.linkInto(node.id, pin.name))
                    out += std::format("    {} {} {};\n", pin.name, stage == kStageLight ? "+=" : "=",
                                       inputCode(node, pin, pin.type));
            return out;
        }
        if (node.type == kCustomExpression) {
            // Its outputs declared, then its body in a block of its own with the inputs and outputs as locals (texture
            // inputs: #defines), indented so no body line looks like a marker or a function's brace.
            std::string block = "    {\n", undefine;
            for (const GraphPinDef& pin : def.inputs) {
                const std::string value = inputCode(node, pin, pin.type);
                if (pin.type == PinType::Texture2D) {
                    out += std::format("#define {} {}\n", pin.name, value);
                    undefine += std::format("#undef {}\n", pin.name);
                } else {
                    block += std::format("        {} {} = {};\n", pinTypeName(pin.type), pin.name, value);
                }
            }
            for (const auto& [pin, unused] : def.outputs) {
                out += std::format("    {} n{}_{};\n", pinTypeName(pin.type), node.id, pin.name);
                block += std::format("        {} {} = {};\n", pinTypeName(pin.type), pin.name,
                                     pin.type == PinType::Float ? "0.0" : std::format("{}_splat(0.0)", pinTypeName(pin.type)));
                code[{node.id, std::string(pin.name)}] = std::format("n{}_{}", node.id, pin.name);
            }
            const std::string body = settingCode(node, def, "code");
            for (size_t start = 0; start < body.size();) {
                const size_t end = std::min(body.find('\n', start), body.size());
                std::string line = body.substr(start, end - start);
                std::erase(line, '\r');
                block += line.empty() ? "\n" : "        " + line + "\n";
                start = end + 1;
            }
            for (const auto& [pin, unused] : def.outputs)
                block += std::format("        n{}_{} = {};\n", node.id, pin.name, pin.name);
            return out + block + "    }\n" + undefine;
        }
        for (const auto& [pin, pattern] : def.outputs) {
            // "{name}" -> an input's code, an earlier output's variable or a setting's value.
            std::string text;
            for (size_t i = 0; i < pattern.size();) {
                const size_t open = pattern.find('{', i), close = pattern.find('}', open);
                if (open == std::string_view::npos) {
                    text += pattern.substr(i);
                    break;
                }
                text += pattern.substr(i, open - i);
                const std::string name(pattern.substr(open + 1, close - open - 1));
                if (const GraphPinDef* in = findPin(def.inputs, name))
                    text += inputCode(node, *in, in->type == PinType::Dynamic ? types.dynamic(node) : in->type);
                else if (const auto it = code.find({node.id, name}); it != code.end())
                    text += it->second;
                else
                    text += settingCode(node, def, name);
                i = close + 1;
            }
            const PinType type = types.output(node, pin);
            if (type == PinType::Texture2D) {
                code[{node.id, std::string(pin.name)}] = text;
                continue;
            }
            const std::string var = std::format("n{}_{}", node.id, pin.name);
            out += std::format("    {} {} = {};\n", pinTypeName(type), var, text);
            code[{node.id, std::string(pin.name)}] = var;
        }
        return out;
    };
    // A stage's function: its nodes in topological order, the lowest id first among those ready.
    const auto function = [&](unsigned stage, std::string_view signature) {
        std::string out = std::format("{}\n{{\n", signature);
        const std::set<int>& nodes = emitted[stage];
        std::map<int, int> waiting; // node -> inputs linked from nodes not yet emitted
        for (const int id : nodes)
            for (const auto& [key, link] : index.into)
                if (key.first == id && nodes.contains(link->from.node))
                    ++waiting[id];
        std::set<int> ready;
        for (const int id : nodes)
            if (!waiting[id])
                ready.insert(id);
        while (!ready.empty()) {
            const int id = *ready.begin();
            ready.erase(ready.begin());
            out += emitNode(*index.node(id), stage);
            for (const auto& [key, link] : index.into)
                if (link->from.node == id && nodes.contains(key.first) && --waiting[key.first] == 0)
                    ready.insert(key.first);
        }
        return out + "}\n";
    };

    std::string text;
    for (const ShaderParam& param : graph.params) {
        text += std::format("param {} {}", shaderParamTypeName(param.type), param.name);
        if (param.type == ShaderParamType::Texture2D) {
            text += std::format(" = {}{};\n", textureDefaultName(param.texture), param.srgb ? " srgb" : "");
            continue;
        }
        const int count = param.type == ShaderParamType::Color ? 4 : componentCount(param.type);
        std::string value = std::format("{}", param.value[0]);
        if (param.type != ShaderParamType::Float) {
            value = std::format("{}({}", shaderParamTypeName(param.type), value);
            for (int i = 1; i < count; ++i)
                value += std::format(", {}", param.value[static_cast<size_t>(i)]);
            value += ")";
        }
        text += std::format(" = {}{};\n", value,
                            param.range ? std::format(" range({}, {})", (*param.range)[0], (*param.range)[1]) : "");
    }
    if (!graph.params.empty())
        text += "\n";
    for (const GraphNodeDef& def : shaderGraphNodeDefs()) // in table order, so the text doesn't depend on node order
        if (!def.helper.empty() && std::ranges::any_of(stagesOf, [&](const auto& n) { return index.node(n.first)->type == def.type; }))
            text += std::string(def.helper) + "\n";
    text += function(kStageFragment, "void fragment()");
    if (outputs.contains(kStageVertex) && !emitted[kStageVertex].empty())
        text += "\n" + function(kStageVertex, "void vertex()");
    if (outputs.contains(kStageLight) && !emitted[kStageLight].empty())
        text += "\n" + function(kStageLight, "void light()");

    // The params' names and limits: parseShader's rules, its errors put on the param's first Param node (or, past the
    // param lines, e.g. a Custom Expression body line starting with "param", on the node there).
    ShaderSource source;
    if (std::string why; !parseShader(text, source, why)) {
        static const std::regex lineError(R"(^line (\d+): (.*)$)");
        std::smatch m;
        int node = 0;
        if (std::regex_match(why, m, lineError)) {
            const auto line = static_cast<size_t>(std::stoi(m[1].str()));
            why = m[2].str();
            for (const GraphNode& n : graph.nodes) // the lowest id
                if (line >= 1 && line <= graph.params.size() && n.type == "param" &&
                    nodeParam(graph, n) == &graph.params[line - 1] && (node == 0 || n.id < node))
                    node = n.id;
            if (line > graph.params.size())
                node = graphNodeAtLine(text, static_cast<int>(line));
        }
        error(node, why);
        return {};
    }
    return text;
}

namespace {

// What follows "// @node " on the marker of the node whose code is at line: "<id> <type>"; "" outside every node.
std::string_view markerAt(std::string_view kshaderText, int line)
{
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= kshaderText.size();) {
        const size_t end = std::min(kshaderText.find('\n', start), kshaderText.size());
        lines.push_back(kshaderText.substr(start, end - start));
        start = end + 1;
    }
    for (int i = std::min(line, static_cast<int>(lines.size())) - 1; i >= 0; --i) {
        const std::string_view l = lines[static_cast<size_t>(i)];
        if (l.starts_with("    // @node "))
            return l.substr(13);
        if (l == "{" || l == "}")
            return {};
    }
    return {};
}

} // namespace

int graphNodeAtLine(std::string_view kshaderText, int line)
{
    return std::atoi(std::string(markerAt(kshaderText, line)).c_str());
}

std::string graphNodeNameAtLine(std::string_view kshaderText, int line)
{
    const std::string_view marker = markerAt(kshaderText, line);
    const size_t space = marker.find(' ');
    if (marker.empty() || space == std::string_view::npos)
        return "";
    const GraphNodeDef* def = findGraphNodeDef(marker.substr(space + 1));
    return std::format("node {} ({})", marker.substr(0, space), def ? def->title : marker.substr(space + 1));
}

std::string mapGraphErrors(std::string_view kshaderText, std::string_view errors)
{
    static const std::regex lineError(R"(^line (\d+): )");
    std::string out;
    for (size_t start = 0; start < errors.size();) {
        const size_t end = std::min(errors.find('\n', start), errors.size());
        const std::string line(errors.substr(start, end - start));
        std::smatch m;
        const int node = std::regex_search(line, m, lineError) ? graphNodeAtLine(kshaderText, std::stoi(m[1].str())) : 0;
        out += (out.empty() ? "" : "\n") + (node ? std::format("node {}: ", node) : "") + line;
        start = end + 1;
    }
    return out;
}

bool isShaderGraphPath(std::string_view path)
{
    return path.ends_with(kShaderGraphExtension);
}

bool shaderFileText(std::string_view path, std::string& text, std::string& error, std::vector<std::string>* warnings)
{
    if (!isShaderGraphPath(path))
        return true;
    ShaderGraph graph;
    if (!parseShaderGraph(text, graph, error))
        return false;
    std::vector<GraphIssue> issues;
    std::string generated = generateKShader(graph, issues);
    error.clear();
    for (const GraphIssue& issue : issues) {
        const std::string line = issue.node ? std::format("node {}: {}", issue.node, issue.message) : issue.message;
        if (!issue.warning)
            error += (error.empty() ? "" : "\n") + line;
        else if (warnings)
            warnings->push_back(line);
    }
    if (!error.empty())
        return false;
    text = std::move(generated);
    return true;
}

} // namespace kast
