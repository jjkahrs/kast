#pragma once

#include "camera.h"
#include "viewport.h"

#include <post_process.h>
#include <renderer.h>
#include <scene.h>

#include <bgfx/bgfx.h>

#include <string>

// The Shader Preview's image: a mesh (sphere, cube or plane) whose material runs one shader as ShaderLibrary has it (a
// graph's unsaved edits included, setOverride), under a directional light, ambient light and a procedural sky, with TIME
// running. Dragging the image orbits the camera, the wheel dollies it. Rendered on its own views and target, like the
// GUI Preview: the scene and post views first .. first + 3 (post runs with effects off, so only those), the output
// target on first + 4.
class ShaderPreview {
public:
    static constexpr bgfx::ViewId kViews = 5;

    explicit ShaderPreview(bgfx::ViewId first) : post_(first), target_(first + 4, "Shader Preview") {}
    void shutdown();
    // The mesh, by kast::Primitive.
    kast::Primitive mesh = kast::Primitive::Sphere;
    // Draws the image into the rest of the current window. shader: project-relative (.kshadergraph or .kshader).
    void draw(const std::string& shader, float time);

private:
    void init();

    kast::PostProcess post_;
    ViewTarget target_;
    kast::Renderer renderer_;
    kast::Scene scene_;
    entt::entity object_ = entt::null;
    bool initialized_ = false;
    EditorCamera camera_{.yaw = 30.0f, .pitch = -20.0f, .distance = 2.2f};
};
