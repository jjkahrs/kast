#include "shader_preview.h"

#include <material.h>
#include <shader.h>

#include <imgui.h>

#include <algorithm>

void ShaderPreview::init()
{
    renderer_.init();
    post_.init();
    scene_.sky.mode = kast::SkyMode::Procedural; // something behind the mesh for SCREEN_TEXTURE to show
    scene_.lighting = {.ambientColor = {0.7f, 0.8f, 1.0f}, .ambientIntensity = 0.3f};
    const entt::entity sun = scene_.create("Sun");
    // As shader_graph_demo.scene's sun: from the upper left, behind the camera.
    scene_.registry().get<kast::Transform>(sun).rotation = {-0.408218f, -0.23457f, -0.109382f, 0.875426f};
    scene_.registry().emplace<kast::DirectionalLight>(sun, kast::DirectionalLight{.color = {1.0f, 0.95f, 0.9f},
                                                                                   .intensity = 1.1f,
                                                                                   .castShadows = false});
    object_ = scene_.create("Preview");
    scene_.registry().emplace<kast::MeshRenderer>(object_);
    initialized_ = true;
}

void ShaderPreview::shutdown()
{
    if (!initialized_)
        return;
    target_.destroy();
    post_.shutdown();
    renderer_.shutdown();
    initialized_ = false;
}

void ShaderPreview::draw(const std::string& shader, float time)
{
    if (!initialized_)
        init();
    // The material: built-in values, the shader's param defaults; blended when the shader samples SCREEN / DEPTH
    // (checkShaderAlphaMode), so it renders instead of failing.
    kast::Material material;
    material.shader = shader;
    if (const auto declaration = kast::shaders().status(shader).declaration; declaration) {
        std::string error;
        if (!kast::checkShaderAlphaMode(material, *declaration, error))
            material.alphaMode = kast::AlphaMode::Blend;
    }
    scene_.registry().get<kast::MeshRenderer>(object_).primitive = mesh;
    scene_.registry().emplace_or_replace<kast::MaterialInstance>(object_, material);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const auto pixels = [](float v) { return static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f)); };
    const uint16_t width = pixels(size.x * scale.x), height = pixels(size.y * scale.y);
    post_.begin(width, height, kast::sceneClearColor(scene_.postProcessing, kViewportClearColor));
    target_.resize(width, height, post_.depth());
    if (!target_.valid() || size.x <= 0.0f || size.y <= 0.0f)
        return;
    ImGui::Image(static_cast<ImTextureID>(target_.texture().idx), size);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            camera_.orbit(io.MouseDelta.x, io.MouseDelta.y);
        if (io.MouseWheel != 0.0f)
            camera_.dolly(io.MouseWheel);
    }
    float view[16], proj[16];
    camera_.viewMatrix(view);
    camera_.projMatrix(proj, size.x / size.y);
    renderer_.setTime(time);
    renderer_.setTarget(post_.target(), post_.width(), post_.height(), post_.blendView());
    // No shadows (the light casts none), so no shadow views: the scene's view stands in for them.
    renderer_.render(scene_, kast::shaders().root(), post_.sceneView(), view, proj, post_.sceneView());
    post_.apply(scene_.postProcessing, false, target_.frameBuffer());
}
