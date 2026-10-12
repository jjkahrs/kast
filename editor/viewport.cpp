#include "viewport.h"

#include <log.h>

#include <imgui.h>
#include <ImGuizmo.h> // after imgui.h, which it needs

#include <algorithm>
#include <format>
#include <limits>

namespace {

constexpr uint32_t kClearColor = 0x2b3440ff;
constexpr float kFlySpeed = 5.0f; // meters per second
constexpr float kFastMultiplier = 4.0f;

kast::Aabb worldBounds(const kast::Scene& scene, entt::entity entity)
{
    float world[16];
    scene.worldMatrix(entity, world);
    const bx::Vec3 origin{world[12], world[13], world[14]};
    const auto* mesh = scene.registry().try_get<kast::MeshRenderer>(entity);
    if (!mesh)
        return {origin, origin};
    const kast::Aabb local = kast::primitiveBounds(mesh->primitive);
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

} // namespace

void SceneViewport::init()
{
    renderer_.init();
    operation_ = ImGuizmo::TRANSLATE;
}

void SceneViewport::shutdown()
{
    if (bgfx::isValid(frameBuffer_))
        bgfx::destroy(frameBuffer_);
    frameBuffer_ = BGFX_INVALID_HANDLE;
    renderer_.shutdown();
}

void SceneViewport::resize(uint16_t width, uint16_t height)
{
    if ((width == width_ && height == height_) || width == 0 || height == 0)
        return;
    if (bgfx::isValid(frameBuffer_))
        bgfx::destroy(frameBuffer_);
    const bgfx::TextureHandle textures[] = {
        bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8,
                              BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP),
        bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY),
    };
    frameBuffer_ = bgfx::createFrameBuffer(2, textures, true);
    width_ = width;
    height_ = height;
    bgfx::setViewName(view_, "Scene");
    bgfx::setViewFrameBuffer(view_, frameBuffer_);
    bgfx::setViewRect(view_, 0, 0, width, height);
    bgfx::setViewClear(view_, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, kClearColor, 1.0f, 0);
    kast::log::info(std::format("Scene framebuffer resized to {}x{}", width, height));
}

void SceneViewport::draw(Document& doc)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (visible) {
        drawToolbar();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        resize(static_cast<uint16_t>(size.x * scale.x), static_cast<uint16_t>(size.y * scale.y));
        if (bgfx::isValid(frameBuffer_) && size.x > 0.0f && size.y > 0.0f) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(bgfx::getTexture(frameBuffer_).idx), size);
            const bool hovered = ImGui::IsItemHovered();

            updateCamera(doc, hovered, size.y);
            float view[16], proj[16], viewProj[16];
            doc.camera.viewMatrix(view);
            doc.camera.projMatrix(proj, size.x / size.y);
            bx::mtxMul(viewProj, view, proj);
            renderer_.render(doc.scene, view_, view, proj);

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

void SceneViewport::drawToolbar()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 start = ImGui::GetCursorPos(); // below the tab bar
    ImGui::SetCursorPos(ImVec2(start.x + style.FramePadding.x * 2.0f, start.y + style.FramePadding.y));
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
    doc.dirty = true;
}

void SceneViewport::pick(Document& doc, const float* viewProj, float x, float y, float width, float height)
{
    const kast::Ray ray = kast::screenRay(viewProj, x, y, width, height, bgfx::getCaps()->homogeneousDepth);
    entt::entity nearest = entt::null;
    float nearestDistance = std::numeric_limits<float>::max();
    for (auto [entity, mesh] : doc.scene.registry().view<kast::MeshRenderer>().each()) {
        // Test in the mesh's local space: exact for rotated/scaled boxes. The direction is left unnormalized
        // so the hit parameter is the same distance along the world ray.
        float world[16], inverse[16];
        doc.scene.worldMatrix(entity, world);
        bx::mtxInverse(inverse, world);
        const kast::Ray local{bx::mul(ray.origin, inverse), kast::transformDir(ray.direction, inverse)};
        const float distance = kast::intersect(local, kast::primitiveBounds(mesh.primitive));
        if (distance >= 0.0f && distance < nearestDistance) {
            nearestDistance = distance;
            nearest = entity;
        }
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
