#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Project Manager: the window kast_editor opens without a project or scene (see README "Project Manager"). It lists
// recent projects and creates, imports or opens one by starting a new kast_editor --project <dir>, then closes and waits
// for that editor.

struct LauncherOptions {
    std::filesystem::path projectsFile;  // the recent-projects list (recent_projects.h)
    std::vector<std::string> childArgs;  // appended to the started editor's arguments (the flags after --)
    int frames = 0;                      // --frames / --screenshot, as for the editor
    std::string screenshot;
    int width = 1024;
    int height = 600;
    std::optional<float> uiScale;
    std::optional<std::pair<std::string, std::string>> newProject; // name, dir: open the prefilled New Project modal
};

// Runs the window until it is closed (0) or a project is opened: then the window hides and this waits for the editor it
// started and returns the editor's exit code, so a terminal stays attached to the editor. Nonzero if the window or a
// --screenshot fails.
int runLauncher(const LauncherOptions& options);
// --launcher-open: opens dir exactly as double-clicking it does, without a window, and waits for the editor like
// runLauncher. 1 if the project is invalid or the editor can't start.
int launcherOpen(const LauncherOptions& options, const std::filesystem::path& dir);
