#pragma once

#include "event_bus.h"
#include "gui.h"
#include "gui_events.h"
#include "scene.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rml {
class ElementDocument;
}

// A scene's game GUIs in Play mode (editor Game view and kast_player): one RmlUi document per UIDocument component,
// stacked in entity creation order (PersistentId), shown while visible. A missing or broken .rml/.rcss logs an error
// naming the file and that document shows nothing; Play goes on.
namespace kast {

class SceneGui {
public:
    explicit SceneGui(std::string contextName) : context_(std::move(contextName)) {}
    ~SceneGui() { clear(); }

    // Loads, unloads, shows and hides documents so they match scene's UIDocuments (paths relative to projectRoot;
    // empty = no project). Button clicks publish on bus (gui_events.h), which must outlive the loaded documents.
    void sync(const Scene& scene, const std::filesystem::path& projectRoot, EventBus& bus);
    // Unloads every document (Stop, kast.loadScene switch).
    void clear();
    // Reloads documents whose .rml or linked .rcss changed on disk since loaded (hot reload; never under a pak).
    void reloadChanged();
    // Clicks the element with this id in the topmost visible document holding one, as the mouse would. False if none.
    bool click(const std::string& id);
    GuiContext& context() { return context_; }

private:
    struct Entry {
        entt::entity entity = entt::null;
        EntityId id = 0;
        std::string path; // project-relative, as in the component
        bool visible = true;
        Rml::ElementDocument* document = nullptr; // null if it failed to load
        std::unique_ptr<ButtonClickListener> listener;
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files; // .rml, linked .rcss
    };

    void load(Entry& entry, EventBus& bus);
    void unload(Entry& entry);
    void restack();

    GuiContext context_;
    std::filesystem::path root_;
    EventBus* bus_ = nullptr;
    std::vector<Entry> entries_; // by id
};

} // namespace kast
