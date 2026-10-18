#include "viewport.h"

#include "ui_common.h"

#include <log.h>
#include <model.h>

#include <imgui.h>
#include <imgui_internal.h> // dock tab visibility
#include <ImGuizmo.h>       // after imgui.h, which it needs

#include <algorithm>
#include <format>
#include <limits>
#include <optional>

namespace {

constexpr float kFlySpeed = 5.0f; // meters per second
constexpr float kFastMultiplier = 4.0f;
constexpr ImU32 kPlayTint = IM_COL32(204, 85, 0, 255); // darker than the accent: Scene panel border and active play button while playing
constexpr float kCameraPickHalfSize = 0.25f; // cameras have no mesh: click within this box around the origin

// The entity's local mesh bounds: its primitive's, its model's (bind pose; the error cube's if the model doesn't load),
// or none.
std::optional<kast::Aabb> localBounds(const kast::Scene& scene, entt::entity entity)
{
    if (const auto* mesh = scene.registry().try_get<kast::MeshRenderer>(entity))
        return kast::primitiveBounds(mesh->primitive);
    const auto* renderer = scene.registry().try_get<kast::ModelRenderer>(entity);
    if (!renderer || renderer->model.empty())
        return std::nullopt;
    const auto model = kast::models().get(renderer->model);
    return model ? model->bounds : kast::primitiveBounds(kast::Primitive::Cube);
}

kast::Aabb worldBounds(const kast::Scene& scene, entt::entity entity)
{
    float world[16];
    scene.worldMatrix(entity, world);
    const bx::Vec3 origin{world[12], world[13], world[14]};
    const std::optional<kast::Aabb> bounds = localBounds(scene, entity);
    if (!bounds)
        return {origin, origin};
    const kast::Aabb& local = *bounds;
    kast::Aabb box{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
                   {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()}};
    for (int corner = 0; corner < 8; ++corner) {
        const bx::Vec3 p = bx::mul(bx::Vec3{corner & 1 ? local.max.x : local.min.x, corner & 2 ? local.max.y : local.min.y,
                                            corner & 4 ? local.max.z : local.min.z},
                                   world);
        box.min = bx::min(box.min, p);
        box.max = bx::max(box.max, p);
    }
    return box;
}

// fn(entity, distance along ray) for each mesh or model the ray hits (distance >= 0 on a hit, else negative).
template <typename Fn>
void forEachMeshHit(const kast::Scene& scene, const kast::Ray& ray, Fn&& fn)
{
    for (entt::entity entity : scene.registry().view<kast::Transform>()) {
        const std::optional<kast::Aabb> bounds = localBounds(scene, entity);
        if (!bounds)
            continue;
        // Test in the mesh's local space: exact for rotated/scaled boxes. The direction is left unnormalized
        // so the hit parameter is the same distance along the world ray.
        float world[16], inverse[16];
        scene.worldMatrix(entity, world);
        bx::mtxInverse(inverse, world);
        const kast::Ray local{bx::mul(ray.origin, inverse), kast::transformDir(ray.direction, inverse)};
        fn(entity, kast::intersect(local, *bounds));
    }
}

constexpr float kDropDistance = 10.0f; // a prefab dropped where the cursor ray hits nothing: this far along it

} // namespace

kast::Ray editorCameraRay(const Document& doc, float u, float v, float aspect, bool homogeneousDepth)
{
    float view[16], proj[16], viewProj[16];
    doc.camera.viewMatrix(view);
    doc.camera.projMatrix(proj, aspect);
    bx::mtxMul(viewProj, view, proj);
    return kast::screenRay(viewProj, u, v, 1.0f, 1.0f, homogeneousDepth);
}

entt::entity dropPrefabOnView(Document& doc, const std::string& path, const kast::Ray& ray, std::string& error)
{
    float nearest = std::numeric_limits<float>::max();
    forEachMeshHit(doc.scene, ray, [&](entt::entity, float distance) {
        if (distance >= 0.0f)
            nearest = std::min(nearest, distance);
    });
    if (nearest == std::numeric_limits<float>::max())
        nearest = ray.direction.y != 0.0f ? -ray.origin.y / ray.direction.y : -1.0f; // the y = 0 plane, if ahead
    if (nearest < 0.0f)
        nearest = kDropDistance;
    const entt::entity entity = dropPrefab(doc, path, entt::null, error);
    if (entity != entt::null)
        doc.scene.registry().get<kast::Transform>(entity).position = bx::mad(ray.direction, nearest, ray.origin);
    return entity;
}

bool beginViewportPanel(const char* name, Document& doc, PanelFocus panel)
{
    const bool requested = doc.focusRequest == panel;
    if (requested)
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // A docked tab is selected a frame after focus, and not at all while its dock node is being (re)built at startup,
    // so keep asking until the tab is really shown.
    const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
    if (requested && visible && ImGui::IsWindowFocused() && (!window->DockIsActive || window->DockTabIsVisible))
        doc.focusRequest = PanelFocus::None;
    return visible;
}

void SceneViewport::init()
{
    renderer_.init();
    post_.init();
    operation_ = ImGuizmo::TRANSLATE;
}

void SceneViewport::shutdown()
{
    target_.destroy();
    post_.shutdown();
    renderer_.shutdown();
}

void ViewTarget::destroy()
{
    if (bgfx::isValid(frameBuffer_))
        bgfx::destroy(frameBuffer_);
    if (bgfx::isValid(color_) && bgfx::isValid(depth_))
        bgfx::destroy(color_); // the shared depth isn't ours; otherwise the framebuffer destroyed both
    frameBuffer_ = BGFX_INVALID_HANDLE;
    color_ = BGFX_INVALID_HANDLE;
}

void ViewTarget::resize(uint16_t width, uint16_t height, bgfx::TextureHandle depth)
{
    if ((width == width_ && height == height_ && depth.idx == depth_.idx) || width == 0 || height == 0)
        return;
    destroy();
    const bool shared = bgfx::isValid(depth);
    color_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8,
                                   BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    const bgfx::TextureHandle textures[] = {
        color_, shared ? depth : bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8,
                                                        BGFX_TEXTURE_RT_WRITE_ONLY)};
    frameBuffer_ = bgfx::createFrameBuffer(2, textures, !shared);
    width_ = width;
    height_ = height;
    depth_ = depth;
    bgfx::setViewName(view_, name_);
    bgfx::setViewFrameBuffer(view_, frameBuffer_);
    bgfx::setViewRect(view_, 0, 0, width, height);
    if (shared)
        bgfx::setViewClear(view_, BGFX_CLEAR_NONE);
    else
        bgfx::setViewClear(view_, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, kViewportClearColor, 1.0f, 0);
    kast::log::info(std::format("{} framebuffer resized to {}x{}", name_, width, height));
}

void SceneViewport::draw(Document& doc)
{
    // Not while a modal is up: e.g. the unsaved-changes prompt would then save or discard a play-mode scene.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteGlobal) && !ImGui::GetTopMostPopupModal())
    {
        if (doc.play == PlayState::Stopped)
            startPlay(doc);
        else
            stopPlay(doc);
    }

    const bool visible = beginViewportPanel("Scene", doc, PanelFocus::Scene);
    if (visible) {
        drawToolbar(doc);
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        const auto width = static_cast<uint16_t>(size.x * scale.x);
        const auto height = static_cast<uint16_t>(size.y * scale.y);
        post_.begin(width, height, kast::sceneClearColor(doc.scene.postProcessing, kViewportClearColor));
        target_.resize(width, height, post_.depth());
        if (target_.valid() && size.x > 0.0f && size.y > 0.0f) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
            if (std::string model; acceptModelDrop(model)) // from the Assets panel: a new entity at the origin
                createModelEntity(doc, model, entt::null);
            if (std::string prefab; acceptPrefabDrop(prefab)) { // where the cursor points
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const kast::Ray ray = editorCameraRay(doc, (mouse.x - origin.x) / size.x, (mouse.y - origin.y) / size.y,
                                                      size.x / size.y, bgfx::getCaps()->homogeneousDepth);
                if (std::string error; dropPrefabOnView(doc, prefab, ray, error) == entt::null)
                    kast::log::error(std::format("Cannot add an instance of '{}': {}", prefab, error));
            }
            const bool hovered = ImGui::IsItemHovered();
            if (doc.play != PlayState::Stopped) {
                ImGui::GetWindowDrawList()->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), kPlayTint,
                                                    0.0f, 0, ImGui::GetStyle().FramePadding.y); // scales with the UI
            }

            updateCamera(doc, hovered, size.y);
            float view[16], proj[16], viewProj[16];
            doc.camera.viewMatrix(view);
            doc.camera.projMatrix(proj, size.x / size.y);
            bx::mtxMul(viewProj, view, proj);
            renderer_.render(doc.scene, doc.project ? doc.project->root : std::filesystem::path(), post_.sceneView(), view,
                             proj, shadowViews_);
            post_.apply(doc.scene.postProcessing, postEffects_, target_.frameBuffer());
            renderer_.renderOverlays(doc.scene, target_.view(), view, proj, doc.selected);

            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
            const bool gizmoShown = doc.scene.valid(doc.selected);
            if (gizmoShown)
                manipulate(doc, view, proj);

            const ImGuiIO& io = ImGui::GetIO();
            const bool onGizmo = gizmoShown && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !onGizmo)
                pick(doc, viewProj, io.MousePos.x - origin.x, io.MousePos.y - origin.y, size.x, size.y);
            shortcuts(doc);
        }
    }
    ImGui::End();
}

void SceneViewport::drawToolbar(Document& doc)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 start = ImGui::GetCursorPos(); // below the tab bar
    ImGui::SetCursorPos(ImVec2(start.x + style.FramePadding.x * 2.0f, start.y + style.FramePadding.y));

    // Play controls first so they stay visible when the panel is narrow.
    const auto playButton = [](const char* label, bool active, bool enabled) {
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, kPlayTint);
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::SmallButton(label);
        ImGui::EndDisabled();
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        return pressed;
    };
    const bool stopped = doc.play == PlayState::Stopped;
    if (playButton("Play (Ctrl+P)", doc.play == PlayState::Playing, doc.play != PlayState::Playing && !doc.prefabEdit))
        startPlay(doc);
    disabledTooltip(doc.prefabEdit ? "Play is disabled while a prefab is open: go Back to the scene first" : nullptr);
    if (playButton("Pause", doc.play == PlayState::Paused, !stopped))
        togglePause(doc);
    if (playButton("Stop", false, !stopped))
        stopPlay(doc);
    ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
    if (ImGui::RadioButton("Move (W)", operation_ == ImGuizmo::TRANSLATE))
        operation_ = ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", operation_ == ImGuizmo::ROTATE))
        operation_ = ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", operation_ == ImGuizmo::SCALE))
        operation_ = ImGuizmo::SCALE;
    ImGui::SameLine();
    if (ImGui::SmallButton(local_ ? "Local (X)" : "World (X)"))
        local_ = !local_;
    ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
    ImGui::Checkbox("Post", &postEffects_);
    ImGui::SetItemTooltip("Show the scene's post-processing effects (Window > Post Processing); fog always shows");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.FramePadding.y);
}

void SceneViewport::updateCamera(Document& doc, bool hovered, float viewportHeight)
{
    const ImGuiIO& io = ImGui::GetIO();
    EditorCamera& camera = doc.camera;
    if (hovered) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            ImGui::SetWindowFocus(); // right/middle clicks don't focus windows on their own
        if (drag_ == Drag::None) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                drag_ = Drag::Fly;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                drag_ = Drag::Pan;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt)
                drag_ = Drag::Orbit;
            else if (io.MouseWheel != 0.0f)
                camera.dolly(io.MouseWheel);
        }
    }

    const ImVec2 delta = io.MouseDelta;
    switch (drag_) {
    case Drag::Fly: {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            drag_ = Drag::None;
            break;
        }
        camera.look(delta.x, delta.y);
        bx::Vec3 move{0.0f, 0.0f, 0.0f};
        const auto axis = [](ImGuiKey positive, ImGuiKey negative) {
            return (ImGui::IsKeyDown(positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(negative) ? 1.0f : 0.0f);
        };
        move = bx::add(move, bx::mul(camera.forward(), axis(ImGuiKey_W, ImGuiKey_S)));
        move = bx::add(move, bx::mul(camera.right(), axis(ImGuiKey_D, ImGuiKey_A)));
        move = bx::add(move, bx::mul(bx::Vec3{0.0f, 1.0f, 0.0f}, axis(ImGuiKey_E, ImGuiKey_Q)));
        const float speed = kFlySpeed * (io.KeyShift ? kFastMultiplier : 1.0f) * io.DeltaTime;
        camera.move(bx::mul(move, speed));
        break;
    }
    case Drag::Orbit:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            camera.orbit(delta.x, delta.y);
        else
            drag_ = Drag::None;
        break;
    case Drag::Pan:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            camera.pan(delta.x, delta.y, viewportHeight);
        else
            drag_ = Drag::None;
        break;
    case Drag::None:
        break;
    }
}

void SceneViewport::manipulate(Document& doc, const float* view, const float* proj)
{
    kast::Scene& scene = doc.scene;
    const entt::entity entity = doc.selected;
    ImGuizmo::Enable(drag_ == Drag::None && !ImGui::GetIO().KeyAlt);
    float world[16];
    scene.worldMatrix(entity, world);
    if (!ImGuizmo::Manipulate(view, proj, static_cast<ImGuizmo::OPERATION>(operation_),
                              local_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, world))
        return;

    // The gizmo edits the world matrix; store it back as local: local = world * inverse(parentWorld).
    float local[16];
    const entt::entity parent = scene.parent(entity);
    if (parent == entt::null) {
        std::copy_n(world, 16, local);
    } else {
        float parentWorld[16], parentInverse[16];
        scene.worldMatrix(parent, parentWorld);
        bx::mtxInverse(parentInverse, parentWorld);
        bx::mtxMul(local, world, parentInverse);
    }
    kast::Transform& transform = scene.registry().get<kast::Transform>(entity);
    kast::decomposeTrs(local, transform.position, transform.rotation, transform.scale);
    markDirty(doc);
}

void SceneViewport::pick(Document& doc, const float* viewProj, float x, float y, float width, float height)
{
    const kast::Ray ray = kast::screenRay(viewProj, x, y, width, height, bgfx::getCaps()->homogeneousDepth);
    entt::entity nearest = entt::null;
    float nearestDistance = std::numeric_limits<float>::max();
    const auto consider = [&](entt::entity entity, float distance) {
        if (distance >= 0.0f && distance < nearestDistance) {
            nearestDistance = distance;
            nearest = entity;
        }
    };
    forEachMeshHit(doc.scene, ray, consider);
    for (entt::entity entity : doc.scene.registry().view<kast::Camera>()) {
        float world[16];
        doc.scene.worldMatrix(entity, world);
        const bx::Vec3 center{world[12], world[13], world[14]};
        const bx::Vec3 half{kCameraPickHalfSize, kCameraPickHalfSize, kCameraPickHalfSize};
        consider(entity, kast::intersect(ray, {bx::sub(center, half), bx::add(center, half)}));
    }
    doc.selected = nearest;
}

void SceneViewport::shortcuts(Document& doc)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput || drag_ != Drag::None ||
        ImGuizmo::IsUsing())
        return;
    if (!io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            operation_ = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            operation_ = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            operation_ = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            local_ = !local_;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && doc.scene.valid(doc.selected))
            doc.camera.focus(worldBounds(doc.scene, doc.selected));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        deleteSelected(doc);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D))
        duplicateSelected(doc);
}
