#pragma once

#include "material.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// User shaders: .kshader files (bgfx .sc GLSL, see docs/engine/shaders.html) whose fragment() and vertex() Kast injects
// into its mesh shader templates (engine/shaders/fs_mesh.sc and vs_mesh*.sc, at their "//@user" line). GPU-free: parsing, code generation, compile error
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
    bool customNormal = false; // mentions NORMAL or NORMAL_MAP outside vertex(): the template always applies the normal map
    bool hasVertex = false;    // has a vertex(): compiled into the vertex templates too
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
// A param's value as JSON, as a .mat stores it (the material's value, else the declared default; a texture2D without
// one: ""). nullopt if source declares no param name.
std::optional<nlohmann::ordered_json> shaderParamField(const Material& material, const ShaderSource& source,
                                                       std::string_view name);
// Sets the material's value for param name, checked against its declaration. False + error leaves material unchanged.
bool setShaderParamField(Material& material, const ShaderSource& source, std::string_view name,
                         const nlohmann::ordered_json& value, std::string& error);

// What a .kshader compiles into. Shadow: fs_mesh with KAST_SHADOW, the shadow passes' fragment shader, so fragment()'s
// ALPHA and discard cut shadows out too. The shadow passes share the vertex stages with the main pass.
enum class ShaderStage { Fragment, Shadow, Vertex, SkinnedVertex, InstancedVertex };
constexpr size_t kShaderStageCount = 5;
bool isVertexStage(ShaderStage stage);
// The template stage compiles from, as engine/shaders names it ("fs_mesh.sc", "vs_mesh_skinned.sc", ...).
std::string_view shaderTemplate(ShaderStage stage);
// The stages source compiles into: Fragment and Shadow, and the vertex stages if it has a vertex() (else its draws use
// the built-in vertex shaders, which are the same templates without user code).
std::vector<ShaderStage> shaderStages(const ShaderSource& source);

// templateText (stage's mesh template, '\n' or "\r\n" line ends) with source's param declarations and code at its
// "//@user" line, and a "#pragma kast_line <n>" marker before each line of the .kshader so compile errors map back to it
// (mapShaderErrors). The stage's function (fragment() or vertex()) gets the template's signature; the other one is
// blanked, keeping line numbers. Empty if the template has no "//@user" line.
std::string generateShader(std::string_view templateText, const ShaderSource& source, ShaderStage stage);
// shaderc's output for a failed compile of generateShader's text, as "line <n>: error X3004: ..." lines with .kshader
// line numbers (n from the marker before the error's line in shaderc's listing). Errors outside the listing or in the
// template say "generated line <n>"; preprocessor errors are kept as they are.
std::string mapShaderErrors(std::string_view shadercOutput);

// Compiles a parsed .kshader for one stage to bytecode; false + error (line numbers in the .kshader).
using ShaderCompiler =
    std::function<bool(const ShaderSource& source, ShaderStage stage, std::vector<uint8_t>& bytecode, std::string& error)>;

// A .kshader that compiled.
struct CompiledShader {
    ShaderSource source;
    std::array<std::vector<uint8_t>, kShaderStageCount> bytecode; // by ShaderStage; empty for stages not in shaderStages()
    const std::vector<uint8_t>& stage(ShaderStage s) const { return bytecode[static_cast<size_t>(s)]; }
};

// The bytecode cache's key for one stage of a .kshader: a hash of its text, the stage and version (what else the
// bytecode depends on: the templates, shaderc). 16 hex digits.
std::string shaderCacheKey(std::string_view text, ShaderStage stage, std::string_view version);

// .kshader files by project-relative path, each parsed and compiled once and again when its file's modification time
// changes. One global library, so the editor's renderers share each compile.
// Async (the editor): compiles run on worker threads; get() keeps handing out the last good shader while a recompile
// runs and after it fails. Otherwise each compile runs inside the call that needs it (screenshots, tests).
// With a cache version, each stage's bytecode is kept in <root>/.kast/shader_cache/<shaderCacheKey>.bin (a dot-folder:
// never shipped), so unchanged shaders are not recompiled on the next start.
class ShaderLibrary {
public:
    enum class State { None, Compiling, Compiled, Failed };
    struct Status {
        State state = State::None;  // None: no path or no root
        std::string error;          // the last compile's (Failed), with .kshader line numbers
        std::shared_ptr<const ShaderSource> declaration; // the file as last parsed (null if it didn't): its params
        bool hasShader = false;     // a good compile renders (Compiling / Failed may still have the last good one)
    };

    // Forgets every cached shader if root differs from the last call's.
    void setRoot(const std::filesystem::path& root);
    // version: the bytecode cache's (shaderCacheKey); empty = no cache.
    void setCompiler(ShaderCompiler compiler, std::string version = {});
    void setAsync(bool async) { async_ = async; }
    // nullptr if path is empty, there is no root, it is still compiling for the first time, or the file is missing or
    // doesn't parse or compile (each failure logged once per load as an error naming the file and its line).
    std::shared_ptr<const CompiledShader> get(const std::string& path);
    // Starts loading path, as get() does, if it isn't yet.
    Status status(const std::string& path);
    // Takes in finished compiles, and reloads shaders whose file changed (or appeared / disappeared) since loaded.
    void reloadChanged();
    // Blocks until no compile runs, and takes them in.
    void wait();

private:
    struct Result {
        std::shared_ptr<const CompiledShader> shader; // null: failed
        std::string error;
        double seconds = 0.0;
        bool cached = false; // every stage came from the cache
    };
    struct Entry {
        std::shared_ptr<const CompiledShader> shader; // the last good compile
        std::optional<std::filesystem::file_time_type> mtime;
        std::shared_ptr<const ShaderSource> declaration;
        std::string error;
        std::future<Result> pending;
        bool loaded = false; // a compile finished before (good or not): the next good one is a reload
    };
    void load(const std::string& path, Entry& entry);
    void finish(const std::string& path, Entry& entry);
    Entry& entry(const std::string& path); // loading it if it is new

    std::filesystem::path root_;
    ShaderCompiler compiler_;
    std::string version_;
    bool async_ = false;
    std::unordered_map<std::string, Entry> entries_;
};

ShaderLibrary& shaders();

// What kast.get/setMaterialProperty and the Inspector edit: a material field (materialField / setMaterialField), else
// a param its .kshader declares (as shaders() last parsed it). Built-in field names win; parseShader rejects params
// named like them. nullopt / false + error ("unknown field ...") for a name that is neither.
std::optional<nlohmann::ordered_json> materialProperty(const Material& material, std::string_view name);
bool setMaterialProperty(Material& material, std::string_view name, const nlohmann::ordered_json& value,
                         std::string& error);

} // namespace kast
