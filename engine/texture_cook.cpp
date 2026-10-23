#include "texture_cook.h"

#include "files.h"
#include "renderer.h"
#include "shader.h"

#include <bimg/bimg.h>
#include <bx/error.h>
#include <bx/readerwriter.h>

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>

namespace kast {
namespace {

uint64_t fnv1a(uint64_t hash, std::string_view bytes)
{
    for (const char c : bytes)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return (hash ^ 0xffu) * 1099511628211ull; // separator: ("ab", "c") and ("a", "bc") differ
}

// Appends what bimg writes to a string.
class StringWriter : public bx::WriterI {
public:
    explicit StringWriter(std::string& out) : out_(out) {}
    int32_t write(const void* data, int32_t size, bx::Error*) override
    {
        out_.append(static_cast<const char*>(data), static_cast<size_t>(size));
        return size;
    }

private:
    std::string& out_;
};

// The size of a PNG / JPG (stb's header read) or a KTX (bimg's); nullopt if it is neither.
std::optional<std::pair<uint32_t, uint32_t>> imageSize(std::string_view image)
{
    bimg::ImageContainer container{};
    bx::Error err;
    if (image.starts_with("\xABKTX") && image.size() <= UINT32_MAX &&
        bimg::imageParse(container, image.data(), static_cast<uint32_t>(image.size()), &err))
        return std::pair{container.m_width, container.m_height};
    int width = 0, height = 0, channels = 0;
    if (image.size() > INT32_MAX || !stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(image.data()),
                                                           static_cast<int>(image.size()), &width, &height, &channels))
        return std::nullopt;
    return std::pair{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

// Whether a PNG / JPG has a pixel with alpha below 255. A KTX (the sky's cube input) counts as opaque: the sky ignores
// alpha.
bool hasAlpha(std::string_view image)
{
    int width = 0, height = 0, channels = 0;
    const auto* bytes = reinterpret_cast<const stbi_uc*>(image.data());
    const int size = static_cast<int>(std::min<size_t>(image.size(), INT32_MAX));
    if (image.starts_with("«KTX") || !stbi_info_from_memory(bytes, size, &width, &height, &channels) ||
        channels == 1 || channels == 3)
        return false;
    stbi_uc* pixels = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
    if (!pixels)
        return false;
    const size_t count = static_cast<size_t>(width) * height;
    bool alpha = false;
    for (size_t i = 0; i < count && !alpha; ++i)
        alpha = pixels[i * 4 + 3] != 255;
    stbi_image_free(pixels);
    return alpha;
}

} // namespace

std::string_view usageName(TextureUsage usage)
{
    switch (usage) {
    case TextureUsage::Color:
        return "color";
    case TextureUsage::Linear:
        return "linear";
    case TextureUsage::Normal:
        break;
    }
    return "normal";
}

bgfx::TextureFormat::Enum cookedFormat(TextureUsage usage, uint32_t width, uint32_t height, bool alpha)
{
    if (width % 4 != 0 || height % 4 != 0)
        return bgfx::TextureFormat::RGBA8;
    if (usage == TextureUsage::Normal)
        return bgfx::TextureFormat::BC5;
    return alpha ? bgfx::TextureFormat::BC3 : bgfx::TextureFormat::BC1;
}

std::vector<std::pair<std::string, TextureUsage>> materialTextures(const Material& material, const ShaderSource* shader)
{
    std::vector<std::pair<std::string, TextureUsage>> textures;
    const auto add = [&](const std::string& path, TextureUsage usage) {
        if (!path.empty())
            textures.emplace_back(path, usage);
    };
    add(material.baseColorTexture, TextureUsage::Color);
    add(material.metallicRoughnessTexture, TextureUsage::Linear);
    add(material.normalTexture, TextureUsage::Normal);
    add(material.occlusionTexture, TextureUsage::Linear);
    add(material.emissiveTexture, TextureUsage::Color);
    for (const auto& [name, value] : material.params) {
        bool srgb = true;
        if (shader)
            for (const ShaderParam& param : shader->params)
                if (param.name == name)
                    srgb = param.srgb;
        add(value.texture, colorUsage(srgb));
    }
    return textures;
}

std::string textureCacheKey(std::string_view image, TextureUsage usage, std::string_view version)
{
    uint64_t hash = fnv1a(14695981039346656037ull, version);
    hash = fnv1a(hash, std::to_string(kTextureCookVersion));
    hash = fnv1a(hash, usageName(usage));
    hash = fnv1a(hash, image);
    return std::format("{:016x}", hash);
}

bool cookTexture(std::string_view image, TextureUsage usage, const TextureCooker& cooker, std::string_view version,
                 const std::filesystem::path& cacheDir, std::string& ktx, bool& cached, std::string& error)
{
    cached = false;
    const std::filesystem::path file = cacheDir / (textureCacheKey(image, usage, version) + ".ktx");
    std::string ignored;
    if (readFile(file, ktx) && (parseGpuImage(ktx, false, ignored) || parseGpuImage(ktx, true, ignored))) {
        cached = true; // else (a write cut short, two editors racing): cooked again, rewritten
        return true;
    }
    const std::optional<std::pair<uint32_t, uint32_t>> size = imageSize(image);
    if (!size) {
        error = "cannot read it as a PNG or JPG image";
        return false;
    }
    ktx.clear();
    if (!cooker(image, usage, cookedFormat(usage, size->first, size->second, hasAlpha(image)), ktx, error))
        return false;
    // ponytail: the cache is never pruned; stale keys stay until the folder is deleted (as the shader cache).
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    writeFileAtomic(file, ktx);
    return true;
}

std::string cubeKtx(std::string_view sides, uint16_t size)
{
    std::string ktx;
    StringWriter writer(ktx);
    bx::Error err;
    bimg::imageWriteKtx(&writer, bimg::TextureFormat::RGBA8, true, size, size, 1, 1, 1, true, sides.data(), &err);
    return ktx;
}

std::string texturePakPath(const std::string& path, TextureUsage usage)
{
    return std::format("{}.{}.ktx", path, usageName(usage));
}

std::string skyPakPath(const std::array<std::string, 6>& faces)
{
    uint64_t hash = 14695981039346656037ull;
    for (const std::string& face : faces)
        hash = fnv1a(hash, face);
    return std::format(".kast_sky/{:016x}.ktx", hash);
}

void TextureCooks::setCooker(TextureCooker cooker, std::string version)
{
    wait();
    cooker_ = std::move(cooker);
    version_ = std::move(version);
}

std::shared_future<CookedTexture> TextureCooks::cook(const std::filesystem::path& root, std::string image,
                                                     TextureUsage usage)
{
    std::erase_if(running_, [](const auto& entry) {
        return entry.second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    const std::filesystem::path cacheDir = root / ".kast" / "texture_cache";
    const std::string key = textureCacheKey(image, usage, version_);
    if (const auto it = running_.find(key); it != running_.end())
        return it->second;
    const auto run = [cooker = cooker_, version = version_, cacheDir, image = std::move(image), usage] {
        const auto start = std::chrono::steady_clock::now();
        CookedTexture result;
        if (!cookTexture(image, usage, cooker, version, cacheDir, result.ktx, result.cached, result.error))
            result.ktx.clear();
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    };
    std::string hit, ignored;
    const bool cached = readFile(cacheDir / (key + ".ktx"), hit) &&
                        (parseGpuImage(hit, false, ignored) || parseGpuImage(hit, true, ignored));
    if (!async_ || cached) {
        std::promise<CookedTexture> done;
        done.set_value(run()); // a hit reads the file again: ponytail, it is small next to the decode it saves
        return done.get_future().share();
    }
    return running_[key] = std::async(std::launch::async, run).share();
}

void TextureCooks::wait()
{
    for (auto& [key, cook] : running_)
        cook.wait();
    running_.clear();
}

TextureCooks& textureCooks()
{
    static TextureCooks cooks;
    return cooks;
}

} // namespace kast
