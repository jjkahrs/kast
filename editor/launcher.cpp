#include "launcher.h"

#include "build.h"
#include "document.h" // New Project rules: projectFolderName, newProjectTarget, newProjectError, createProject
#include "imgui_bgfx.h"
#include "recent_projects.h"
#include "ui_common.h"

#include <app.h>
#include <log.h>
#include <project.h>

#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <ctime>
#include <format>
#include <mutex>

namespace {

constexpr bgfx::ViewId kImGuiView = 255;
constexpr uint32_t kBackbufferClearColor = 0x202020ff;
constexpr const char* kEditorExeName = "kast_editor.exe";
constexpr float kNameScale = 1.3f; // project names are drawn this much larger than the base font
constexpr ImVec4 kErrorColor{1.0f, 0.45f, 0.45f, 1.0f};
// SDL filters match extensions only; Import rejects any other *.kast file name.
constexpr SDL_DialogFileFilter kProjectFilter{"Kast project (project.kast)", "kast"};

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path fromUtf8(const std::string& text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

int64_t now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string localTime(int64_t seconds)
{
    const std::time_t time = seconds;
    std::tm local{};
    char text[32] = "-";
    if (localtime_s(&local, &time) == 0)
        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

// The one open path (Open, double-click, Enter, Import, New Project, --launcher-open): loads file, bumps it to the top
// of list and saves the list, then starts kast_editor --project <root> [--projects-file <file>] <childArgs>. Returns
// the editor process, or null + error if the project is invalid (list unchanged) or the editor can't start (list still
// saved).
SDL_Process* openInEditor(std::vector<RecentProject>& list, const std::filesystem::path& file,
                          const LauncherOptions& options, std::string& error)
{
    if (file.filename() != kast::kProjectFileName) {
        error = std::format("'{}' is not a {} file", file.string(), kast::kProjectFileName);
        return nullptr;
    }
    kast::Project project;
    if (!kast::loadProject(file, project, error))
        return nullptr;
    bumpRecent(list, project.root, project.name, now());
    if (std::string saveError; !saveRecent(options.projectsFile, list, saveError))
        kast::log::warn(saveError);

    const char* base = SDL_GetBasePath(); // this exe's folder: the editor is the same kast_editor.exe
    if (!base) {
        error = std::format("Cannot find the editor's folder: {}", SDL_GetError());
        return nullptr;
    }
    std::vector<std::string> args{"--project", utf8(project.root)};
    if (!options.projectsFile.empty())
        args.insert(args.end(), {"--projects-file", utf8(options.projectsFile)});
    args.insert(args.end(), options.childArgs.begin(), options.childArgs.end());
    SDL_Process* editor = startProcess(fromUtf8(base) / kEditorExeName, args, error);
    if (editor)
        kast::log::info(std::format("Opening project '{}' in a new editor", project.name));
    return editor;
}

// Waits for the editor the Project Manager started and returns its exit code, so a terminal (run.bat) stays attached
// to it until it closes.
int waitForEditor(SDL_Process* editor)
{
    int code = 0;
    if (!SDL_WaitProcess(editor, true, &code))
        kast::log::error(std::format("Lost track of the editor process: {}", SDL_GetError()));
    SDL_DestroyProcess(editor);
    return code;
}

class Launcher {
public:
    explicit Launcher(const LauncherOptions& options) : options_(options), list_(loadRecent(options.projectsFile))
    {
        refresh();
        if (options.newProject)
            openNewProject(options.newProject->first, options.newProject->second);
    }

    void setWindow(SDL_Window* window) { window_ = window; }
    // The editor started by opening a project, or null; the caller waits for it after closing the window.
    SDL_Process* editor() const { return editor_; }

    void draw()
    {
        handleDialogResult();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Kast Project Manager", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float buttonWidth = ImGui::GetFontSize() * 9.0f;
        const float errorHeight = error_.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        ImGui::BeginChild("projects", ImVec2(-(buttonWidth + ImGui::GetStyle().ItemSpacing.x), -errorHeight),
                          ImGuiChildFlags_Borders);
        if (list_.empty())
            ImGui::TextDisabled("No projects yet. Create one with New Project, or add an existing one with Import.");
        for (int i = 0; i < static_cast<int>(list_.size()); ++i)
            drawRow(i);
        ImGui::EndChild();
        ImGui::SameLine();
        drawButtons(buttonWidth);
        if (!error_.empty())
            ImGui::TextColored(kErrorColor, "%s", error_.c_str());
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !openBlocker())
            openSelected();
        drawNewProjectModal();
        ImGui::End();
    }

private:
    enum class DialogKind { Import, NewProjectFolder };

    // Re-checks every entry (folder exists, project.kast loads) and keeps the selection in range.
    void refresh()
    {
        problems_.clear();
        for (const RecentProject& entry : list_)
            problems_.push_back(recentProblem(entry));
        if (selected_ >= static_cast<int>(list_.size()))
            selected_ = -1;
    }

    // Why Open is disabled, or null if the selected project can be opened.
    const char* openBlocker() const
    {
        if (selected_ < 0)
            return "Select a project";
        return problems_[selected_].empty() ? nullptr : problems_[selected_].c_str();
    }

    void drawRow(int i)
    {
        const RecentProject& entry = list_[i];
        const std::string& problem = problems_[i];
        const float line = ImGui::GetTextLineHeight();
        const float height = line * kNameScale + line * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::PushID(i);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected_ == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, height))) {
            selected_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && problem.empty())
                openSelected();
        }
        const ImVec2 bottom = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(top);
        if (!problem.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kNameScale);
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::PopFont();
        ImGui::TextUnformatted(entry.dir.string().c_str());
        ImGui::Text("Last opened %s", localTime(entry.lastOpened).c_str());
        if (!problem.empty()) {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(kErrorColor, "%s", problem.c_str());
        }
        ImGui::SetCursorScreenPos(bottom);
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // ImGui requires an item after moving the cursor back down
        ImGui::PopID();
    }

    void drawButtons(float width)
    {
        const ImVec2 size(width, 0.0f);
        ImGui::BeginGroup();
        ImGui::BeginDisabled(dialogOpen_);
        if (ImGui::Button("New Project", size))
            openNewProject("", "");
        if (ImGui::Button("Import", size)) {
            showDialog(DialogKind::Import);
            SDL_ShowOpenFileDialog(onDialogDone, this, window_, &kProjectFilter, 1, nullptr, false);
        }
        ImGui::Separator();
        const char* blocker = openBlocker();
        ImGui::BeginDisabled(blocker != nullptr);
        if (ImGui::Button("Open", size))
            openSelected();
        ImGui::EndDisabled();
        disabledTooltip(blocker);
        ImGui::BeginDisabled(selected_ < 0);
        if (ImGui::Button("Remove", size)) { // the list entry only: the project's files are never touched
            list_.erase(list_.begin() + selected_);
            selected_ = -1;
            save();
            refresh();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndGroup();
    }

    void save()
    {
        if (std::string error; !saveRecent(options_.projectsFile, list_, error)) {
            error_ = error;
            kast::log::error(error);
        }
    }

    void openSelected() { open(list_[selected_].dir / kast::kProjectFileName); }

    void open(const std::filesystem::path& file)
    {
        std::string error;
        editor_ = openInEditor(list_, file, options_, error);
        selected_ = -1;
        refresh(); // openInEditor may have added or moved an entry, even when the editor didn't start
        if (editor_) {
            SDL_HideWindow(window_); // gone for the user; the process waits for the editor (waitForEditor)
            kast::requestQuit();
            return;
        }
        error_ = error;
        kast::log::error(error);
    }

    void openNewProject(std::string name, std::string dir)
    {
        newProjectName_ = std::move(name);
        newProjectDir_ = std::move(dir);
        newProjectRequested_ = true;
    }

    void drawNewProjectModal()
    {
        if (std::exchange(newProjectRequested_, false))
            ImGui::OpenPopup("New Project");
        if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        const float fieldWidth = ImGui::GetFontSize() * 24.0f;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("Name", &newProjectName_);
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputText("Directory", &newProjectDir_);
        ImGui::SameLine();
        ImGui::BeginDisabled(dialogOpen_);
        if (ImGui::Button("Browse...")) {
            showDialog(DialogKind::NewProjectFolder);
            std::error_code ec;
            const bool startHere = std::filesystem::is_directory(newProjectDir_, ec);
            SDL_ShowOpenFolderDialog(onDialogDone, this, window_, startHere ? newProjectDir_.c_str() : nullptr, false);
        }
        ImGui::EndDisabled();

        const std::string error = newProjectError(newProjectName_, newProjectDir_);
        const bool previewable = !newProjectDir_.empty() && !projectFolderName(newProjectName_).empty();
        ImGui::Text("Will create: %s", previewable ? newProjectTarget(newProjectName_, newProjectDir_).string().c_str() : "-");
        if (!error.empty())
            ImGui::TextColored(kErrorColor, "%s", error.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(!error.empty() || dialogOpen_);
        const bool create = ImGui::Button("Create");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (create) {
            ImGui::CloseCurrentPopup();
            std::string createError;
            if (std::filesystem::path dir; createProject(newProjectName_, newProjectDir_, dir, createError)) {
                open(dir / kast::kProjectFileName);
            } else {
                error_ = createError;
                kast::log::error(createError);
            }
        } else if (ImGui::Button("Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !dialogOpen_)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // SDL calls onDialogDone on any thread; the main thread picks the result up in handleDialogResult().
    void showDialog(DialogKind kind)
    {
        dialogOpen_ = true;
        dialogKind_ = kind;
    }

    static void onDialogDone(void* userdata, const char* const* files, int /*filter*/)
    {
        auto* self = static_cast<Launcher*>(userdata);
        std::lock_guard lock(self->dialogMutex_);
        self->dialogDone_ = true;
        self->dialogPath_ = files && files[0] ? files[0] : "";
        self->dialogError_ = files ? "" : SDL_GetError();
    }

    void handleDialogResult()
    {
        std::string path;
        std::string dialogError;
        {
            std::lock_guard lock(dialogMutex_);
            if (!dialogDone_)
                return;
            dialogDone_ = false;
            path = std::move(dialogPath_);
            dialogError = std::move(dialogError_);
        }
        dialogOpen_ = false;
        if (!dialogError.empty()) {
            error_ = std::format("File dialog failed: {}", dialogError);
            kast::log::error(error_);
        } else if (path.empty()) {
            return; // cancelled
        } else if (dialogKind_ == DialogKind::NewProjectFolder) {
            newProjectDir_ = path;
        } else {
            open(fromUtf8(path)); // SDL gives UTF-8
        }
    }

    const LauncherOptions& options_;
    std::vector<RecentProject> list_;
    std::vector<std::string> problems_; // recentProblem of each list_ entry
    int selected_ = -1;
    std::string error_; // shown under the list until the next failure
    SDL_Window* window_ = nullptr;
    SDL_Process* editor_ = nullptr;
    std::string newProjectName_; // New Project modal fields
    std::string newProjectDir_;  // parent directory, typed or picked with Browse...
    bool newProjectRequested_ = false;

    bool dialogOpen_ = false;
    DialogKind dialogKind_ = DialogKind::Import;
    std::mutex dialogMutex_;
    bool dialogDone_ = false;
    std::string dialogPath_;  // empty = cancelled
    std::string dialogError_; // set if the dialog itself failed
};

} // namespace

int runLauncher(const LauncherOptions& options)
{
    kast::log::info("Kast Project Manager starting");
    Launcher launcher(options);
    UiScale uiScale(options.uiScale);
    FrameLimit frames(options.frames, options.screenshot);

    kast::AppHooks hooks;
    hooks.init = [&](SDL_Window* window) {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; // one fixed full-window layout: nothing to remember
        ImGui::StyleColorsDark();
        loadFont();
        uiScale.init(window);
        if (!ImGui_ImplSDL3_InitForOther(window)) {
            kast::log::error("ImGui SDL3 backend init failed");
            ImGui::DestroyContext();
            return false;
        }
        imgui_bgfx::init(kImGuiView);
        launcher.setWindow(window);
        bgfx::setViewClear(kImGuiView, BGFX_CLEAR_COLOR, kBackbufferClearColor);
        return true;
    };
    hooks.event = [&](const SDL_Event& event) {
        uiScale.onEvent(event);
        ImGui_ImplSDL3_ProcessEvent(&event);
    };
    hooks.frame = [&] {
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        launcher.draw();
        ImGui::Render();
        bgfx::touch(kImGuiView);
        imgui_bgfx::render(ImGui::GetDrawData());
        frames.endFrame();
    };
    hooks.screenshotSaved = [&](const char*, bool saved) { frames.onScreenshotSaved(saved); };
    int editorCode = 0;
    hooks.shutdown = [&] {
        if (launcher.editor()) // before SDL shuts down, which the process handle needs
            editorCode = waitForEditor(launcher.editor());
        imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    };

    const int code = kast::run({.title = "Kast Project Manager", .width = options.width, .height = options.height,
                                .activate = options.frames == 0},
                               hooks);
    if (code != 0)
        return code;
    return frames.failed() ? 1 : editorCode;
}

int launcherOpen(const LauncherOptions& options, const std::filesystem::path& dir)
{
    std::vector<RecentProject> list = loadRecent(options.projectsFile);
    std::string error;
    SDL_Process* editor = openInEditor(list, dir / kast::kProjectFileName, options, error);
    if (!editor) {
        kast::log::error(error);
        return 1;
    }
    return waitForEditor(editor);
}
