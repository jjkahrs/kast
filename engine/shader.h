#pragma once

#include "material.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// User shaders: .kshader files (bgfx .sc GLSL, see docs/engine/shaders.html) whose fragment() Kast injects into its mesh
// shader template (engine/shaders/fs_mesh.sc, at its "//@user" line). GPU-free: parsing, code generation, compile error
// mapping and the library of compiled bytecode. Compiling (shaderc) is a ShaderCompiler the host provides: the editor's
// runs shaderc.exe (editor/shader_compiler.h); the player has none. The renderer owns the programs.
namespace kast {

constexpr int kMaxShaderParams = 16;  // float / vec / color params: one u_kastParams vec4 each
// Sampler stages per draw: bgfx's D3D11 limit (caps limits.maxTextureSamplers, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT).
// The mesh shader takes 0..9 (material textures, shadow maps); texture2D params take 10..13; 14 and 15 are kept for
// SCREEN_TEXTURE and DEPTH_TEXTURE.
constexpr int kMaxTextureSamplers = 16;
constexpr uint8_t kFirstShaderTextureStage = 10;
constexpr int kMaxShaderTextures = 4;

enum class ShaderParamType { Float, Vec2, Vec3, Vec4, Color, Texture2D };
// What a texture2D param samples when its material gives no texture: 1x1 white, opaque black or a flat normal.
enum class TextureDefault { White, Black, Normal };

// One "param <type> <name> [= <default>] [range(<min>, <max>)] [srgb];" line of a .kshader.
struct ShaderParam {
    std::string name;
    ShaderParamType type = ShaderParamType::Float;
    std::array<float, 4> value{}; // default; color without alpha: alpha 1
    std::optional<std::array<float, 2>> range; // float: the Inspector slider's bounds (values outside still apply)
    TextureDefault texture = TextureDefault::White;
    bool srgb = false; // texture2D: color data, decoded to linear when sampled
    int line = 0;      // in the .kshader, from 1
    int slot = 0;      // numeric: its u_kastParams index; texture2D: its stage is kFirstShaderTextureStage + slot
};

// A parsed .kshader.
struct ShaderSource {
    std::vector<ShaderParam> params;
    std::string code;          // the file with its param lines blanked (line numbers unchanged), '\n' line ends
    bool customNormal = false; // mentions NORMAL or NORMAL_MAP: the template always applies the normal map
};

// Number of floats a numeric param holds (1..4); 0 for texture2D.
int componentCount(ShaderParamType type);
// The names a param may not take: the template's built-ins (ALBEDO, UV, sampleBaseColor, ...), the stage functions.
const std::vector<std::string>& shaderBuiltinNames();
// False + error ("line 3: ...") if a param line is malformed, a name is reserved or repeated, there are too many
// params or textures, or there is no void fragment().
bool parseShader(std::string_view text, ShaderSource& out, std::string& error);

// The values the u_kastParams uniform gets: each numeric param's material value, else its default.
std::array<std::array<float, 4>, kMaxShaderParams> shaderParamValues(const Material& material, const ShaderSource& source);
// The texture a texture2D param samples: the material's path ("" = the param's default).
std::string shaderParamTexture(const Material& material, const ShaderParam& param);
// False + error if a material value has the wrong type for its param (numbers for a texture, 3 numbers for a vec2...).
// Values for params the shader doesn't declare are not errors (it may have changed since the .mat was saved): their
// names go to unknown.
bool checkShaderParams(const Material& material, const ShaderSource& source, std::string& error,
                       std::vector<std::string>& unknown);

// templateText (a mesh template, '\n' or "\r\n" line ends) with source's param declarations and code at its "//@user"
// line, its fragment() given the template's signature, and a "#pragma kast_line <n>" marker before each line of the
// .kshader so compile errors map back to it (mapShaderErrors). Empty if the template has no "//@user" line.
std::string generateShader(std::string_view templateText, const ShaderSource& source);
// shaderc's output for a failed compile of generateShader's text, as "line <n>: error X3004: ..." lines with .kshader
// line numbers (n from the marker before the error's line in shaderc's listing). Errors outside the listing or in the
// template say "generated line <n>"; preprocessor errors are kept as they are.
std::string mapShaderErrors(std::string_view shadercOutput);

enum class ShaderStage { Fragment }; // the templates a .kshader compiles into
// Compiles a parsed .kshader for one stage to bytecode; false + error (line numbers in the .kshader).
using ShaderCompiler =
    std::function<bool(const ShaderSource& source, ShaderStage stage, std::vector<uint8_t>& bytecode, std::string& error)>;

// A .kshader that compiled.
struct CompiledShader {
    ShaderSource source;
    std::vector<uint8_t> fragment; // fs_mesh with its fragment()
};

// .kshader files by project-relative path, each parsed and compiled once and again when its file's modification time
// changes. One global library, so the editor's renderers share each compile.
// ponytail: compiles on the calling (render) thread, blocking it for each compile.
class ShaderLibrary {
public:
    // Forgets every cached shader if root differs from the last call's.
    void setRoot(const std::filesystem::path& root);
    void setCompiler(ShaderCompiler compiler) { compiler_ = std::move(compiler); }
    // nullptr if path is empty, there is no root, the file is missing or doesn't parse or compile: logged once per load
    // as an error naming the file and its line.
    std::shared_ptr<const CompiledShader> get(const std::string& path);
    // Reloads shaders whose file changed (or appeared / disappeared) since loaded.
    void reloadChanged();

private:
    struct Entry {
        std::shared_ptr<const CompiledShader> shader;
        std::optional<std::filesystem::file_time_type> mtime;
    };
    void load(const std::string& path, Entry& entry);

    std::filesystem::path root_;
    ShaderCompiler compiler_;
    std::unordered_map<std::string, Entry> entries_;
};

ShaderLibrary& shaders();

} // namespace kast
