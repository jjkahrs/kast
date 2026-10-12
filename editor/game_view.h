#pragma once

#include "document.h"
#include "viewport.h"

#include <renderer.h>

// The Game panel: renders the scene from its active Camera (kast::activeCamera) with no grid, gizmos or other
// editor overlays, live in edit mode too. Comes to the front when Play starts. While it is focused and playing,
// the host feeds SDL input to Document::input and the Scene panel's shortcuts don't apply (it isn't focused).
class GameViewport {
public:
    explicit GameViewport(bgfx::ViewId view) : target_(view, "Game") {}

    void init() { renderer_.init(); }
    void shutdown();
    void draw(Document& doc);
    bool hasInput(const Document& doc) const { return focused_ && doc.play == PlayState::Playing; }

private:
    ViewTarget target_;
    kast::Renderer renderer_;
    bool focused_ = false;
};
