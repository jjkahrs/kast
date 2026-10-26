#pragma once

#include "gui.h"
#include "gui_access.h"
#include "scene.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// World-space UI (docs/engine/world-ui.html): the RmlUi documents of WorldUI components, one per entity, floating at
// their entities in Play mode (editor Game view, kast_player). Overlay mode: each is drawn screen-space at a constant
// pixel size, centered over its anchor (the entity's world matrix applied to its offset) with its bottom on it, nearer
// ones on top; hidden when the anchor is behind the eye or off the viewport, beyond maxDistance, or (occlude) when a
// physics body lies between the camera and it. SceneGui owns a WorldGui and draws it under its screen GUIs; kast.gui
// calls with an entity first reach its document (GuiAccess::writeEntity).
namespace kast {

class Physics;

struct ScreenPoint {
    float x = 0.0f, y = 0.0f; // pixels from the viewport's top-left
    float depth = 0.0f;       // in front of the eye along the view direction (clip w)
};
// point through viewProj (view then projection) on a width x height viewport; nullopt unless it is in front of the
// eye. x, y may lie outside the viewport.
std::optional<ScreenPoint> worldToScreen(const float* viewProj, const bx::Vec3& point, float width, float height);

struct WorldUIAnchor {
    bx::Vec3 position{0.0f, 0.0f, 0.0f};
    float maxDistance = 0.0f; // meters from the eye; 0: unlimited
};
struct WorldUIPlacement {
    size_t index = 0; // into the anchors
    ScreenPoint screen;
    float distance = 0.0f; // from the eye
};
// The anchors to draw seen from eye through viewProj on a width x height viewport: in front of the eye, on the
// viewport, within their maxDistance and not occluded (asked last, only for those; empty: none is), far to near (draw
// order: the nearest last, on top; equal distances keep the anchors' order).
std::vector<WorldUIPlacement> placeWorldUI(std::span<const WorldUIAnchor> anchors, const bx::Vec3& eye,
                                           const float* viewProj, float width, float height,
                                           const std::function<bool(size_t)>& occluded = {});
// True if physics hits a body between eye and anchor other than a trigger or one of owner's (its own, an ancestor's or
// a descendant's).
bool worldUIOccluded(const Physics& physics, const Scene& scene, entt::entity owner, const bx::Vec3& eye,
                     const bx::Vec3& anchor);

class WorldGui {
public:
    explicit WorldGui(std::string contextName) : context_(std::move(contextName)) {}
    ~WorldGui() { clear(); }
    WorldGui(const WorldGui&) = delete;
    WorldGui& operator=(const WorldGui&) = delete;

    // Loads and unloads documents to match scene's Overlay WorldUIs that have a path (relative to projectRoot; empty =
    // no project): call once a frame. A missing or broken .rml logs an error once per path and shows nothing.
    void sync(const Scene& scene, const std::filesystem::path& projectRoot);
    // Places the documents for a frame drawn through view and proj on a width x height pixel viewport (see above);
    // physics occludes (null or not running: nothing does). A document loaded since is hidden until the next call.
    void layout(const Scene& scene, const float* view, const float* proj, int width, int height, const Physics* physics);
    bool empty() const; // no document loaded: render would draw nothing
    // Draws them into view .. view + gui::kViews - 1 on frameBuffer, as GuiContext::render.
    void render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer);
    void clear();   // unloads every document
    void reset();   // and forgets the kept writes and the logged paths (a new scene's systems start)
    void release(); // unloads every document and removes the context (before gui::shutdown)
    // kast.gui with an entity first: applied to its document now and kept for documents that load later, as
    // GuiAccess::setText. entityValue: as GuiAccess::value, in its document only.
    void write(EntityId entity, const std::string& id, const GuiWrite& write);
    std::optional<GuiValue> value(EntityId entity, const std::string& id);
    Rml::ElementDocument* document(EntityId entity) const; // null if none is loaded (tests)

private:
    struct Entry {
        entt::entity entity = entt::null;
        EntityId id = 0;
        std::string path; // project-relative, as in the component
        Rml::ElementDocument* document = nullptr; // null if it failed to load
    };

    void load(Entry& entry);
    void unload(Entry& entry);
    void apply(Rml::ElementDocument& document, EntityId entity, const std::string& id, const GuiWrite& write);

    GuiContext context_;
    std::filesystem::path root_;
    std::vector<Entry> entries_;
    std::map<EntityId, std::map<std::string, GuiWrite>> written_;
    std::set<std::string> logged_; // paths whose failure was logged
};

} // namespace kast
