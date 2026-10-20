#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <bgfx/bgfx.h>

#include <optional>
#include <vector>

// RmlUi render interface on bgfx: geometry as static vertex/index buffers, textures RmlUi generates (font glyphs) and
// PNG/JPG images it loads (<img src>, decorator: image(...)),
// scissoring and CSS transforms. Premultiplied alpha blending, drawn in submit order (sequential view).
// Layers (filters, box-shadow) draw into pooled offscreen RGBA8 targets the size of the GUI, created on the first
// PushLayer and kept across frames; every switch of target moves on to the next of gui::kViews views (see gui.h).
namespace kast {

class GuiRenderer final : public Rml::RenderInterface {
public:
    void init();
    void shutdown();
    // Where the next RenderGeometry calls draw: view on frameBuffer, laid out as width x height pixels. Layer passes
    // take the views after it, up to view + gui::kViews - 1.
    void begin(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, int width, int height);

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;
    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    // ponytail: no clip masks (RmlUi's default no-ops): overflow clipping of transformed or rounded boxes falls back
    // to plain scissoring. Implement EnableClipMask/RenderToClipMask with the stencil buffer if a GUI needs them.

private:
    // Draws to layer from here on: true at once if view_ already does, else moves to the next view (cleared to
    // transparent if clear). False when the views are used up (logged once): the draw is dropped.
    bool target(Rml::LayerHandle layer);
    bool nextView(Rml::LayerHandle layer, bool clear);
    void setView(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer);
    bgfx::FrameBufferHandle frameBuffer(Rml::LayerHandle layer) const;
    void applyScissor();
    void destroyLayers();

    bgfx::ViewId firstView_ = 0;
    bgfx::ViewId view_ = 0;
    bgfx::FrameBufferHandle base_ = BGFX_INVALID_HANDLE; // layer 0, begin()'s target
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    float proj_[16] = {};
    std::vector<bgfx::FrameBufferHandle> layers_; // the pool: layers_[i] backs layer handle i + 1
    Rml::LayerHandle top_ = 0;       // where RmlUi draws now
    Rml::LayerHandle viewLayer_ = 0; // what view_ draws into
    bool viewsWarned_ = false;
    bgfx::VertexLayout layout_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_ = BGFX_INVALID_HANDLE; // bound for untextured geometry
    bool scissor_ = false;
    Rml::Rectanglei scissorRegion_;
    std::optional<Rml::Matrix4f> transform_;
};

} // namespace kast
