#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <bgfx/bgfx.h>

#include <initializer_list>
#include <optional>
#include <vector>

// RmlUi render interface on bgfx: geometry as static vertex/index buffers, textures RmlUi generates (font glyphs) and
// PNG/JPG images it loads (<img src>, decorator: image(...)),
// scissoring and CSS transforms. Premultiplied alpha blending, drawn in submit order (sequential view).
// Layers (filters, box-shadow) draw into pooled offscreen RGBA8 targets the size of the GUI, created on the first
// PushLayer and kept across frames; every switch of target moves on to the next of gui::kViews views (see gui.h).
// Filters (gui_effects.cpp) run as passes over spare pool layers; gradients are a shader on the element's geometry.
namespace kast {

struct GuiFilter;

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
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;
    // Clip masks only inside layers, which share one stencil buffer: what box-shadow needs to keep its shadow off the
    // element. ponytail: on the base layer (the backbuffer or a host target, whose stencil isn't ours) they stay
    // no-ops, so overflow clipping of transformed or rounded boxes there falls back to plain scissoring; draw the base
    // through a layer with stencil if a GUI needs them.
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

    // See gui::wantsReadableTarget / gui::present.
    bool wantsReadableTarget() const { return wantsReadable_; }
    void present(bgfx::ViewId view, bgfx::TextureHandle texture, int width, int height);

private:
    static constexpr int kMaxGradientStops = 16; // fs_gui_gradient.sc's MAX_STOPS

    // A layer's picture held at scale (1, 1/2, 1/4, ...) of the GUI's size, from the target's top-left corner.
    struct Image {
        Rml::LayerHandle layer;
        float scale;
    };

    // Draws to layer from here on: true at once if view_ already does, else moves to the next view (cleared to
    // transparent if clear). False when the views are used up (logged once): the draw is dropped.
    bool target(Rml::LayerHandle layer);
    bool nextView(Rml::LayerHandle layer, bool clear);
    void setView(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer);
    bgfx::FrameBufferHandle frameBuffer(Rml::LayerHandle layer) const;
    void applyScissor();
    // The scissor region (the whole GUI without one) at scale, rounded outwards.
    void applyScissor(float scale);
    void destroyLayers();
    void ensureLayer(Rml::LayerHandle layer);
    void submit(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, bgfx::TextureHandle texture,
                bgfx::ProgramHandle program, uint64_t state, uint32_t stencil);
    // Draws image with program over the whole current target at toScale, moved by offset (GUI pixels). Set the
    // uniforms, state and scissor first.
    void drawImage(Image image, float toScale, bgfx::ProgramHandle program, Rml::Vector2f offset = {},
                   uint32_t stencil = BGFX_STENCIL_NONE);
    // The stencil test for draws into the current view: the clip mask's, if one is on and drawn in a layer.
    uint32_t clipTest() const;
    // A free pool layer above the top one that is none of busy (created if needed) for filter passes.
    Rml::LayerHandle spare(std::initializer_list<Rml::LayerHandle> busy);
    // Each runs filter passes on image and leaves image on the result. False when out of views.
    bool applyFilter(const GuiFilter& filter, Image& image);
    bool blur(float sigma, Image& image, Rml::LayerHandle keep);
    bool colorMatrix(const float* matrix, Image& image, Rml::LayerHandle keep, Rml::Vector2f offset = {});

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
    bool wantsReadable_ = false;
    bgfx::TextureHandle stencil_ = BGFX_INVALID_HANDLE; // shared by the layers
    bool clipMask_ = false;
    bool maskInLayer_ = false; // the last RenderToClipMask drew into a layer's stencil
    uint8_t clipRef_ = 1;      // stencil value inside the mask
    bgfx::VertexLayout layout_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle matrixProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle blurProgram_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle gradientProgram_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle matrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle blur_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle weights_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle gradient_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle shape_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle stopColor_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle stopPos_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_ = BGFX_INVALID_HANDLE; // bound for untextured geometry
    bool scissor_ = false;
    Rml::Rectanglei scissorRegion_;
    std::optional<Rml::Matrix4f> transform_;
};

} // namespace kast
