#pragma once

namespace kast {

// When the environment sets KAST_NO_CRASH_DIALOGS=1 (ctest and Claude Code sessions do), abort(), failed asserts, CRT
// debug reports and crashes write to stderr and end the process instead of opening a modal dialog that waits for a
// click. Unset, the dialogs stay so a developer can press Retry to attach a debugger. Call first thing in main.
// Same as engine/crash_dialogs.cpp; the services build without the engine, so kast_map carries its own copy.
void disableCrashDialogsIfRequested();

} // namespace kast
