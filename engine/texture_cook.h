#pragma once

#include "material.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Compressed (BCn) textures: what each texture is sampled as picks its cooked format, the editor cooks PNG / JPG images
// with texturec.exe (editor/texture_cooker.h, injected with TextureCooks::setCooker) through a cache, and Build puts the
// cooked KTX files in game.pak, where the player reads them (it never cooks). Authored .ktx / .dds are never cooked.
namespace kast {

struct ShaderSource;

// What a texture is sampled as. Base color, emissive, sky faces, particles and sRGB .kshader params are Color;
// metallic-roughness, occlusion and linear params are Linear; normal maps are Normal.
enum class TextureUsage : uint8_t { Color, Linear, Normal };
constexpr TextureUsage kTextureUsages[] = {TextureUsage::Color, TextureUsage::Linear, TextureUsage::Normal};
// A slot the caller only knows the sRGB flag of (.kshader params, particles).
constexpr TextureUsage colorUsage(bool srgb) { return srgb ? TextureUsage::Color : TextureUsage::Linear; }
constexpr bool isSrgb(TextureUsage usage) { return usage == TextureUsage::Color; }
std::string_view usageName(TextureUsage usage); // "color", "linear", "normal"
// The format a width x height image of usage cooks to: BC5 for normals (X, Y; the mesh shader rebuilds Z); otherwise
// BC1 when opaque, BC3 when alpha has a pixel below 255 (Unity's and Godot's defaults). RGBA8 when a side isn't a
// multiple of 4 (D3D11 takes BCn only in whole blocks).
// ponytail: no BC7: texturec's BC7 encoder (nvtt) takes ~30 s for 256x256 and minutes for 1024x1024. Add it as a
// high-quality option here (and bump kTextureCookVersion) if a fast BC7 encoder reaches texturec.
bgfx::TextureFormat::Enum cookedFormat(TextureUsage usage, uint32_t width, uint32_t height, bool alpha);
// Bumped when cooked output changes (formats, texturec flags): every cache entry misses.
constexpr int kTextureCookVersion = 2;

// The textures a material samples, with each one's usage (empty slots left out; an ORM pair also as its ormTexturePath). shader: its .kshader as parsed, for
// its texture2D params' sRGB flags (null: params count as Color).
std::vector<std::pair<std::string, TextureUsage>> materialTextures(const Material& material, const ShaderSource* shader);

// A material whose occlusion and metallic-roughness textures are different images samples them packed into one
// (fs_mesh.sc s_metallicRoughness, glTF's ORM layout: R occlusion, G roughness, B metallic). ormTexturePath names that
// texture, "orm:<metallicRoughness>|<occlusion>" (texture paths hold no '|'); "" when the material has at most one of
// them, or both are the same image (sampled as it is). ormSources splits the name back (nullopt for any other path).
std::string ormTexturePath(const Material& material);
std::optional<std::pair<std::string, std::string>> ormSources(std::string_view path);
// The packed image as a PNG: metallicRoughness's pixels with occlusion's R (resampled to its size if they differ) as
// R. Each is PNG, JPG, KTX or DDS bytes (a KTX / DDS: its first mip). nullopt + error if one can't be decoded.
std::optional<std::string> ormImage(std::string_view metallicRoughness, std::string_view occlusion, std::string& error);

// Cooks image (PNG / JPG bytes, or an RGBA8 cube KTX from cubeKtx) into a KTX of format with mips: the editor's runs
// texturec. False + error (texturec's output) if it fails.
using TextureCooker = std::function<bool(std::string_view image, TextureUsage usage, bgfx::TextureFormat::Enum format,
                                         std::string& ktx, std::string& error)>;
// The texture cache's key: a hash of version (the cooker's), kTextureCookVersion, usage and the image bytes. 16 hex digits.
std::string textureCacheKey(std::string_view image, TextureUsage usage, std::string_view version);
// Cooks image with cooker through <cacheDir>/<textureCacheKey>.ktx: a hit reads that file, a miss cooks and writes it.
// False + error if the image can't be read or the cooker fails.
bool cookTexture(std::string_view image, TextureUsage usage, const TextureCooker& cooker, std::string_view version,
                 const std::filesystem::path& cacheDir, std::string& ktx, bool& cached, std::string& error);
// An RGBA8 cube of size x size sides (sides: their pixels, one after another in bgfx's side order +X, -X, +Y, -Y, +Z,
// -Z) as a KTX without mips: the sky's cooker input.
std::string cubeKtx(std::string_view sides, uint16_t size);
// Where game.pak keeps texture path (a project file or "<model>#<i>") cooked for usage ("<path>.<usage>.ktx"; an ORM
// pair's: ".kast_orm/<hash>.ktx"), and the cube cooked from sky faces. The editor's Build writes these; the player, which never cooks, reads them.
std::string texturePakPath(const std::string& path, TextureUsage usage);
std::string skyPakPath(const std::array<std::string, 6>& faces);

// A cook on the way or done.
struct CookedTexture {
    std::string ktx; // empty: failed
    std::string error;
    bool cached = false;
    double seconds = 0.0;
};

// The editor's texture cooking, shared by every renderer: with a cooker set, PNG / JPG textures load as their cooked
// KTX from <root>/.kast/texture_cache (a dot-folder: never shipped). Async (the editor): a miss cooks on a worker
// thread while the renderer draws the RGBA8 image, then swaps; otherwise it cooks inside the call (screenshots, tests).
class TextureCooks {
public:
    // version: the cache's (textureCacheKey).
    void setCooker(TextureCooker cooker, std::string version);
    void setAsync(bool async) { async_ = async; }
    bool enabled() const { return static_cast<bool>(cooker_); }
    // image's cooked KTX for usage in root's cache: ready at once on a hit or when not async; else a cook on a worker,
    // one per key however many callers ask.
    std::shared_future<CookedTexture> cook(const std::filesystem::path& root, std::string image, TextureUsage usage);
    // Blocks until no cook runs.
    void wait();

private:
    TextureCooker cooker_;
    std::string version_;
    bool async_ = false;
    std::map<std::string, std::shared_future<CookedTexture>> running_; // by cache file
};

TextureCooks& textureCooks();

} // namespace kast
