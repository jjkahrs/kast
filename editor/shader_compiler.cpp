#include "shader_compiler.h"

#include <files.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <format>
#include <fstream>
#include <iterator>
#include <process.h> // _getpid

namespace {

// The folder of this program (UTF-8 from SDL), where CMake puts shaderc.exe and kshader/.
std::filesystem::path toolDir()
{
    const char* base = SDL_GetBasePath();
    const std::string text = base ? base : "";
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool readBytes(const std::filesystem::path& file, std::string& out)
{
    std::ifstream in(file, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(in), {});
    return in.good() || in.eof();
}

std::string u8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

bool compileShader(const kast::ShaderSource& source, kast::ShaderStage stage, std::vector<uint8_t>& bytecode,
                   std::string& error)
{
    const std::filesystem::path tools = toolDir();
    const std::filesystem::path shaderc = tools / "shaderc.exe";
    const std::filesystem::path mesh = tools / "kshader" / "mesh";
    const std::string_view templateName = kast::shaderTemplate(stage);
    std::string templateText;
    if (!std::filesystem::exists(shaderc) || !readBytes(mesh / templateName, templateText) || templateText.empty()) {
        error = std::format("shaderc.exe or kshader/mesh/{} is missing next to the editor ({})", templateName, u8(tools));
        return false;
    }
    const std::string generated = kast::generateShader(templateText, source, stage);
    if (generated.empty()) {
        error = std::format("the mesh template kshader/mesh/{} has no //@user line", templateName);
        return false;
    }

    // ponytail: a temp file per compile (shaderc reads files); the project's shader cache could hold them instead.
    static std::atomic<int> counter = 0;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kast_shaderc";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path in = dir / std::format("{}_{}.sc", _getpid(), counter++);
    const std::filesystem::path out = std::filesystem::path(in).replace_extension(".bin");
    if (!kast::writeFileAtomic(in, generated)) {
        error = std::format("cannot write '{}'", u8(in));
        return false;
    }
    // As CMakeLists.txt compiles the built-in shaders. SDL quotes each argument.
    const std::string args[] = {u8(shaderc), "-f", u8(in), "-o", u8(out), "--type",
                                kast::isVertexStage(stage) ? "vertex" : "fragment", "--platform", "windows", "-p", "s_5_0",
                                "-i", u8(tools / "kshader" / "include"), "-i", u8(mesh), "--varyingdef",
                                u8(mesh / "varying.def.sc")};
    std::vector<const char*> argv;
    for (const std::string& arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true); // no console window
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) {
        error = std::format("cannot start shaderc: {}", SDL_GetError());
        std::filesystem::remove(in, ec);
        return false;
    }
    size_t size = 0;
    int code = -1;
    void* output = SDL_ReadProcess(process, &size, &code); // waits for it to exit
    const std::string text = output ? std::string(static_cast<const char*>(output), size) : "";
    SDL_free(output);
    SDL_DestroyProcess(process);
    std::string bytes;
    const bool ok = code == 0 && readBytes(out, bytes) && !bytes.empty();
    std::filesystem::remove(in, ec);
    std::filesystem::remove(out, ec);
    if (!ok) {
        error = kast::mapShaderErrors(text);
        if (error.empty())
            error = std::format("shaderc failed (exit code {}) with no output", code);
        return false;
    }
    bytecode.assign(bytes.begin(), bytes.end());
    return true;
}
