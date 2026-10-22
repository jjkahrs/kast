#include "shader.h"

#include "files.h"
#include "log.h"
#include "project.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>

namespace kast {
namespace {

constexpr std::string_view kUserLine = "//@user"; // where generateShader puts a .kshader in a template

constexpr std::array<std::pair<ShaderParamType, std::string_view>, 6> kTypes = {{
    {ShaderParamType::Float, "float"},
    {ShaderParamType::Vec2, "vec2"},
    {ShaderParamType::Vec3, "vec3"},
    {ShaderParamType::Vec4, "vec4"},
    {ShaderParamType::Color, "color"},
    {ShaderParamType::Texture2D, "texture2D"},
}};

std::string_view typeName(ShaderParamType type)
{
    return std::ranges::find(kTypes, type, &std::pair<ShaderParamType, std::string_view>::first)->second;
}

// Names a param would break as a #define in the user code: types, statement keywords and texture functions; and the
// prefixes of bgfx's and the template's own identifiers.
constexpr std::string_view kKeywords[] = {
    "void", "bool", "int", "uint", "float", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4",
    "mat2", "mat3", "mat4", "true", "false", "if", "else", "for", "while", "do", "return", "break", "continue",
    "discard", "struct", "uniform", "const", "in", "out", "inout", "static", "sampler2D", "texture2D", "texture2DLod",
    "SAMPLER2D", "mul", "main"};
constexpr std::string_view kReservedPrefixes[] = {"u_", "s_", "v_", "a_", "i_", "gl_", "bgfx", "kast", "Kast"};

bool isIdentifier(std::string_view s)
{
    return !s.empty() && (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
           std::ranges::all_of(s, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// A param line's tokens: identifiers, numbers (with sign) and single punctuation characters; stops at "//".
std::vector<std::string> tokenize(std::string_view line)
{
    std::vector<std::string> tokens;
    for (size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (line.substr(i, 2) == "//") {
            break;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = i;
            while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
                ++end;
            tokens.emplace_back(line.substr(i, end - i));
            i = end;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
                   ((c == '-' || c == '+') && i + 1 < line.size() &&
                    (std::isdigit(static_cast<unsigned char>(line[i + 1])) || line[i + 1] == '.'))) {
            size_t end = i + 1;
            const auto exponentSign = [&](size_t k) {
                return (line[k] == '-' || line[k] == '+') && (line[k - 1] == 'e' || line[k - 1] == 'E');
            };
            while (end < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '.' || exponentSign(end)))
                ++end;
            tokens.emplace_back(line.substr(i, end - i));
            i = end;
        } else {
            tokens.emplace_back(1, c);
            ++i;
        }
    }
    return tokens;
}

std::optional<float> number(const std::string& token)
{
    std::string_view s = token;
    if (s.starts_with('+'))
        s.remove_prefix(1);
    if (s.ends_with('f') || s.ends_with('F')) // GLSL-style 1.0f
        s.remove_suffix(1);
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// Parses one param line (after "param"). False + error (without the line number) if it is malformed.
bool parseParam(const std::vector<std::string>& tokens, ShaderParam& param, std::string& error)
{
    size_t i = 1; // tokens[0] is "param"
    const auto at = [&](size_t k) -> std::string_view { return k < tokens.size() ? std::string_view(tokens[k]) : ""; };
    const auto expect = [&](std::string_view what) {
        if (at(i) == what) {
            ++i;
            return true;
        }
        error = std::format("expected '{}'{}", what, at(i).empty() ? " at the end" : std::format(", found '{}'", at(i)));
        return false;
    };
    const auto it = std::ranges::find(kTypes, at(i), &std::pair<ShaderParamType, std::string_view>::second);
    if (it == kTypes.end()) {
        error = std::format("unknown param type '{}': float, vec2, vec3, vec4, color or texture2D", at(i));
        return false;
    }
    param.type = it->first;
    ++i;
    if (!isIdentifier(at(i))) {
        error = at(i).empty() ? "expected a param name" : std::format("'{}' is not a valid param name", at(i));
        return false;
    }
    param.name = tokens[i++];
    const int count = componentCount(param.type);
    param.value = {0.0f, 0.0f, 0.0f, 0.0f};
    if (param.type == ShaderParamType::Color)
        param.value = {1.0f, 1.0f, 1.0f, 1.0f};
    if (at(i) == "=") {
        ++i;
        if (param.type == ShaderParamType::Texture2D) {
            constexpr std::pair<std::string_view, TextureDefault> kDefaults[] = {
                {"white", TextureDefault::White}, {"black", TextureDefault::Black}, {"normal", TextureDefault::Normal}};
            const auto d = std::ranges::find(kDefaults, at(i), &std::pair<std::string_view, TextureDefault>::first);
            if (d == std::end(kDefaults)) {
                error = std::format("a texture2D default must be white, black or normal, not '{}'", at(i));
                return false;
            }
            param.texture = d->second;
            ++i;
        } else if (param.type == ShaderParamType::Float) {
            const std::optional<float> v = number(std::string(at(i)));
            if (!v) {
                error = std::format("a float default must be a number, not '{}'", at(i));
                return false;
            }
            param.value[0] = *v;
            ++i;
        } else {
            // vecN(...) or color(...): N numbers (color: 3 or 4), or one for all of them.
            if (!expect(typeName(param.type)) || !expect("("))
                return false;
            std::vector<float> values;
            for (;;) {
                const std::optional<float> v = number(std::string(at(i)));
                if (!v) {
                    error = std::format("expected a number in {}(...), found '{}'", typeName(param.type), at(i));
                    return false;
                }
                values.push_back(*v);
                ++i;
                if (at(i) != ",")
                    break;
                ++i;
            }
            if (!expect(")"))
                return false;
            const bool colorRgb = param.type == ShaderParamType::Color && values.size() == 3;
            if (values.size() != 1 && values.size() != static_cast<size_t>(count) && !colorRgb) {
                error = std::format("{}(...) takes {} numbers{}, not {}", typeName(param.type), count,
                                    param.type == ShaderParamType::Color ? " (or 3)" : "", values.size());
                return false;
            }
            if (values.size() == 1)
                values.assign(static_cast<size_t>(count), values[0]);
            std::ranges::copy(values, param.value.begin()); // color(r, g, b): alpha stays 1
        }
    }
    if (at(i) == "range") {
        if (param.type != ShaderParamType::Float) {
            error = "range(min, max) is only for float params";
            return false;
        }
        ++i;
        std::array<float, 2> bounds{};
        for (size_t k = 0; k < 2; ++k) {
            if (!expect(k == 0 ? "(" : ","))
                return false;
            const std::optional<float> v = number(std::string(at(i)));
            if (!v) {
                error = std::format("expected a number in range(min, max), found '{}'", at(i));
                return false;
            }
            bounds[k] = *v;
            ++i;
        }
        if (!expect(")"))
            return false;
        if (!(bounds[0] < bounds[1])) {
            error = "range(min, max) needs min < max";
            return false;
        }
        param.range = bounds;
    }
    if (at(i) == "srgb") {
        if (param.type != ShaderParamType::Texture2D) {
            error = "srgb is only for texture2D params";
            return false;
        }
        param.srgb = true;
        ++i;
    }
    if (!expect(";"))
        return false;
    if (i != tokens.size()) {
        error = std::format("unexpected '{}' after ';'", at(i));
        return false;
    }
    return true;
}

// code with its comments replaced by spaces (newlines kept), so offsets and lines still match.
std::string stripComments(std::string_view code)
{
    std::string out(code);
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i] == '/' && out[i + 1] == '/') {
            for (; i < out.size() && out[i] != '\n'; ++i)
                out[i] = ' ';
        } else if (out[i] == '/' && out[i + 1] == '*') {
            const size_t end = out.find("*/", i + 2);
            const size_t stop = end == std::string::npos ? out.size() : end + 2;
            for (; i < stop; ++i)
                if (out[i] != '\n')
                    out[i] = ' ';
            --i;
        }
    }
    return out;
}

const std::regex& fragmentSignature()
{
    static const std::regex re(R"(\bvoid\s+fragment\s*\(\s*(void\s*)?\))");
    return re;
}

const std::regex& vertexSignature()
{
    static const std::regex re(R"(\bvoid\s+vertex\s*\(\s*(void\s*)?\))");
    return re;
}

// code with the function signature matches (in stripped, code without comments) blanked from its signature to its
// closing brace, newlines kept. Unchanged without one, or if its braces don't close (the compiler then says so).
std::string blankFunction(std::string code, const std::string& stripped, const std::regex& signature)
{
    std::smatch match;
    if (!std::regex_search(stripped, match, signature))
        return code;
    const auto start = static_cast<size_t>(match.position(0));
    size_t i = stripped.find('{', start + static_cast<size_t>(match.length(0)));
    for (int depth = 0; i < stripped.size(); ++i) {
        depth += stripped[i] == '{' ? 1 : stripped[i] == '}' ? -1 : 0;
        if (depth == 0)
            break;
    }
    if (i >= stripped.size())
        return code;
    for (size_t k = start; k <= i; ++k)
        if (code[k] != '\n')
            code[k] = ' ';
    return code;
}

// The lines of text, without their '\n' or "\r\n" (none after a last '\n').
std::vector<std::string_view> splitLines(std::string_view text)
{
    std::vector<std::string_view> lines;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

} // namespace

int componentCount(ShaderParamType type)
{
    switch (type) {
    case ShaderParamType::Float:
        return 1;
    case ShaderParamType::Vec2:
        return 2;
    case ShaderParamType::Vec3:
        return 3;
    case ShaderParamType::Vec4:
    case ShaderParamType::Color:
        return 4;
    case ShaderParamType::Texture2D:
        break;
    }
    return 0;
}

const std::vector<std::string>& shaderBuiltinNames()
{
    // The templates' (fs_mesh.sc, mesh.sh), plus those of later stages, kept now so a param can't take them later.
    static const std::vector<std::string> names = {
        "UV", "COLOR", "NORMAL", "TANGENT", "WORLD_POSITION", "VIEW_POSITION", "FRAG_COORD", "ALBEDO", "ALPHA",
        "METALLIC", "ROUGHNESS", "EMISSION", "NORMAL_MAP", "NORMAL_MAP_DEPTH", "AO", "TIME", "CAMERA_POSITION",
        "MATERIAL_BASE_COLOR", "MATERIAL_METALLIC", "MATERIAL_ROUGHNESS", "MATERIAL_EMISSION", "MATERIAL_ALPHA_CUTOFF",
        "MATERIAL_UV_TRANSFORM", "sampleBaseColor", "sampleMetallicRoughness", "sampleNormal", "sampleOcclusion",
        "sampleEmissive", "fragment", "vertex", "light",
        // vertex() (mesh.sh) and light() built-ins, SCREEN_TEXTURE / DEPTH_TEXTURE
        "VERTEX", "INSTANCE_ID", "MODEL_MATRIX", "WORLD_MATRIX", "VIEW_MATRIX", "PROJECTION_MATRIX", "LIGHT_DIRECTION",
        "LIGHT_COLOR", "ATTENUATION", "VIEW", "DIFFUSE_LIGHT", "SPECULAR_LIGHT", "SCREEN_TEXTURE", "SCREEN_UV",
        "DEPTH_TEXTURE", "PI"};
    return names;
}

bool parseShader(std::string_view text, ShaderSource& out, std::string& error)
{
    ShaderSource source;
    std::map<std::string, int> seen; // name -> line
    int numeric = 0, textures = 0, lineNumber = 0;
    for (std::string_view line : splitLines(text)) {
        ++lineNumber;
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty() || tokens[0] != "param") {
            source.code.append(line).push_back('\n');
            continue;
        }
        ShaderParam param;
        param.line = lineNumber;
        const auto fail = [&](const std::string& message) {
            error = std::format("line {}: {}", lineNumber, message);
            return false;
        };
        if (!parseParam(tokens, param, error))
            return fail(error);
        if (std::ranges::find(shaderBuiltinNames(), param.name) != shaderBuiltinNames().end())
            return fail(std::format("param '{}' has the name of a built-in", param.name));
        if (std::ranges::find(kKeywords, param.name) != std::end(kKeywords) ||
            std::ranges::any_of(kReservedPrefixes, [&](std::string_view p) { return param.name.starts_with(p); }))
            return fail(std::format("param '{}' has a reserved name (a keyword, or starting with u_, s_, v_, a_, i_, gl_, "
                                    "bgfx or kast)",
                                    param.name));
        if (const auto [it, added] = seen.try_emplace(param.name, lineNumber); !added)
            return fail(std::format("param '{}' is already declared on line {}", param.name, it->second));
        if (param.type == ShaderParamType::Texture2D) {
            if (textures == kMaxShaderTextures)
                return fail(std::format("too many texture2D params: at most {} (sampler stages {}..{}; a draw has {}, the "
                                        "mesh shader uses 0..{} and keeps {}..{} for SCREEN_TEXTURE and DEPTH_TEXTURE)",
                                        kMaxShaderTextures, kFirstShaderTextureStage,
                                        kFirstShaderTextureStage + kMaxShaderTextures - 1, kMaxTextureSamplers,
                                        kFirstShaderTextureStage - 1, kFirstShaderTextureStage + kMaxShaderTextures,
                                        kMaxTextureSamplers - 1));
            param.slot = textures++;
        } else {
            if (numeric == kMaxShaderParams)
                return fail(std::format("too many params: at most {} float, vec2, vec3, vec4 and color params",
                                        kMaxShaderParams));
            param.slot = numeric++;
        }
        source.params.push_back(std::move(param));
        source.code.push_back('\n'); // blanked: the code keeps its line numbers
    }
    const std::string stripped = stripComments(source.code);
    if (!std::regex_search(stripped, fragmentSignature())) {
        error = "no fragment() function: a .kshader needs one, as void fragment() { ... }";
        return false;
    }
    static const std::regex normal(R"(\bNORMAL(_MAP)?\b)");
    source.customNormal = std::regex_search(blankFunction(stripped, stripped, vertexSignature()), normal);
    source.hasVertex = std::regex_search(stripped, vertexSignature());
    out = std::move(source);
    return true;
}

std::array<std::array<float, 4>, kMaxShaderParams> shaderParamValues(const Material& material, const ShaderSource& source)
{
    std::array<std::array<float, 4>, kMaxShaderParams> values{};
    for (const ShaderParam& param : source.params) {
        if (param.type == ShaderParamType::Texture2D)
            continue;
        std::array<float, 4>& slot = values[static_cast<size_t>(param.slot)];
        slot = param.value;
        const auto it = material.params.find(param.name);
        if (it == material.params.end())
            continue;
        for (size_t k = 0; k < it->second.numbers.size() && k < 4; ++k)
            slot[k] = it->second.numbers[k];
        if (param.type == ShaderParamType::Color && it->second.numbers.size() == 3)
            slot[3] = 1.0f;
    }
    return values;
}

std::string shaderParamTexture(const Material& material, const ShaderParam& param)
{
    const auto it = material.params.find(param.name);
    return it == material.params.end() ? "" : it->second.texture;
}

bool checkShaderParams(const Material& material, const ShaderSource& source, std::string& error,
                       std::vector<std::string>& unknown)
{
    unknown.clear();
    for (const auto& [name, value] : material.params) {
        const auto param = std::ranges::find(source.params, name, &ShaderParam::name);
        if (param == source.params.end()) {
            unknown.push_back(name);
            continue;
        }
        const size_t count = value.numbers.size();
        const bool ok = param->type == ShaderParamType::Texture2D ? count == 0
                        : param->type == ShaderParamType::Color   ? count == 3 || count == 4
                                                                  : count == static_cast<size_t>(componentCount(param->type));
        if (!ok) {
            const std::string want = param->type == ShaderParamType::Texture2D ? "a texture path"
                                     : param->type == ShaderParamType::Float   ? "a number"
                                     : param->type == ShaderParamType::Color   ? "an array of 3 or 4 numbers"
                                     : std::format("an array of {} numbers", componentCount(param->type));
            error = std::format("param '{}' is a {}: its value must be {}", name, typeName(param->type), want);
            return false;
        }
    }
    return true;
}

std::optional<nlohmann::ordered_json> shaderParamField(const Material& material, const ShaderSource& source,
                                                       std::string_view name)
{
    const auto param = std::ranges::find(source.params, name, &ShaderParam::name);
    if (param == source.params.end())
        return std::nullopt;
    const std::string key(name);
    Material values; // just this param, so materialField writes it as a .mat would
    if (const auto it = material.params.find(key); it != material.params.end()) {
        values.params[key] = it->second;
    } else if (param->type != ShaderParamType::Texture2D) {
        const int count = param->type == ShaderParamType::Color ? 4 : componentCount(param->type);
        values.params[key].numbers.assign(param->value.begin(), param->value.begin() + count);
    } else {
        values.params[key] = {};
    }
    return (*materialField(values, "params"))[key];
}

bool setShaderParamField(Material& material, const ShaderSource& source, std::string_view name,
                         const nlohmann::ordered_json& value, std::string& error)
{
    const std::string key(name);
    if (std::ranges::find(source.params, name, &ShaderParam::name) == source.params.end()) {
        error = std::format("the shader declares no param \"{}\"", name);
        return false;
    }
    Material parsed; // the value alone: its shape, then its type against the declaration
    nlohmann::ordered_json params = nlohmann::ordered_json::object();
    params[key] = value;
    std::vector<std::string> unknown;
    if (!setMaterialField(parsed, "params", params, error) || !checkShaderParams(parsed, source, error, unknown))
        return false;
    material.params[key] = parsed.params[key];
    return true;
}

bool isVertexStage(ShaderStage stage)
{
    return stage != ShaderStage::Fragment && stage != ShaderStage::Shadow;
}

std::string_view shaderTemplate(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Fragment:
    case ShaderStage::Shadow:
        return "fs_mesh.sc";
    case ShaderStage::Vertex:
        return "vs_mesh.sc";
    case ShaderStage::SkinnedVertex:
        return "vs_mesh_skinned.sc";
    case ShaderStage::InstancedVertex:
        break;
    }
    return "vs_mesh_instanced.sc";
}

std::vector<ShaderStage> shaderStages(const ShaderSource& source)
{
    std::vector<ShaderStage> stages = {ShaderStage::Fragment, ShaderStage::Shadow};
    if (source.hasVertex)
        stages.insert(stages.end(), {ShaderStage::Vertex, ShaderStage::SkinnedVertex, ShaderStage::InstancedVertex});
    return stages;
}

std::string generateShader(std::string_view templateText, const ShaderSource& source, ShaderStage stage)
{
    const bool vertex = isVertexStage(stage);
    std::string out;
    bool injected = false;
    for (std::string_view line : splitLines(templateText)) {
        if (trim(line) != kUserLine) {
            out.append(line).push_back('\n');
            continue;
        }
        injected = true;
        out += "#pragma warning(disable: 3568) // \"unknown pragma\": the kast_line markers\n";
        if (vertex && source.hasVertex)
            out += "#define KAST_VERTEX\n";
        if (!vertex)
            out += "#define KAST_FRAGMENT\n";
        if (stage == ShaderStage::Shadow)
            out += "#define KAST_SHADOW\n";
        if (!vertex && source.customNormal)
            out += "#define KAST_CUSTOM_NORMAL\n";
        if (std::ranges::any_of(source.params, [](const ShaderParam& p) { return p.type != ShaderParamType::Texture2D; }))
            out += std::format("uniform vec4 u_kastParams[{}];\n", kMaxShaderParams);
        for (const ShaderParam& param : source.params) {
            if (param.type == ShaderParamType::Texture2D) {
                out += std::format("SAMPLER2D(s_kastTexture{}, {});\n", param.slot, kFirstShaderTextureStage + param.slot);
                out += std::format("#define {} s_kastTexture{}\n", param.name, param.slot);
            } else {
                constexpr std::string_view kSwizzles[] = {"", ".x", ".xy", ".xyz", ""};
                out += std::format("#define {} (u_kastParams[{}]{})\n", param.name, param.slot,
                                   kSwizzles[componentCount(param.type)]);
            }
        }
        // The stage's function gets the template's signature, the other is blanked; a marker before each line (not
        // inside a macro continued with '\').
        const std::string stripped = stripComments(source.code);
        std::string code = blankFunction(source.code, stripped, vertex ? fragmentSignature() : vertexSignature());
        std::smatch match;
        if (std::regex_search(stripped, match, vertex ? vertexSignature() : fragmentSignature()))
            code.replace(static_cast<size_t>(match.position(0)), static_cast<size_t>(match.length(0)),
                         vertex ? "void vertex(inout KastVertex kast_v)" : "void fragment(inout KastFragment kast_f)");
        int number = 0;
        bool continued = false;
        for (std::string_view user : splitLines(code)) {
            ++number;
            if (!continued)
                out += std::format("#pragma kast_line {}\n", number);
            out.append(user).push_back('\n');
            continued = user.ends_with('\\');
        }
        for (const ShaderParam& param : source.params)
            out += std::format("#undef {}\n", param.name);
    }
    return injected ? out : std::string();
}

std::string mapShaderErrors(std::string_view shadercOutput)
{
    // The listing: "    362: code" and ">>> 366: code" (then ">>>   7:   ^", the column) around the first error.
    static const std::regex listing(R"(^(>>>)?\s*(\d+): ?(.*)$)");
    static const std::regex marker(R"(^\s*#pragma\s+kast_line\s+(\d+)\s*$)");
    static const std::regex message(R"(\((\d+),\d+(?:-\d+)?\): error (\w+): (.*)$)");
    std::map<int, std::string> code; // generated line -> text
    std::vector<std::string> errors;
    bool afterArrow = false;
    for (std::string_view raw : splitLines(shadercOutput)) {
        const std::string line(raw);
        std::smatch m;
        if (std::regex_match(line, m, listing)) {
            const bool arrow = m[1].matched;
            if (!(arrow && afterArrow)) // not the column line under the error's
                code[std::stoi(m[2].str())] = m[3].str();
            afterArrow = arrow;
            continue;
        }
        afterArrow = false;
        if (std::regex_search(line, m, message)) {
            const int generated = std::stoi(m[1].str());
            std::optional<int> user;
            for (int g = generated - 1; !user && code.contains(g); --g) // up the listing to the line's marker
                if (std::smatch k; std::regex_match(code[g], k, marker))
                    user = std::stoi(k[1].str());
            errors.push_back(user ? std::format("line {}: error {}: {}", *user, m[2].str(), m[3].str())
                                  : std::format("generated line {}: error {}: {}", generated, m[2].str(), m[3].str()));
        } else if (line.starts_with("cpp:") && line.find("Error") != std::string::npos) {
            errors.push_back(line); // the preprocessor's: unterminated #if and the like
        }
    }
    if (errors.empty())
        return std::string(trim(shadercOutput));
    std::string out;
    for (const std::string& e : errors)
        out += (out.empty() ? "" : "\n") + e;
    return out;
}

std::string shaderCacheKey(std::string_view text, ShaderStage stage, std::string_view version)
{
    uint64_t hash = 14695981039346656037ull; // FNV-1a
    const auto add = [&](std::string_view bytes) {
        for (const char c : bytes)
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        hash = (hash ^ 0xffu) * 1099511628211ull; // separator: ("ab", "c") and ("a", "bc") differ
    };
    add(version);
    add(std::to_string(static_cast<int>(stage)));
    add(text);
    return std::format("{:016x}", hash);
}

bool compileShaderStages(const ShaderSource& source, std::string_view text, const ShaderCompiler& compiler,
                         std::string_view version, const std::filesystem::path& cacheDir, CompiledShader& out,
                         bool& cached, std::string& error)
{
    cached = true;
    for (const ShaderStage stage : shaderStages(source)) {
        std::vector<uint8_t>& bytecode = out.bytecode[static_cast<size_t>(stage)];
        const std::filesystem::path file =
            version.empty() ? std::filesystem::path() : cacheDir / (shaderCacheKey(text, stage, version) + ".bin");
        if (!file.empty()) {
            std::ifstream in(file, std::ios::binary);
            bytecode.assign(std::istreambuf_iterator<char>(in), {});
            if (!bytecode.empty())
                continue;
        }
        cached = false;
        if (!compiler(source, stage, bytecode, error))
            return false;
        if (file.empty())
            continue;
        // ponytail: the cache is never pruned; stale keys stay until the folder is deleted.
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        writeFileAtomic(file, std::string_view(reinterpret_cast<const char*>(bytecode.data()), bytecode.size()));
    }
    return true;
}

std::string shaderPakPath(std::string_view text, ShaderStage stage)
{
    return ".kshader_bytecode/" + shaderCacheKey(text, stage, "game.pak") + ".bin";
}

void ShaderLibrary::setRoot(const std::filesystem::path& root)
{
    if (root != root_) {
        root_ = root;
        entries_.clear(); // ponytail: waits for running compiles (std::async futures); only on a project switch
    }
}

void ShaderLibrary::setCompiler(ShaderCompiler compiler, std::string version)
{
    compiler_ = std::move(compiler);
    version_ = std::move(version);
}

ShaderLibrary::Entry& ShaderLibrary::entry(const std::string& path)
{
    auto [it, added] = entries_.try_emplace(path);
    if (added)
        load(path, it->second);
    return it->second;
}

std::shared_ptr<const CompiledShader> ShaderLibrary::get(const std::string& path)
{
    if (path.empty() || root_.empty())
        return nullptr;
    return entry(path).shader;
}

ShaderLibrary::Status ShaderLibrary::status(const std::string& path)
{
    if (path.empty() || root_.empty())
        return {};
    const Entry& e = entry(path);
    const State state = e.pending.valid() ? State::Compiling : e.error.empty() ? State::Compiled : State::Failed;
    return {.state = state, .error = e.error, .declaration = e.declaration, .hasShader = e.shader != nullptr};
}

void ShaderLibrary::reloadChanged()
{
    // ponytail: one stat per cached shader per call (per frame), as for materials.
    for (auto& [path, entry] : entries_) {
        if (entry.pending.valid()) {
            if (entry.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue; // the next call looks again; a change saved meanwhile reloads after it
            finish(path, entry);
        }
        if (modifiedTime(root_ / path) != entry.mtime)
            load(path, entry);
    }
}

void ShaderLibrary::wait()
{
    for (auto& [path, entry] : entries_)
        if (entry.pending.valid())
            finish(path, entry);
}

void ShaderLibrary::load(const std::string& path, Entry& entry)
{
    std::string error, text;
    const std::optional<std::filesystem::path> file = resolveProjectPath(root_, path, error);
    entry.mtime = file ? modifiedTime(*file) : std::nullopt;
    auto source = std::make_shared<ShaderSource>();
    entry.declaration = nullptr;
    if (file && !readFile(*file, text))
        error = "cannot read the file";
    else if (file && parseShader(text, *source, error))
        entry.declaration = source;
    if (entry.declaration && !compiler_) { // the player: game.pak's bytecode
        auto shader = std::make_shared<CompiledShader>();
        shader->source = *source;
        for (const ShaderStage stage : shaderStages(*source)) {
            std::string bytes;
            if (!readFile(root_ / shaderPakPath(text, stage), bytes)) {
                error = "no compiled bytecode for it (the editor's Build compiles it into game.pak; shaderc runs only "
                        "in the editor)";
                break;
            }
            shader->bytecode[static_cast<size_t>(stage)].assign(bytes.begin(), bytes.end());
        }
        if (error.empty()) {
            entry.pending = std::async(std::launch::deferred,
                                       [shader] { return Result{.shader = shader, .precompiled = true}; });
            finish(path, entry);
            return;
        }
    }
    if (!error.empty()) {
        entry.pending = std::async(std::launch::deferred, [error] { return Result{.error = error}; });
        finish(path, entry);
        return;
    }
    // The compile works on copies: the entry may be reloaded, or the library's root change, while it runs.
    const auto compile = [source = *source, text = std::move(text), compiler = compiler_, version = version_,
                          cache = root_ / ".kast" / "shader_cache"] {
        const auto start = std::chrono::steady_clock::now();
        auto shader = std::make_shared<CompiledShader>();
        shader->source = source;
        Result result;
        if (!compileShaderStages(source, text, compiler, version, cache, *shader, result.cached, result.error))
            return result;
        result.shader = std::move(shader);
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    };
    entry.pending = std::async(async_ ? std::launch::async : std::launch::deferred, compile);
    if (!async_)
        finish(path, entry);
}

void ShaderLibrary::finish(const std::string& path, Entry& entry)
{
    Result result = entry.pending.get();
    const bool reload = entry.loaded;
    entry.loaded = true;
    entry.error = result.error;
    if (result.shader) {
        entry.shader = std::move(result.shader);
        if (result.precompiled)
            log::info(std::format("Loaded shader '{}' (precompiled)", path));
        else
            log::info(std::format("{} shader '{}' {}", reload ? "Recompiled" : "Compiled", path,
                                  result.cached ? "(cache hit)" : std::format("in {:.2f} s", result.seconds)));
    } else {
        log::error(std::format("Shader '{}': {}; {}", path, result.error,
                               entry.shader ? "the last good version keeps rendering" : "rendering it magenta"));
    }
}

ShaderLibrary& shaders()
{
    static ShaderLibrary library;
    return library;
}

std::optional<nlohmann::ordered_json> materialProperty(const Material& material, std::string_view name)
{
    if (std::optional<nlohmann::ordered_json> field = materialField(material, name))
        return field;
    const std::shared_ptr<const ShaderSource> source = shaders().status(material.shader).declaration;
    return source ? shaderParamField(material, *source, name) : std::nullopt;
}

bool setMaterialProperty(Material& material, std::string_view name, const nlohmann::ordered_json& value,
                         std::string& error)
{
    if (!materialField(material, name)) {
        const std::shared_ptr<const ShaderSource> source = shaders().status(material.shader).declaration;
        if (source && std::ranges::find(source->params, name, &ShaderParam::name) != source->params.end())
            return setShaderParamField(material, *source, name, value, error);
    }
    return setMaterialField(material, name, value, error);
}

} // namespace kast
