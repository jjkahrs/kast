#include "texture_cooker.h"

#include "shader_compiler.h"

#include <files.h>

#include <atomic>
#include <chrono>
#include <format>
#include <mutex> // std::call_once
#include <process.h> // _getpid

namespace {

std::string u8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

bool cookTextureWithTexturec(std::string_view image, kast::TextureUsage usage, bgfx::TextureFormat::Enum format,
                             std::string& ktx, std::string& error)
{
    const std::filesystem::path texturec = toolDir() / "texturec.exe";
    if (!std::filesystem::exists(texturec)) {
        error = std::format("texturec.exe is missing next to the editor ({})", u8(toolDir()));
        return false;
    }
    // ponytail: temp files per cook (texturec reads and writes files), as compileShader does. Each name is new (process
    // id, count, start time: a reused process id never meets an old file); a virus scanner holding a file just written
    // is waited out by writeFileAtomic's rename retries.
    static std::atomic<int> counter = 0;
    static const long long started = std::chrono::system_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kast_texturec";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::once_flag cleaned; // before this process's first file: what earlier runs left (a lock, a crash)
    std::call_once(cleaned, [&] { kast::removeStaleTempFiles(dir, std::format("{}_", _getpid()), std::chrono::hours(1)); });
    const std::string_view extension = image.starts_with("\x89PNG") ? ".png" : image.starts_with("\xABKTX") ? ".ktx" : ".jpg";
    const std::filesystem::path in = dir / std::format("{}_{}_{:x}{}", _getpid(), counter++, started, extension);
    const std::filesystem::path out = std::filesystem::path(in).replace_extension(".out.ktx");
    if (!kast::writeFileAtomic(in, image)) {
        error = std::format("cannot write '{}'", u8(in));
        return false;
    }
    const char* type = format == bgfx::TextureFormat::BC5   ? "BC5"
                       : format == bgfx::TextureFormat::BC3 ? "BC3"
                       : format == bgfx::TextureFormat::BC1 ? "BC1"
                                                            : "RGBA8";
    std::vector<std::string> args{u8(texturec), "-f", u8(in), "-o", u8(out), "-m", "-t", type};
    if (usage == kast::TextureUsage::Linear)
        args.emplace_back("--linear");
    else if (usage == kast::TextureUsage::Normal)
        args.emplace_back("--normalmap");
    std::string output;
    int code = -1;
    const bool ran = runTool(args, output, code, error);
    const bool ok = ran && code == 0 && kast::readFile(out, ktx) && !ktx.empty();
    std::filesystem::remove(in, ec);
    std::filesystem::remove(out, ec);
    if (ok)
        return true;
    if (ran) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        error = output.empty() ? std::format("texturec failed (exit code {}) and wrote no texture", code)
                               : std::format("texturec failed (exit code {}): {}", code, output);
    }
    return false;
}

std::string texturecVersion()
{
    std::error_code ec;
    const std::filesystem::path texturec = toolDir() / "texturec.exe";
    return std::format("{:016x}", std::hash<std::string>{}(std::format(
                                      "{} {}", std::filesystem::file_size(texturec, ec),
                                      std::filesystem::last_write_time(texturec, ec).time_since_epoch().count())));
}
