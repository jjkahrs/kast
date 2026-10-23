#pragma once

#include "event_bus.h"
#include "gui.h"
#include "gui_access.h"
#include "gui_events.h"
#include "gui_models.h"
#include "gui_widgets.h"
#include "scene.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml {
class Element;
class ElementDocument;
} // namespace Rml

// A scene's game GUIs in Play mode (editor Game view and kast_player): one RmlUi document per UIDocument component,
// stacked in entity creation order (PersistentId), shown while visible, each over the default widget style. Clicks,
// drags and widget changes publish on the bus (gui_events.h, gui_widgets.h). A missing or broken .rml/.rcss logs an error
// naming the file and that document shows nothing; Play goes on.
namespace kast {

// text as RML: &, <, > and " escaped, "\n" as <br/> (GuiAccess::setText on an element that isn't a text field).
std::string guiTextRml(std::string_view text);

class SceneGui final : public GuiAccess {
public:
    explicit SceneGui(std::string contextName) : context_(std::move(contextName)) {}
    ~SceneGui() override { clear(); }

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

    // kast.gui.* (gui_access.h).
    std::optional<GuiValue> value(const std::string& id) override;
    void setValue(const std::string& id, GuiValue value) override;
    void setText(const std::string& id, std::string_view text) override;
    void setVisible(const std::string& id, bool visible) override;
    void setAttribute(const std::string& id, const std::string& name, std::string_view value) override;
    bool focus(const std::string& id) override;
    std::optional<std::string> focusedField() override;
    bool mouseOver() override;
    std::optional<std::string> hovered() override;
    void setModelValue(const std::string& model, const std::string& key, GuiValue value) override;
    std::optional<GuiValue> modelValue(const std::string& model, const std::string& key) override;
    bool cloneElement(const std::string& templateId, const std::string& parentId, const std::string& newId) override;
    bool createElement(const std::string& parentId, const std::string& tag, const std::string& newId,
                       const std::map<std::string, std::string>& attributes) override;
    bool removeElement(const std::string& id) override;
    void reset() override;

private:
    struct Written { // kast.gui writes to one id, applied again to documents that load later
        std::optional<std::string> text;
        std::optional<bool> visible;
        std::map<std::string, std::string> attributes;
        std::optional<GuiValue> value;
    };

    struct Entry {
        entt::entity entity = entt::null;
        EntityId id = 0;
        std::string path; // project-relative, as in the component
        bool visible = true;
        Rml::ElementDocument* document = nullptr; // null if it failed to load
        std::unique_ptr<ButtonClickListener> listener;
        std::unique_ptr<DragListener> dragListener;
        std::unique_ptr<WidgetListener> widgetListener;
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files; // .rml, linked .rcss
    };

    void load(Entry& entry, EventBus& bus);
    void unload(Entry& entry);
    void restack();
    std::string documentPath(const Rml::Element& document) const;
    void apply(Rml::ElementDocument& document, const std::string& id, const Written& written);
    bool applyValue(Rml::Element& element, const std::string& id, const GuiValue& value); // false: logged
    Rml::ElementDocument* documentWith(const std::string& id) const; // the topmost loaded one holding id

    GuiModels models_; // before context_: the context's models point into it
    GuiContext context_;
    std::map<std::string, Written> written_;
    std::filesystem::path root_;
    EventBus* bus_ = nullptr;
    bool quiet_ = false; // while kast.gui writes a widget's value: its change event isn't the player's
    std::vector<Entry> entries_; // by id
};

} // namespace kast
