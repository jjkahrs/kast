#pragma once

#include "event_bus.h"
#include "gui.h"
#include "gui_access.h"
#include "gui_events.h"
#include "gui_lists.h"
#include "gui_models.h"
#include "gui_prefabs.h"
#include "gui_richtext.h"
#include "gui_widgets.h"
#include "scene.h"
#include "world_ui.h"

#include <filesystem>
#include <functional>
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
// drags, widget changes and list selections publish on the bus (gui_events.h, gui_widgets.h, gui_lists.h). A missing or broken .rml/.rcss logs an error
// naming the file and that document shows nothing; Play goes on.
// Scaling (gui_scale.h): Dp documents live in one RmlUi context, All documents in a second one laid out at the size /
// dp ratio and drawn over the first (so an All document is above every Dp one, whatever their ids). Why two: RmlUi's
// dp ratio and layout size are per context. Both act as one GUI: the mouse goes to the All context while it is over
// one of its elements, else to the Dp one; a click or focus() in one takes the keyboard there and blurs the other's
// focused element; click, focus, mouseOver, hovered and the data models span both.
// WorldUI documents (world_ui.h) are its WorldGui's: synced, cleared and reset with these, the Overlay ones drawn under
// them, the World ones into the WorldGui's atlas after them (the host hands its quads to the Renderer). The mouse goes
// to the screen GUIs first; the interactive WorldUI panels get it while it is over no screen GUI element. mouseOver
// counts both.
namespace kast {

class SceneGui final : public GuiAccess {
public:
    explicit SceneGui(std::string contextName)
        : context_(contextName), allContext_(contextName + "-all"), world_(contextName + "-world") {}
    ~SceneGui() override { clear(); }

    // Loads, unloads, shows and hides documents so they match scene's UIDocuments (paths relative to projectRoot;
    // empty = no project), then updates their lists (ListListener::update): call it once a frame before rendering.
    // Button clicks publish on bus (gui_events.h), which must outlive the loaded documents.
    void sync(const Scene& scene, const std::filesystem::path& projectRoot, EventBus& bus);
    // Unloads every document (Stop, kast.loadScene switch).
    void clear();
    // Reloads documents whose .rml or linked .rcss changed on disk since loaded (hot reload; never under a pak).
    void reloadChanged();
    // Clicks the element with this id in the topmost visible document holding one, as the mouse would. False if none.
    bool click(const std::string& id);
    GuiContext& context() { return context_; } // the Dp documents' (tests drive it directly)
    // The WorldUI documents: the host lays them out each frame from its camera (WorldGui::layout) before render().
    WorldGui& world() { return world_; }
    // The pixel size the GUIs draw in (a zero side, e.g. minimized: ignored, nothing recomputes), and the OS display
    // scale of the window's display; either recomputes the dp ratio. setScale (below) sets the rest.
    void resize(int width, int height);
    void setDisplayScale(double osScale);
    // Draws the Overlay WorldUI documents, then both contexts over them, into view .. view + gui::kViews - 1 on
    // frameBuffer, as GuiContext::render, then the World ones into their atlas in the views after those. shareViews:
    // another GUI drew in that range earlier this frame; these take the views after its (view is ignored).
    void render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, bool shareViews = false);
    // An SDL event, positions mapped as GuiContext::onEvent; routed between the two contexts, then the WorldUI panels
    // (see above).
    void onEvent(const SDL_Event& event, float originX = 0.0f, float originY = 0.0f, float scale = 1.0f);
    void release(); // unloads every document and removes both contexts (before gui::shutdown)
    // Called after each setScale (kast_player saves the player's UI scale).
    void setScaleListener(std::function<void(const GuiScale&)> listener) { scaleListener_ = std::move(listener); }

    // kast.gui.* (gui_access.h).
    std::optional<GuiValue> value(const std::string& id) override;
    void setValue(const std::string& id, GuiValue value) override;
    void scrollTo(const std::string& id, int index) override;
    void scrollToEnd(const std::string& id) override;
    void setText(const std::string& id, std::string_view text) override;
    void setRichText(const std::string& id, std::string_view text) override;
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
    bool instantiatePrefab(const std::string& prefab, const std::string& parentId, const std::string& newId) override;
    bool removeElement(const std::string& id) override;
    void reset() override;
    GuiScale scale() override { return scale_; }
    void setScale(const GuiScale& scale) override;
    double dpRatio() override { return dpRatio_; }
    void writeEntity(uint64_t entity, const std::string& id, const GuiWrite& write) override { world_.write(entity, id, write); }
    std::optional<GuiValue> entityValue(uint64_t entity, const std::string& id) override { return world_.value(entity, id); }
    void worldText(const std::array<float, 3>& position, std::string_view text, const std::string& cssClass,
                   float lifetime) override
    {
        world_.worldText({position[0], position[1], position[2]}, text, cssClass, lifetime);
    }
    size_t worldTextCount() override { return world_.worldTextCount(); }
    void advance(float dt) override { world_.advance(dt); }

private:
    using Written = GuiWrite;

    struct Entry {
        entt::entity entity = entt::null;
        EntityId id = 0;
        std::string path; // project-relative, as in the component
        bool visible = true;
        GuiScaling scaling = GuiScaling::Dp; // which context it is in
        Rml::ElementDocument* document = nullptr; // null if it failed to load
        std::unique_ptr<ButtonClickListener> listener;
        std::unique_ptr<DragListener> dragListener;
        std::unique_ptr<WidgetListener> widgetListener;
        std::unique_ptr<ListListener> listListener;
        std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> files; // .rml, .rcss, GUI prefabs
        std::vector<std::string> prefabSheets; // the GUI prefab .rcss it has (project paths)
    };

    void load(Entry& entry, EventBus& bus);
    void unload(Entry& entry);
    void restack();
    std::string documentPath(const Rml::Element& document) const;
    void apply(Entry& entry, Rml::ElementDocument& document, const std::string& id, const Written& written);
    bool applyValue(Entry& entry, Rml::Element& element, const std::string& id, const GuiValue& value); // false: logged
    void scroll(const std::string& id, int index, const char* function); // scrollTo, scrollToEnd
    Rml::ElementDocument* documentWith(const std::string& id) const; // the topmost loaded one holding id
    GuiPrefabReader prefabReader() const; // project files by project path
    GuiContext& contextFor(GuiScaling scaling) { return scaling == GuiScaling::All ? allContext_ : context_; }
    std::vector<Rml::Context*> contexts(); // the live ones (the Dp one made if needed); none without RmlUi
    void updateScale(); // the dp ratio from the size and settings, applied to both contexts
    void takeKeyboard(GuiScaling scaling); // that context gets keys and text; the other's focused element is blurred
    void routeScreen(const SDL_Event& event, float originX, float originY, float scale); // between the two contexts
    bool screenMouseOver(); // over an element of the screen GUIs

    GuiModels models_; // before the contexts: their models point into it
    GuiContext context_;    // Dp documents
    GuiContext allContext_; // All documents, made when the first loads
    WorldGui world_;
    GuiScaling keyboard_ = GuiScaling::Dp; // the context keys and text go to
    GuiScale scale_;
    double osScale_ = 1.0;
    int width_ = 0;
    int height_ = 0;
    double dpRatio_ = 1.0;
    std::optional<double> loggedRatio_;
    std::function<void(const GuiScale&)> scaleListener_;
    std::map<std::string, Written> written_;
    std::filesystem::path root_;
    EventBus* bus_ = nullptr;
    bool quiet_ = false; // while kast.gui writes a widget's value: its change event isn't the player's
    std::vector<Entry> entries_; // by id
};

} // namespace kast
