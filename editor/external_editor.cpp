#include "external_editor.h"

#include "assets.h" // utf8, fromUtf8

#include <log.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <format>
#include <fstream>

namespace {

constexpr const char* kCommandKey = "externalEditor";
// A command that fails (e.g. "not recognized", exit 9009) does so well within this; one still running after it is
// taken to be the editor, left alone.
constexpr uint64_t kFailureWindowMs = 3000;

std::string nativeUtf8(const std::filesystem::path& path) // backslashes, as Windows editors expect
{
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::wstring widen(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// Through cmd.exe so PATH lookups find .cmd/.bat shims such as VS Code's code.cmd. No console window, no wait.
HANDLE startCommand(const std::string& command, std::string& error)
{
    // /s: cmd strips just the outer quotes, so the command's own quotes are kept as written.
    std::wstring line = L"cmd.exe /d /s /c \"" + widen(command) + L"\"";
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &info)) {
        error = std::format("cannot start cmd.exe (error {})", GetLastError());
        return nullptr;
    }
    CloseHandle(info.hThread);
    return info.hProcess;
}

// Opens file as Explorer would. Windows may ask which app to use; that counts as opened.
void openWithAssociation(const std::filesystem::path& file)
{
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32)
        kast::log::info(std::format("Opened '{}' with its Windows file association", utf8(file)));
    else
        kast::log::error(std::format("Cannot open '{}' externally: ShellExecute failed (error {})", utf8(file), result));
}

} // namespace

std::filesystem::path defaultEditorSettingsFile()
{
    char* dir = SDL_GetPrefPath("Kast", "Editor");
    if (!dir) {
        kast::log::error(std::format("Cannot find the editor settings folder: {}", SDL_GetError()));
        return {};
    }
    const std::filesystem::path file = fromUtf8(dir) / "editor_settings.json";
    SDL_free(dir);
    return file;
}

std::string loadEditorCommand(const std::filesystem::path& file)
{
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec))
        return kDefaultEditorCommand;
    try {
        std::ifstream in(file, std::ios::binary);
        return nlohmann::json::parse(in).at(kCommandKey).get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        kast::log::warn(std::format("Editor settings '{}' are invalid ({}); using the default external editor command",
                                    utf8(file), e.what()));
        return kDefaultEditorCommand;
    }
}

bool saveEditorCommand(const std::filesystem::path& file, const std::string& command, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << nlohmann::ordered_json{{kCommandKey, command}}.dump(2) << '\n';
    out.close();
    if (file.empty() || !out) {
        error = std::format("Cannot write the editor settings '{}'", utf8(file));
        return false;
    }
    return true;
}

std::string expandEditorCommand(std::string_view command, const std::filesystem::path& file)
{
    constexpr std::string_view kPlaceholder = "{file}";
    const std::string path = nativeUtf8(file);
    std::string expanded(command);
    if (expanded.find(kPlaceholder) == std::string::npos)
        return expanded + (expanded.empty() ? "" : " ") + "\"" + path + "\"";
    for (size_t at = expanded.find(kPlaceholder); at != std::string::npos; at = expanded.find(kPlaceholder, at + path.size()))
        expanded.replace(at, kPlaceholder.size(), path);
    return expanded;
}

ExternalEditor::ExternalEditor(std::filesystem::path settingsFile)
    : settingsFile_(std::move(settingsFile)), command_(loadEditorCommand(settingsFile_))
{
}

ExternalEditor::~ExternalEditor()
{
    for (const Launch& launch : launches_)
        CloseHandle(launch.process);
}

bool ExternalEditor::setCommand(std::string command, std::string& error)
{
    if (!saveEditorCommand(settingsFile_, command, error))
        return false;
    command_ = std::move(command);
    return true;
}

void ExternalEditor::open(const std::filesystem::path& file)
{
    const std::filesystem::path absolute = std::filesystem::absolute(file).lexically_normal();
    const std::string command = expandEditorCommand(command_, absolute);
    std::string error;
    HANDLE process = startCommand(command, error);
    if (!process) {
        kast::log::warn(std::format("External editor command '{}' failed: {}; falling back to the file association",
                                    command, error));
        openWithAssociation(absolute);
        return;
    }
    kast::log::info(std::format("Opening '{}' in the external editor: {}", utf8(absolute), command));
    launches_.push_back({process, absolute, command, GetTickCount64()});
}

void ExternalEditor::poll()
{
    std::erase_if(launches_, [](const Launch& launch) {
        DWORD exitCode = 0;
        const bool exited = WaitForSingleObject(launch.process, 0) == WAIT_OBJECT_0;
        if (!exited && GetTickCount64() - launch.startedMs < kFailureWindowMs)
            return false; // keep watching
        if (exited && GetExitCodeProcess(launch.process, &exitCode) && exitCode != 0) {
            kast::log::warn(std::format("External editor command '{}' failed (exit code {}); falling back to the file "
                                        "association",
                                        launch.command, exitCode));
            openWithAssociation(launch.file);
        }
        CloseHandle(launch.process);
        return true;
    });
}
