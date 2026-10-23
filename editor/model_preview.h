#pragma once

#include "camera.h"
#include "viewport.h"

#include <model.h>
#include <post_process.h>
#include <renderer.h>
#include <scene.h>

#include <bgfx/bgfx.h>

#include <memory>
#include <string>

// The Model Preview panel: one model file (double-clicked in Assets) rendered with its own materials under a
// directional light, ambient light and a procedural sky, auto-framed. Dragging the image orbits the camera, the wheel
// dollies it, "Reset view" frames it again. Its clips play looping at speed 1 (a combo picks one, a button pauses), and
// a readout gives its mesh stats (kast::modelStats). The file changing on disk shows the new version (ModelLibrary
// reloads it; the clip is found again by name), deleting it clears the panel ("The file doesn't exist."), an Assets
// rename or move follows it (remap). Rendered on its own views and target, like the Shader Preview: the scene and post
// views first .. first + 3, the output target on first + 4.
class ModelPreview {
public:
    static constexpr const char* kWindow = "Model Preview";
    static constexpr bgfx::ViewId kViews = 5;

    explicit ModelPreview(bgfx::ViewId first) : post_(first), target_(first + 4, "Model Preview") {}
    void shutdown();

    // Shows path (project-relative; replaces the one shown), framed, playing its default clip, and brings the panel to
    // the front.
    void open(const std::string& path);
    // Brings the panel to the front again (main: the first frames' docking undoes open's).
    void focus() { focus_ = true; }
    // Plays the clip of that name instead of the default once the model loads (falls back to the default, warning).
    void selectClip(const std::string& name) { clip_ = name; }
    // An Assets rename or move (project-relative): the shown file follows.
    void remap(const std::string& from, const std::string& to);
    bool isOpen() const { return open_; }
    // The panel, while open; dt: seconds the clip advances.
    void draw(float dt);

private:
    void init();
    // A newly shown model, or a reload of it: re-resolves the clip by name (else the default), and frames it if new.
    void show(const std::shared_ptr<const kast::Model>& model);
    void drawImage(const kast::Model& model, float dt);

    kast::PostProcess post_;
    ViewTarget target_;
    kast::Renderer renderer_;
    kast::Scene scene_;
    entt::entity object_ = entt::null;
    bool initialized_ = false;
    EditorCamera camera_;

    std::string path_;                    // project-relative
    std::shared_ptr<const kast::Model> shown_; // what the AnimatorState was set up for (held: a reload is a new pointer)
    std::string clip_;                    // the clip's name ("" = the default)
    bool playing_ = true;
    bool open_ = false;
    bool focus_ = false;
};
