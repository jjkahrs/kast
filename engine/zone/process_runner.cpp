#include "process_runner.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace zone {
namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A Windows command-line argument: quoted, with the backslashes before a quote doubled (CommandLineToArgvW rules).
std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

} // namespace

ProcessRunner::~ProcessRunner() = default; // jthread joins

bool ProcessRunner::start(const fs::path& exe, const std::vector<fs::path>& args, std::string& err) {
    if (running_) {
        err = "already running";
        return false;
    }
    if (thread_.joinable()) thread_.join();
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        err = "cannot create a pipe";
        return false;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0); // only the child's end is inherited
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring command = quote(exe.wstring());
    for (const fs::path& a : args) command += L" " + quote(a.wstring());
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process);
    CloseHandle(write); // the child holds its copy; the read end sees EOF when it exits
    if (!started) {
        CloseHandle(read);
        err = "cannot start " + utf8(exe) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    {
        std::lock_guard lock(mutex_);
        lines_.clear();
        exitCode_.reset();
    }
    running_ = true;
    thread_ = std::jthread([this, read, handle = process.hProcess] {
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            pending.append(buffer, got);
            for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                std::string line = pending.substr(0, end);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard lock(mutex_);
                lines_.push_back(std::move(line));
            }
        }
        WaitForSingleObject(handle, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(handle, &code);
        CloseHandle(handle);
        CloseHandle(read);
        std::lock_guard lock(mutex_);
        if (!pending.empty()) lines_.push_back(pending);
        exitCode_ = int(code);
        running_ = false;
    });
    return true;
}

std::vector<std::string> ProcessRunner::takeLines() {
    std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
}

std::optional<int> ProcessRunner::exitCode() const {
    std::lock_guard lock(mutex_);
    return exitCode_;
}

fs::path find_mapbake(const std::string& setting) {
    std::error_code ec;
    if (!setting.empty()) {
        const fs::path p(std::u8string(setting.begin(), setting.end()));
        return fs::is_regular_file(p, ec) ? p : fs::path();
    }
    std::vector<fs::path> candidates;
#ifdef KAST_SOURCE_DIR
    for (const char* preset : {"release", "debug"})
        candidates.push_back(fs::path(KAST_SOURCE_DIR) / "services" / "world" / "build" / preset / "kast_mapbake.exe");
#endif
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) candidates.push_back(fs::path(self).parent_path() / "kast_mapbake.exe");
    for (const fs::path& p : candidates)
        if (fs::is_regular_file(p, ec)) return p;
    return {};
}

} // namespace zone
