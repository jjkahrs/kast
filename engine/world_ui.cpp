#include "world_ui.h"

#include "files.h"
#include "gui_prefabs.h"
#include "gui_richtext.h"
#include "gui_widgets.h"
#include "log.h"
#include "physics.h"
#include "project.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace kast {
namespace {

constexpr float kMinDepth = 1e-4f;       // nearer to the eye plane than this counts as behind it
constexpr int kMaxOcclusionCasts = 8;     // bodies of the owner (or triggers) skipped per anchor, at most
constexpr float kOcclusionSkip = 1e-3f;   // meters past a skipped hit the next cast starts
constexpr float kOcclusionMargin = 1e-2f; // a hit this near the anchor (its own surface) doesn't occlude it
constexpr float kMinPixelsPerMeter = 1e-3f; // a World panel's pixelsPerMeter is taken as at least this

// kast.gui.worldText's default style: rising and fading over 1 s, centered on its point, never taking the mouse. A
// project's ui/world_text.rcss comes after it (same selectors win).
constexpr const char* kWorldTextStyle = R"(
body { pointer-events: none; }
.world-text { position: absolute; width: 400px; margin-left: -200px; height: 40px; margin-top: -20px; line-height: 40px;
              text-align: center; font-family: Roboto; font-size: 28px; font-weight: bold; color: #ffffff;
              font-effect: outline(2px #000000); pointer-events: none; animation: 1s world-text-rise; }
.damage { color: #ff4040; }
.heal { color: #40ff60; }
.xp { color: #c080ff; font-size: 22px; }
.crit { color: #ffcc00; font-size: 40px; }
@keyframes world-text-rise {
    0% { transform: translateY(0px); opacity: 1; }
    60% { transform: translateY(-36px); opacity: 1; }
    100% { transform: translateY(-60px); opacity: 0; }
}
)";

} // namespace

std::optional<ScreenPoint> worldToScreen(const float* viewProj, const bx::Vec3& point, float width, float height)
{
    const float clip[4] = {point.x, point.y, point.z, 1.0f};
    float out[4];
    bx::vec4MulMtx(out, clip, viewProj);
    if (!(out[3] > kMinDepth))
        return std::nullopt;
    return ScreenPoint{.x = (out[0] / out[3] * 0.5f + 0.5f) * width, .y = (0.5f - out[1] / out[3] * 0.5f) * height,
                       .depth = out[3]};
}

std::vector<WorldUIPlacement> placeWorldUI(std::span<const WorldUIAnchor> anchors, const bx::Vec3& eye,
                                           const float* viewProj, float width, float height,
                                           const std::function<bool(size_t)>& occluded)
{
    std::vector<WorldUIPlacement> out;
    for (size_t i = 0; i < anchors.size(); ++i) {
        const float distance = bx::length(bx::sub(anchors[i].position, eye));
        if (anchors[i].maxDistance > 0.0f && distance > anchors[i].maxDistance)
            continue;
        const std::optional<ScreenPoint> screen = worldToScreen(viewProj, anchors[i].position, width, height);
        if (!screen || screen->x < 0.0f || screen->x > width || screen->y < 0.0f || screen->y > height)
            continue;
        if (occluded && occluded(i))
            continue;
        out.push_back({.index = i, .screen = *screen, .distance = distance});
    }
    std::ranges::stable_sort(out, std::ranges::greater{}, &WorldUIPlacement::distance);
    return out;
}

bool worldUIOccluded(const Physics& physics, const Scene& scene, entt::entity owner, const bx::Vec3& eye,
                     const bx::Vec3& anchor)
{
    const bx::Vec3 toAnchor = bx::sub(anchor, eye);
    float left = bx::length(toAnchor) - kOcclusionMargin;
    if (left <= 0.0f)
        return false;
    const bx::Vec3 direction = bx::normalize(toAnchor);
    bx::Vec3 from = eye;
    for (int cast = 0; cast < kMaxOcclusionCasts; ++cast) {
        const std::optional<Physics::RayHit> hit = physics.raycast(from, direction, left);
        if (!hit)
            return false;
        const Collider* collider = scene.registry().try_get<Collider>(hit->entity);
        const bool skipped = (collider && collider->isTrigger) || scene.isSelfOrAncestor(hit->entity, owner) ||
                             scene.isSelfOrAncestor(owner, hit->entity);
        if (!skipped)
            return true;
        const float advance = hit->distance + kOcclusionSkip;
        from = bx::add(from, bx::mul(direction, advance));
        left -= advance;
        if (left <= 0.0f)
            return false;
    }
    return false; // ponytail: past kMaxOcclusionCasts skipped bodies the anchor counts as seen
}

std::vector<std::optional<WorldUITile>> packWorldUIAtlas(std::span<const std::pair<int, int>> sizes, int atlasSize)
{
    // ponytail: rows ("shelves") in distance order, repacked every frame; a skyline packer if mixed sizes waste room.
    std::vector<std::optional<WorldUITile>> tiles;
    int x = 0, y = 0, row = 0; // the free spot in the current row, its height so far
    for (const auto& [width, height] : sizes) {
        if (width <= 0 || height <= 0 || width > atlasSize || height > atlasSize) {
            tiles.emplace_back();
            continue;
        }
        int tx = x, ty = y, tallest = row;
        if (tx + width > atlasSize) { // a new row under this one
            tx = 0;
            ty += tallest;
            tallest = 0;
        }
        if (ty + height > atlasSize) { // the next, smaller panel may still fit where this one didn't
            tiles.emplace_back();
            continue;
        }
        tiles.push_back(WorldUITile{tx, ty, width, height});
        x = tx + width + kWorldUIAtlasPadding;
        y = ty;
        row = std::max(tallest, height + kWorldUIAtlasPadding);
    }
    return tiles;
}

std::array<bx::Vec3, 4> worldUIQuadCorners(WorldUIFacing facing, const bx::Vec3& center, float width, float height,
                                           const float* cameraWorld, const float* entityWorld)
{
    const bx::Vec3 cameraRight{cameraWorld[0], cameraWorld[1], cameraWorld[2]};
    bx::Vec3 right = cameraRight, up{cameraWorld[4], cameraWorld[5], cameraWorld[6]};
    if (facing == WorldUIFacing::YawOnly) {
        up = {0.0f, 1.0f, 0.0f};
        right = {cameraRight.x, 0.0f, cameraRight.z};
        if (bx::length(right) < 1e-4f) // the camera rolled on its side: face it by where it stands instead
            right = bx::cross(up, bx::sub({cameraWorld[12], 0.0f, cameraWorld[14]}, {center.x, 0.0f, center.z}));
        if (bx::length(right) < 1e-4f)
            right = {1.0f, 0.0f, 0.0f};
    } else if (facing == WorldUIFacing::Fixed) {
        right = {entityWorld[0], entityWorld[1], entityWorld[2]};
        up = {entityWorld[4], entityWorld[5], entityWorld[6]};
    }
    right = bx::mul(bx::normalize(right), width * 0.5f);
    up = bx::mul(bx::normalize(up), height * 0.5f);
    return {bx::add(bx::sub(center, right), up), bx::add(bx::add(center, right), up),
            bx::sub(bx::add(center, right), up), bx::sub(bx::sub(center, right), up)};
}

WorldUIRay worldUIMouseRay(const float* viewProj, const bx::Vec3& eye, float x, float y, float width, float height)
{
    float inverse[16];
    bx::mtxInverse(inverse, viewProj);
    const float ndcX = x / width * 2.0f - 1.0f, ndcY = 1.0f - y / height * 2.0f;
    // Two points on the ray (depths 0.5 and 1 are inside either clip depth range), then the origin on it nearest the
    // eye: the eye for a perspective camera, on its plane for an orthographic one.
    const bx::Vec3 a = bx::mulH({ndcX, ndcY, 0.5f}, inverse), b = bx::mulH({ndcX, ndcY, 1.0f}, inverse);
    const bx::Vec3 direction = bx::normalize(bx::sub(b, a));
    return {bx::mad(direction, bx::dot(bx::sub(eye, a), direction), a), direction};
}

std::optional<WorldUIQuadHit> worldUIRayQuad(const WorldUIRay& ray, const std::array<bx::Vec3, 4>& corners)
{
    const bx::Vec3 across = bx::sub(corners[1], corners[0]), down = bx::sub(corners[3], corners[0]);
    const bx::Vec3 normal = bx::cross(across, down);
    const float facing = bx::dot(normal, ray.direction);
    if (std::abs(facing) < 1e-8f)
        return std::nullopt; // edge on
    const float distance = bx::dot(normal, bx::sub(corners[0], ray.origin)) / facing;
    if (distance <= 0.0f)
        return std::nullopt;
    const bx::Vec3 local = bx::sub(bx::mad(ray.direction, distance, ray.origin), corners[0]);
    const float u = bx::dot(local, across) / bx::dot(across, across), v = bx::dot(local, down) / bx::dot(down, down);
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return std::nullopt;
    return WorldUIQuadHit{distance, u, v};
}

void WorldGui::sync(const Scene& scene, const std::filesystem::path& projectRoot, EventBus* bus)
{
    root_ = projectRoot;
    bus_ = bus;
    std::vector<Entry> next;
    for (auto [entity, ui, id] : scene.registry().view<const WorldUI, const PersistentId>().each()) {
        if (ui.path.empty())
            continue;
        // ponytail: a linear find per entity; index entries_ by entity if scenes get thousands of panels
        const auto old = std::ranges::find_if(entries_, [&](const Entry& e) {
            return e.entity == entity && e.path == ui.path && e.mode == ui.mode;
        });
        if (old != entries_.end()) {
            next.push_back(std::move(*old));
            entries_.erase(old);
        } else {
            next.push_back({.entity = entity, .id = id.value, .path = ui.path, .mode = ui.mode});
            load(next.back());
        }
    }
    for (Entry& gone : entries_)
        unload(gone);
    entries_ = std::move(next);
}

void WorldGui::layout(const Scene& scene, const float* view, const float* proj, int width, int height,
                      const Physics* physics)
{
    if (width <= 0 || height <= 0)
        return; // minimized: the documents keep their places
    float inverse[16];
    bx::mtxMul(viewProj_, view, proj);
    bx::mtxInverse(inverse, view);
    eye_ = {inverse[12], inverse[13], inverse[14]};
    width_ = width;
    height_ = height;
    scene_ = &scene;
    physics_ = physics;
    layoutOverlay(scene, viewProj_, eye_, width, height, physics);
    layoutWorld(scene, inverse);
    layoutTexts(viewProj_, width, height);
}

void WorldGui::layoutOverlay(const Scene& scene, const float* viewProj, const bx::Vec3& eye, int width, int height,
                             const Physics* physics)
{
    context_.resize(width, height);
    const entt::registry& registry = scene.registry();
    std::vector<WorldUIAnchor> anchors;
    std::vector<Entry*> candidates; // by anchor
    for (Entry& entry : entries_) {
        if (!entry.document || entry.mode != WorldUIMode::Overlay)
            continue;
        const WorldUI& ui = registry.get<WorldUI>(entry.entity); // sync ran this frame: the entity has it
        if (entry.interactive != ui.interactive) { // a panel that isn't lets the mouse through to the ones under it
            entry.interactive = ui.interactive;
            entry.document->SetProperty(Rml::PropertyId::PointerEvents,
                                        Rml::Property(ui.interactive ? Rml::Style::PointerEvents::Auto
                                                                     : Rml::Style::PointerEvents::None));
        }
        if (!ui.visible) {
            entry.document->Hide();
            continue;
        }
        float world[16];
        scene.worldMatrix(entry.entity, world);
        anchors.push_back({bx::mul(ui.offset, world), ui.maxDistance});
        candidates.push_back(&entry);
    }
    const bool occluding = physics && physics->running();
    const std::vector<WorldUIPlacement> placements =
        placeWorldUI(anchors, eye, viewProj, static_cast<float>(width), static_cast<float>(height), [&](size_t i) {
            return occluding && registry.get<WorldUI>(candidates[i]->entity).occlude &&
                   worldUIOccluded(*physics, scene, candidates[i]->entity, eye, anchors[i].position);
        });
    std::vector<bool> placed(candidates.size(), false);
    for (const WorldUIPlacement& placement : placements) { // far to near: each pulled over the ones before
        const WorldUI& ui = registry.get<WorldUI>(candidates[placement.index]->entity);
        Rml::ElementDocument& document = *candidates[placement.index]->document;
        const auto px = [](float value) { return Rml::Property(std::round(value), Rml::Unit::PX); };
        document.SetProperty(Rml::PropertyId::Left, px(placement.screen.x - static_cast<float>(ui.width) * 0.5f));
        document.SetProperty(Rml::PropertyId::Top, px(placement.screen.y - static_cast<float>(ui.height)));
        document.SetProperty(Rml::PropertyId::Width, px(static_cast<float>(ui.width)));
        document.SetProperty(Rml::PropertyId::Height, px(static_cast<float>(ui.height)));
        if (!document.IsVisible())
            document.Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        document.PullToFront();
        placed[placement.index] = true;
    }
    for (size_t i = 0; i < candidates.size(); ++i)
        if (!placed[i])
            candidates[i]->document->Hide();
}

void WorldGui::layoutWorld(const Scene& scene, const float* cameraWorld)
{
    atlas_.resize(kWorldUIAtlasSize, kWorldUIAtlasSize);
    const bx::Vec3 eye{cameraWorld[12], cameraWorld[13], cameraWorld[14]};
    const bx::Vec3 forward{-cameraWorld[8], -cameraWorld[9], -cameraWorld[10]}; // cameras look down their -Z
    struct Panel {
        Entry* entry;
        const WorldUI* ui;
        std::array<float, 16> world{};
        bx::Vec3 center{0.0f, 0.0f, 0.0f};
        float distance = 0.0f;
    };
    std::vector<Panel> panels;
    for (Entry& entry : entries_) {
        if (!entry.document || entry.mode != WorldUIMode::World)
            continue;
        entry.tile.reset();
        entry.corners.reset();
        const WorldUI& ui = scene.registry().get<WorldUI>(entry.entity); // sync ran this frame: the entity has it
        Panel panel{.entry = &entry, .ui = &ui};
        scene.worldMatrix(entry.entity, panel.world.data());
        panel.center = bx::mul(ui.offset, panel.world.data());
        const bx::Vec3 toPanel = bx::sub(panel.center, eye);
        panel.distance = bx::length(toPanel);
        const float metersPerPixel = 1.0f / std::max(ui.pixelsPerMeter, kMinPixelsPerMeter);
        const float radius = 0.5f * std::hypot(float(ui.width), float(ui.height)) * metersPerPixel;
        if (!ui.visible || (ui.maxDistance > 0.0f && panel.distance > ui.maxDistance) ||
            bx::dot(toPanel, forward) < -radius) { // wholly behind the eye
            entry.document->Hide();
            continue;
        }
        panels.push_back(panel);
    }
    std::ranges::stable_sort(panels, std::ranges::less{}, &Panel::distance); // the nearest get tiles first
    std::vector<std::pair<int, int>> sizes;
    for (const Panel& panel : panels)
        sizes.emplace_back(panel.ui->width, panel.ui->height);
    const std::vector<std::optional<WorldUITile>> tiles = packWorldUIAtlas(sizes);
    quads_.clear();
    size_t skipped = 0;
    for (size_t i = 0; i < panels.size(); ++i) {
        Entry& entry = *panels[i].entry;
        Rml::ElementDocument& document = *entry.document;
        if (!tiles[i]) {
            ++skipped;
            document.Hide();
            continue;
        }
        entry.tile = tiles[i];
        const auto px = [](int value) { return Rml::Property(float(value), Rml::Unit::PX); };
        document.SetProperty(Rml::PropertyId::Left, px(entry.tile->x));
        document.SetProperty(Rml::PropertyId::Top, px(entry.tile->y));
        document.SetProperty(Rml::PropertyId::Width, px(entry.tile->width));
        document.SetProperty(Rml::PropertyId::Height, px(entry.tile->height));
        if (!document.IsVisible())
            document.Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        if (!entry.shown) // its tile isn't drawn yet: from the next frame
            continue;
        const WorldUI& ui = *panels[i].ui;
        const float metersPerPixel = 1.0f / std::max(ui.pixelsPerMeter, kMinPixelsPerMeter);
        const auto uv = [](int pixels) { return float(pixels) / float(kWorldUIAtlasSize); };
        const WorldUITile& shown = *entry.shown;
        quads_.push_back({.corners = worldUIQuadCorners(ui.facing, panels[i].center, float(ui.width) * metersPerPixel,
                                                        float(ui.height) * metersPerPixel, cameraWorld,
                                                        panels[i].world.data()),
                          .uv = {uv(shown.x), uv(shown.y), uv(shown.x + shown.width), uv(shown.y + shown.height)}});
        entry.interactive = ui.interactive;
        entry.corners = quads_.back().corners;
    }
    if (skipped > 0 && !atlasWarned_) {
        atlasWarned_ = true;
        log::warn(std::format("World UI atlas full ({0}x{0}): the {1} farthest World panel(s) not drawn (logged once)",
                              kWorldUIAtlasSize, skipped));
    }
}

bgfx::TextureHandle WorldGui::atlasTexture() const
{
    return bgfx::isValid(atlasFrameBuffer_) ? bgfx::getTexture(atlasFrameBuffer_) : bgfx::TextureHandle BGFX_INVALID_HANDLE;
}

void WorldGui::layoutTexts(const float* viewProj, int width, int height)
{
    if (texts_.empty() || (!textDocument_ && !loadTextDocument()))
        return;
    textDocument_->PullToFront(); // over the panels
    for (FloatingText& text : texts_) {
        if (!text.element) {
            Rml::ElementPtr element = textDocument_->CreateElement("div");
            element->SetClassNames("world-text " + text.cssClass);
            element->SetInnerRML(guiTextRml(text.text));
            text.element = textDocument_->AppendChild(std::move(element));
        }
        const std::optional<ScreenPoint> at = worldToScreen(viewProj, text.position, float(width), float(height));
        const bool shown = at && at->x >= 0.0f && at->x <= float(width) && at->y >= 0.0f && at->y <= float(height);
        text.element->SetProperty(Rml::PropertyId::Visibility,
                                  Rml::Property(shown ? Rml::Style::Visibility::Visible : Rml::Style::Visibility::Hidden));
        if (shown) {
            text.element->SetProperty(Rml::PropertyId::Left, Rml::Property(std::round(at->x), Rml::Unit::PX));
            text.element->SetProperty(Rml::PropertyId::Top, Rml::Property(std::round(at->y), Rml::Unit::PX));
        }
    }
}

std::pair<const WorldGui::Entry*, Rml::Vector2f> WorldGui::pickWorld(float x, float y) const
{
    if (width_ <= 0 || height_ <= 0)
        return {};
    const WorldUIRay ray = worldUIMouseRay(viewProj_, eye_, x, y, float(width_), float(height_));
    const Entry* nearest = nullptr;
    WorldUIQuadHit hit;
    for (const Entry& entry : entries_) {
        if (!entry.interactive || !entry.corners || !entry.tile || !entry.document)
            continue;
        if (const std::optional<WorldUIQuadHit> h = worldUIRayQuad(ray, *entry.corners); h && (!nearest || h->distance < hit.distance)) {
            nearest = &entry;
            hit = *h;
        }
    }
    if (!nearest)
        return {};
    const bx::Vec3 point = bx::mad(ray.direction, hit.distance, ray.origin);
    if (physics_ && physics_->running() && scene_ && worldUIOccluded(*physics_, *scene_, nearest->entity, ray.origin, point))
        return {}; // scene geometry in front of it takes the click
    const WorldUITile& tile = *nearest->tile;
    return {nearest, {float(tile.x) + hit.u * float(tile.width), float(tile.y) + hit.v * float(tile.height)}};
}

void WorldGui::onEvent(const SDL_Event& event, float originX, float originY, float scale, bool blocked)
{
    const bool button = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    const bool leave = event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
    if (!gui::ready() || !(button || leave || event.type == SDL_EVENT_MOUSE_MOTION))
        return;
    const auto away = [](GuiContext& context) {
        if (context.created())
            context.context()->ProcessMouseLeave();
    };
    if (blocked || leave) {
        away(context_);
        away(atlas_);
        return;
    }
    if (context_.created()) { // Overlay panels are drawn over the scene: they come first
        context_.onEvent(event, originX, originY, scale);
        if (gui::overElement(*context_.context())) {
            away(atlas_);
            return;
        }
    }
    const float x = ((button ? event.button.x : event.motion.x) - originX) * scale;
    const float y = ((button ? event.button.y : event.motion.y) - originY) * scale;
    const auto [panel, at] = pickWorld(x, y);
    if (!panel) {
        away(atlas_);
        return;
    }
    SDL_Event mapped = event; // at its atlas pixel
    (button ? mapped.button.x : mapped.motion.x) = at.x;
    (button ? mapped.button.y : mapped.motion.y) = at.y;
    atlas_.onEvent(mapped);
}

bool WorldGui::mouseOver()
{
    const auto over = [](GuiContext& context) { return context.created() && gui::overElement(*context.context()); };
    return gui::ready() && (over(context_) || over(atlas_));
}

void WorldGui::worldText(const bx::Vec3& position, std::string_view text, const std::string& cssClass, float lifetime)
{
    if (texts_.size() >= kMaxWorldTexts) {
        removeText(texts_.front());
        texts_.pop_front();
    }
    texts_.push_back({.position = position, .left = lifetime, .text = std::string(text), .cssClass = cssClass});
}

void WorldGui::advance(float dt)
{
    for (FloatingText& text : texts_)
        text.left -= dt;
    std::erase_if(texts_, [this](const FloatingText& text) {
        if (text.left > 0.0f)
            return false;
        removeText(text);
        return true;
    });
}

void WorldGui::removeText(const FloatingText& text)
{
    if (text.element && textDocument_ && gui::ready())
        textDocument_->RemoveChild(text.element);
}

bool WorldGui::loadTextDocument()
{
    Rml::Context* context = context_.context();
    if (!context)
        return false;
    std::string style = kWorldTextStyle, reason, sheet;
    if (!root_.empty())
        if (const auto file = resolveProjectPath(root_, "ui/world_text.rcss", reason); file && readFile(*file, sheet))
            style += sheet; // the project's own over the defaults
    textDocument_ = context->LoadDocumentFromMemory("<rml><head><style>" + style + "</style></head><body></body></rml>",
                                                    "world_text");
    if (!textDocument_) {
        log::error("World UI: the floating text document did not load (see the RmlUi errors above)");
        texts_.clear(); // not tried again until the next worldText
        return false;
    }
    textDocument_->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
    textDocument_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return true;
}

bool WorldGui::empty() const
{
    return texts_.empty() && std::ranges::none_of(entries_, [](const Entry& entry) {
        return entry.document != nullptr && entry.mode == WorldUIMode::Overlay;
    });
}

void WorldGui::render(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer) { context_.render(view, frameBuffer); }

void WorldGui::renderAtlas()
{
    const bool placed = std::ranges::any_of(entries_, [](const Entry& entry) { return entry.tile.has_value(); });
    if (placed && !bgfx::isValid(atlasFrameBuffer_)) {
        const bgfx::TextureHandle color =
            bgfx::createTexture2D(kWorldUIAtlasSize, kWorldUIAtlasSize, true, 1, bgfx::TextureFormat::RGBA8,
                                  BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP); // mips made as drawn
        atlasFrameBuffer_ = bgfx::createFrameBuffer(1, &color, true);
        bgfx::setName(color, "World UI atlas");
    }
    if (placed)
        atlas_.render(0, atlasFrameBuffer_, true, true);
    for (Entry& entry : entries_)
        entry.shown = entry.tile;
}

void WorldGui::clear()
{
    for (Entry& entry : entries_)
        unload(entry);
    entries_.clear();
}

void WorldGui::reset()
{
    clear();
    written_.clear();
    logged_.clear();
    texts_.clear();
    if (textDocument_ && gui::ready())
        textDocument_->Close(); // the next session's first text reads ui/world_text.rcss again
    textDocument_ = nullptr;
}

void WorldGui::release()
{
    clear();
    texts_.clear();
    textDocument_ = nullptr; // closed with its context
    context_.release();
    atlas_.release();
    if (bgfx::isValid(atlasFrameBuffer_))
        bgfx::destroy(atlasFrameBuffer_); // and its texture
    atlasFrameBuffer_ = BGFX_INVALID_HANDLE;
}

void WorldGui::write(EntityId entity, const std::string& id, const GuiWrite& write)
{
    GuiWrite& kept = written_[entity][id];
    if (write.text || write.rich) {
        kept.text = write.text;
        kept.rich = write.rich;
    }
    if (write.visible)
        kept.visible = write.visible;
    for (const auto& [name, value] : write.attributes)
        kept.attributes[name] = value;
    if (write.value)
        kept.value = write.value;
    for (Entry& entry : entries_)
        if (entry.id == entity && entry.document)
            apply(*entry.document, entity, id, write);
}

std::optional<GuiValue> WorldGui::value(EntityId entity, const std::string& id)
{
    Rml::ElementDocument* found = document(entity);
    Rml::Element* element = found ? found->GetElementById(id) : nullptr;
    return element ? guiWidgetValue(*element) : std::nullopt;
}

Rml::ElementDocument* WorldGui::document(EntityId entity) const
{
    const auto entry = std::ranges::find(entries_, entity, &Entry::id);
    return entry != entries_.end() ? entry->document : nullptr;
}

void WorldGui::apply(Rml::ElementDocument& document, EntityId entity, const std::string& id, const GuiWrite& write)
{
    Rml::Element* element = document.GetElementById(id);
    if (!element)
        return;
    applyGuiWrite(*element, write);
    if (std::string error; write.value && !setGuiWidgetValue(*element, *write.value, error))
        log::warn(std::format("kast.gui.setValue({}, '{}'): {}: unchanged", entity, id, error));
}

void WorldGui::load(Entry& entry)
{
    entry.document = nullptr;
    GuiContext& owner = entry.mode == WorldUIMode::World ? atlas_ : context_;
    if (entry.mode == WorldUIMode::World)
        owner.resize(kWorldUIAtlasSize, kWorldUIAtlasSize);
    Rml::Context* context = owner.context();
    if (!context)
        return;
    const auto fail = [&](const std::string& why) {
        if (logged_.insert(entry.path).second)
            log::error(std::format("World UI document '{}'{}", entry.path, why));
    };
    std::string reason = "no project is open";
    const std::optional<std::filesystem::path> file = root_.empty() ? std::nullopt : resolveProjectPath(root_, entry.path, reason);
    std::string text;
    if (!file)
        return fail(": " + reason);
    if (!readFile(*file, text))
        return fail(" not found");
    GuiPrefabExpansion prefabs{.document = entry.path, .read = [root = root_](const std::string& path) -> std::optional<std::string> {
        std::string ignored, text;
        const std::optional<std::filesystem::path> file = resolveProjectPath(root, path, ignored);
        return file && readFile(*file, text) ? std::optional(std::move(text)) : std::nullopt;
    }};
    const std::string rml = guiExpandPrefabsRml(text, prefabs);
    gui::loadProjectFonts(root_); // first: a bad font file's RmlUi error isn't this document's
    const uint64_t problems = gui::problemCount();
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, gui::toRmlPath(*file));
    if (!document || gui::problemCount() != problems) {
        if (document)
            document->Close();
        return fail(" (or its style sheet) has errors, see above: not shown");
    }
    gui::convertRichText(*document);
    gui::addWidgetStyle(*document);
    document->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::Block)); // so it takes the size
    document->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
    if (entry.mode == WorldUIMode::World) { // nothing drawn past its tile, into a neighbor's
        document->SetProperty(Rml::PropertyId::OverflowX, Rml::Property(Rml::Style::Overflow::Hidden));
        document->SetProperty(Rml::PropertyId::OverflowY, Rml::Property(Rml::Style::Overflow::Hidden));
    }
    if (const auto written = written_.find(entry.id); written != written_.end())
        for (const auto& [id, write] : written->second) // what Lua wrote before this document loaded
            apply(*document, entry.id, id, write);
    if (entry.mode == WorldUIMode::Overlay) // no mouse until layout() sees it interactive (World: only those get any)
        document->SetProperty(Rml::PropertyId::PointerEvents, Rml::Property(Rml::Style::PointerEvents::None));
    if (bus_) {
        entry.listener = std::make_unique<ButtonClickListener>(*bus_, entry.path, entry.id);
        document->AddEventListener(Rml::EventId::Click, entry.listener.get());
    }
    entry.document = document; // hidden until layout() places it
}

void WorldGui::unload(Entry& entry)
{
    if (entry.document && gui::ready()) {
        if (entry.listener)
            entry.document->RemoveEventListener(Rml::EventId::Click, entry.listener.get());
        entry.document->Close();
    }
    entry.document = nullptr;
    entry.listener.reset();
}

} // namespace kast
