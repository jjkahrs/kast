#pragma once

#include "gui.h"
#include "gui_access.h"
#include "renderer.h"
#include "scene.h"

#include <array>
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
// physics body lies between the camera and it. World mode: each is drawn into its tile of one shared atlas texture
// (one more RmlUi context) and shown as a quad of width / pixelsPerMeter x height / pixelsPerMeter meters centered on
// its anchor, turned by its facing, drawn by the Renderer with the scene's blended draws (depth-tested, so scene
// geometry hides it); hidden beyond maxDistance or behind the eye; the nearest get tiles first, the farthest go
// without when the atlas is full. SceneGui owns a WorldGui and draws it under its screen GUIs, and the atlas after
// them; kast.gui calls with an entity first reach its document (GuiAccess::writeEntity).
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

// World mode's atlas: kWorldUIAtlasSize pixels square, RGBA8 with mips (2048: 16 MiB, 21 with mips; room for 210
// 256x64 nameplates), its tiles kWorldUIAtlasPadding transparent pixels apart so the mips of neighbors don't mix.
constexpr int kWorldUIAtlasSize = 2048;
constexpr int kWorldUIAtlasPadding = 4;
struct WorldUITile {
    int x = 0, y = 0; // top-left, pixels
    int width = 0, height = 0;
    bool operator==(const WorldUITile&) const = default;
};
// Tiles for panels of these pixel sizes (width, height; nearest first) in an atlasSize square, packed in rows in that
// order; nullopt for a panel that doesn't fit in what is left (the ones after it are still tried).
std::vector<std::optional<WorldUITile>> packWorldUIAtlas(std::span<const std::pair<int, int>> sizes,
                                                         int atlasSize = kWorldUIAtlasSize);
// The corners (top-left, top-right, bottom-right, bottom-left) of a width x height meter quad centered on center.
// Billboard: in the camera's plane (cameraWorld: the inverse of the view matrix); YawOnly: upright, turned around world
// Y to the camera; Fixed: in the entity's XY plane (entityWorld; scale ignored), read from its +Z side.
std::array<bx::Vec3, 4> worldUIQuadCorners(WorldUIFacing facing, const bx::Vec3& center, float width, float height,
                                           const float* cameraWorld, const float* entityWorld);

class WorldGui {
public:
    explicit WorldGui(std::string contextName) : context_(contextName), atlas_(contextName + "-atlas") {}
    ~WorldGui() { clear(); }
    WorldGui(const WorldGui&) = delete;
    WorldGui& operator=(const WorldGui&) = delete;

    // Loads and unloads documents to match scene's WorldUIs that have a path (relative to projectRoot; empty = no
    // project): call once a frame. A missing or broken .rml logs an error once per path and shows nothing.
    void sync(const Scene& scene, const std::filesystem::path& projectRoot);
    // Places the documents for a frame drawn through view and proj on a width x height pixel viewport (see above);
    // physics occludes Overlay ones (null or not running: nothing does). A document loaded since is hidden until the
    // next call. Logs a warning the first time the atlas is full.
    void layout(const Scene& scene, const float* view, const float* proj, int width, int height, const Physics* physics);
    // The World-mode quads of the last layout(), for Renderer::setWorldQuads with atlasTexture() before the scene is
    // rendered. They show the atlas as the last renderAtlas() drew it: a panel's quad comes the frame after its tile.
    const std::vector<Renderer::WorldQuad>& quads() const { return quads_; }
    bgfx::TextureHandle atlasTexture() const;
    bool empty() const; // no Overlay document loaded: render would draw nothing
    // Draws the Overlay documents into view .. view + gui::kViews - 1 on frameBuffer, as GuiContext::render.
    void render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer);
    // Draws the World documents placed by layout() into the atlas (cleared first), in the views after those of another
    // context rendered just before in the same frame (GuiContext::render's shareViews); nothing if none is placed.
    void renderAtlas();
    void clear();   // unloads every document
    void reset();   // and forgets the kept writes and the logged paths (a new scene's systems start)
    void release(); // unloads every document, removes the contexts and frees the atlas (before gui::shutdown)
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
        WorldUIMode mode = WorldUIMode::Overlay; // the context it is in
        Rml::ElementDocument* document = nullptr; // null if it failed to load
        std::optional<WorldUITile> tile;  // World: where layout() placed it in the atlas
        std::optional<WorldUITile> shown; // World: where the last renderAtlas() drew it
    };

    void load(Entry& entry);
    void unload(Entry& entry);
    void apply(Rml::ElementDocument& document, EntityId entity, const std::string& id, const GuiWrite& write);
    void layoutOverlay(const Scene& scene, const float* viewProj, const bx::Vec3& eye, int width, int height,
                       const Physics* physics);
    void layoutWorld(const Scene& scene, const float* cameraWorld);

    GuiContext context_; // Overlay documents
    GuiContext atlas_;   // World documents, laid out kWorldUIAtlasSize square
    bgfx::FrameBufferHandle atlasFrameBuffer_ = BGFX_INVALID_HANDLE; // made by the first renderAtlas()
    std::vector<Renderer::WorldQuad> quads_;
    bool atlasWarned_ = false; // "atlas full" is logged once per run
    std::filesystem::path root_;
    std::vector<Entry> entries_;
    std::map<EntityId, std::map<std::string, GuiWrite>> written_;
    std::set<std::string> logged_; // paths whose failure was logged
};

} // namespace kast
