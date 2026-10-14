// Runs a command and fails if any of its windows becomes the foreground window while it runs (a --frames test run
// must not take focus from the user's app), or if it never shows a visible window (then nothing was checked).
// Usage: kast_focus_probe <exe> [args...]   Exit: 0 ok, 1 took focus / no window / command failed, 2 bad usage.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

// The command line after this program's own (possibly quoted) path.
std::wstring childCommandLine()
{
    const wchar_t* line = GetCommandLineW();
    if (*line == L'"') {
        const wchar_t* end = std::wcschr(line + 1, L'"');
        line = end ? end + 1 : L"";
    } else {
        while (*line && *line != L' ' && *line != L'\t')
            ++line;
    }
    while (*line == L' ' || *line == L'\t')
        ++line;
    return line;
}

struct VisibleSearch {
    DWORD pid;
    bool found;
};

BOOL CALLBACK checkWindow(HWND window, LPARAM param)
{
    auto* search = reinterpret_cast<VisibleSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && IsWindowVisible(window))
        search->found = true;
    return !search->found;
}

} // namespace

int main()
{
    std::wstring command = childCommandLine();
    if (command.empty()) {
        std::fprintf(stderr, "Usage: kast_focus_probe <exe> [args...]\n");
        return 2;
    }
    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "CreateProcess failed (%lu): %ls\n", GetLastError(), command.c_str());
        return 1;
    }
    CloseHandle(process.hThread);

    int samples = 0;
    int focused = 0;
    VisibleSearch visible{process.dwProcessId, false};
    do {
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        ++samples;
        if (foregroundPid == process.dwProcessId)
            ++focused;
        if (!visible.found)
            EnumWindows(checkWindow, reinterpret_cast<LPARAM>(&visible));
    } while (WaitForSingleObject(process.hProcess, 5) == WAIT_TIMEOUT);

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    std::printf("focus probe: %d samples, %d with the child in the foreground, window %s, exit code %lu\n", samples,
                focused, visible.found ? "visible" : "never visible", exitCode);
    if (focused > 0)
        std::fprintf(stderr, "FAIL: the child became the foreground window\n");
    if (!visible.found)
        std::fprintf(stderr, "FAIL: the child never showed a visible window\n");
    return focused > 0 || !visible.found || exitCode != 0 ? 1 : 0;
}
