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
    // Where the next RenderGeometry calls draw: view on frameBuffer, width x height pixels. Layer passes take the views
    // after it, up to view + gui::kViews - 1. scale: RmlUi's units are scale pixels each (a context laid out at
    // width / scale x height / scale, drawn scaled up). shareViews: a second context drawn over the last begin()'s in
    // the same frame, taking the views after the ones it used (view is ignored), within the same range. clear (with
    // shareViews): that view clears frameBuffer to transparent first. redirectBase: the base layer draws into a pool
    // layer with the shared stencil, put over frameBuffer by end(), so clip masks work on it too (see EnableClipMask).
    void begin(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, int width, int height, float scale = 1.0f,
               bool shareViews = false, bool clear = false, bool redirectBase = false);
    // After the context's Render(): puts a redirected base layer on begin()'s target. True if the document asked for a
    // clip mask on its base layer (drawn with plain scissoring unless this render redirected it).
    bool end();

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
    // mask-image: the top layer holds the mask decorators; the CompositeLayers that follows multiplies the element by
    // its alpha (a filter pass). The layer itself is the mask, kept from spare() until then.
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;
    // Clip masks draw into the stencil the layers share: box-shadow keeps its shadow off the element with one, and
    // overflow clips transformed or rounded boxes to their shape. The base layer (the backbuffer or a host target,
    // whose stencil isn't ours) has none unless begin() redirects it; a clip mask asked for there falls back to plain
    // scissoring and makes end() return true, so the host redirects the next renders (GuiContext::render).
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;

    // See gui::wantsReadableTarget / gui::present.
    bool wantsReadableTarget() const { return wantsReadable_; }
    void present(bgfx::ViewId view, bgfx::TextureHandle texture, int width, int height);

private:
    static constexpr int kMaxGradientStops = 16; // fs_gui_gradient.sc's MAX_STOPS
    static constexpr Rml::LayerHandle kNoLayer = ~Rml::LayerHandle(0); // viewLayer_ when no view is left to draw in
    static constexpr Rml::LayerHandle kHost = kNoLayer - 1; // begin()'s target itself, while layer 0 is redirected

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
    Rml::Rectanglei scissorPixels() const; // the scissor region in target pixels
    void applyScissor();
    // The scissor region (the whole GUI without one) at scale, rounded outwards.
    void applyScissor(float scale);
    bgfx::FrameBufferHandle createLayer(); // a GUI-sized RGBA8 target on the shared stencil
    void ensureLayer(Rml::LayerHandle layer);
    // Draws the redirected base layer over begin()'s target; then, if more draws follow, clears it for them.
    bool flushBase(bool more);
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
    bgfx::FrameBufferHandle base_ = BGFX_INVALID_HANDLE; // begin()'s target: layer 0 unless redirected
    bool redirected_ = false;
    bool baseMaskAsked_ = false; // since begin(), RenderToClipMask on the base layer
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    float scale_ = 1.0f; // target pixels per RmlUi unit (begin)
    float proj_[16] = {};
    // One context size's targets. The renderer is shared (e.g. a screen GUI and the world UI atlas alternate in a
    // frame), so a size's pool is parked while another size renders instead of being destroyed and recreated.
    struct LayerPool {
        std::vector<bgfx::FrameBufferHandle> layers; // layers[i] backs layer handle i + 1
        bgfx::FrameBufferHandle redirect = BGFX_INVALID_HANDLE; // layer 0 while redirected_ (created on first use)
        bgfx::TextureHandle stencil = BGFX_INVALID_HANDLE;      // shared by the layers
        uint64_t used = 0;                                      // begins_ when last begun
    };
    static void destroyPool(LayerPool& pool);
    LayerPool pool_; // begin()'s size
    std::vector<std::pair<uint32_t, LayerPool>> parked_; // other sizes' pools, by width << 16 | height
    uint64_t begins_ = 0;
    Rml::LayerHandle top_ = 0;       // where RmlUi draws now
    Rml::LayerHandle viewLayer_ = 0; // what view_ draws into
    Rml::LayerHandle maskLayer_ = 0; // a saved mask-image (SaveLayerAsMaskImage) until the next PushLayer; 0 if none
    bool viewsWarned_ = false;
    bool wantsReadable_ = false;
    bool clipMask_ = false;
    bool maskInLayer_ = false; // the last RenderToClipMask drew into the stencil (a layer or the redirected base)
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
