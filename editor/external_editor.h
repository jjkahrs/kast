#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Opens text assets in the user's own code editor (Assets panel). The command is a per-user template stored under
// "externalEditor" in editor_settings.json (editor_settings.h), shared by every project. No ImGui.

constexpr const char* kDefaultEditorCommand = "code \"{file}\"";

// The "externalEditor" command in file; kDefaultEditorCommand if the file is missing, or (with a warning) invalid.
std::string loadEditorCommand(const std::filesystem::path& file);
// Sets "externalEditor" to command in file, keeping its other settings. False + error on failure.
bool saveEditorCommand(const std::filesystem::path& file, const std::string& command, std::string& error);
// command with every "{file}" replaced by file's path (UTF-8); without "{file}", the quoted path is appended.
std::string expandEditorCommand(std::string_view command, const std::filesystem::path& file);

class ExternalEditor {
public:
    explicit ExternalEditor(std::filesystem::path settingsFile);
    ~ExternalEditor();
    ExternalEditor(const ExternalEditor&) = delete;
    ExternalEditor& operator=(const ExternalEditor&) = delete;

    const std::string& command() const { return command_; }
    // Saves command as the new setting; used from the next open() on. False + error if it can't be written.
    bool setCommand(std::string command, std::string& error);
    // Runs the command for file without waiting for it. If it can't start, opens file with its Windows association.
    void open(const std::filesystem::path& file);
    // Call every frame: a command that exits with an error soon after open() (e.g. not found) falls back to the
    // Windows association too.
    void poll();

private:
    struct Launch {
        void* process; // HANDLE of the cmd.exe running the command
        std::filesystem::path file;
        std::string command;
        uint64_t startedMs;
    };

    std::filesystem::path settingsFile_;
    std::string command_;
    std::vector<Launch> launches_;
};
