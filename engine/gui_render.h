#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <bgfx/bgfx.h>

#include <optional>

// RmlUi render interface on bgfx: geometry as static vertex/index buffers, textures RmlUi generates (font glyphs) and
// PNG/JPG images it loads (<img src>, decorator: image(...)),
// scissoring and CSS transforms. Premultiplied alpha blending, drawn in submit order (sequential view).
namespace kast {

class GuiRenderer final : public Rml::RenderInterface {
public:
    void init();
    void shutdown();
    // Where the next RenderGeometry calls draw: view on frameBuffer, laid out as width x height pixels.
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
    // ponytail: no clip masks (RmlUi's default no-ops): overflow clipping of transformed or rounded boxes falls back
    // to plain scissoring. Implement EnableClipMask/RenderToClipMask with the stencil buffer if a GUI needs them.

private:
    bgfx::ViewId view_ = 0;
    bgfx::VertexLayout layout_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white_ = BGFX_INVALID_HANDLE; // bound for untextured geometry
    bool scissor_ = false;
    Rml::Rectanglei scissorRegion_;
    std::optional<Rml::Matrix4f> transform_;
};

} // namespace kast
