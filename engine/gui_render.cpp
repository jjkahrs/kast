#include "gui_render.h"

#include "fs_gui.bin.h"
#include "files.h"
#include "gui.h"
#include "log.h"
#include "renderer.h"
#include "vs_gui.bin.h"

#include <RmlUi/Core/Vertex.h>

#include <bx/math.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace kast {
namespace {

struct Geometry {
    bgfx::VertexBufferHandle vertices;
    bgfx::IndexBufferHandle indices;
};

// 0 is RmlUi's "no texture"; bgfx handle indices start at 0.
Rml::TextureHandle toRml(bgfx::TextureHandle texture) { return static_cast<Rml::TextureHandle>(texture.idx) + 1; }
bgfx::TextureHandle toBgfx(Rml::TextureHandle texture) { return {static_cast<uint16_t>(texture - 1)}; }

constexpr uint64_t kLayerFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT;
constexpr uint64_t kBlendState =
    BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);

} // namespace

void GuiRenderer::init()
{
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end(); // Rml::Vertex order
    program_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_gui, sizeof(vs_gui))),
                                   bgfx::createShader(bgfx::makeRef(fs_gui, sizeof(fs_gui))), true);
    sampler_ = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    const uint32_t white = 0xffffffff;
    white_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&white, sizeof(white)));
}

void GuiRenderer::shutdown()
{
    destroyLayers();
    if (bgfx::isValid(program_))
        bgfx::destroy(program_);
    if (bgfx::isValid(sampler_))
        bgfx::destroy(sampler_);
    if (bgfx::isValid(white_))
        bgfx::destroy(white_);
    program_ = BGFX_INVALID_HANDLE;
    sampler_ = BGFX_INVALID_HANDLE;
    white_ = BGFX_INVALID_HANDLE;
}

void GuiRenderer::begin(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, int width, int height)
{
    if (width != width_ || height != height_)
        destroyLayers(); // the pool is the GUI's size; recreated as layers are pushed
    firstView_ = view_ = view;
    base_ = frameBuffer;
    width_ = static_cast<uint16_t>(width);
    height_ = static_cast<uint16_t>(height);
    top_ = viewLayer_ = 0;
    scissor_ = false;
    transform_.reset();
    bx::mtxOrtho(proj_, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f, 1000.0f, 0.0f,
                 bgfx::getCaps()->homogeneousDepth);
    setView(view, frameBuffer);
}

void GuiRenderer::setView(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer)
{
    bgfx::setViewFrameBuffer(view, frameBuffer);
    bgfx::setViewRect(view, 0, 0, width_, height_);
    bgfx::setViewTransform(view, nullptr, proj_);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::touch(view);
}

bool GuiRenderer::target(Rml::LayerHandle layer) { return layer == viewLayer_ || nextView(layer, false); }

bool GuiRenderer::nextView(Rml::LayerHandle layer, bool clear)
{
    if (view_ + 1 >= firstView_ + gui::kViews) {
        // ponytail: a fixed view budget per frame; past it, draws that need another view are dropped. Raise
        // gui::kViews (the hosts reserve it) if documents stack that many effects.
        if (!viewsWarned_)
            log::warn(std::format("GUI effects need more than {} bgfx views in a frame; some are not drawn", gui::kViews));
        viewsWarned_ = true;
        return false;
    }
    ++view_;
    viewLayer_ = layer;
    setView(view_, frameBuffer(layer));
    bgfx::setViewName(view_, layer ? "GUI layer" : "GUI");
    bgfx::setViewClear(view_, clear ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE, 0); // views keep their clear across frames
    return true;
}

bgfx::FrameBufferHandle GuiRenderer::frameBuffer(Rml::LayerHandle layer) const
{
    return layer == 0 ? base_ : layers_[layer - 1];
}

void GuiRenderer::destroyLayers()
{
    for (bgfx::FrameBufferHandle layer : layers_)
        bgfx::destroy(layer);
    layers_.clear();
}

Rml::CompiledGeometryHandle GuiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                         Rml::Span<const int> indices)
{
    if (vertices.empty() || indices.empty()) // bgfx can't make an empty buffer; 0 is RmlUi's "no geometry"
        return 0;
    auto* geometry = new Geometry{
        bgfx::createVertexBuffer(bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(Rml::Vertex))),
                                 layout_),
        bgfx::createIndexBuffer(bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(int))),
                                BGFX_BUFFER_INDEX32)};
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void GuiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    const auto* geometry = reinterpret_cast<const Geometry*>(handle);
    if (!geometry || !target(top_))
        return;
    float model[16];
    bx::mtxTranslate(model, translation.x, translation.y, 0.0f);
    if (transform_) { // the CSS transform applies after the translation
        float translated[16];
        bx::memCopy(translated, model, sizeof(model));
        bx::mtxMul(model, translated, transform_->data());
    }
    bgfx::setTransform(model);
    bgfx::setVertexBuffer(0, geometry->vertices);
    bgfx::setIndexBuffer(geometry->indices);
    bgfx::setTexture(0, sampler_, texture ? toBgfx(texture) : white_);
    applyScissor();
    bgfx::setState(kBlendState);
    bgfx::submit(view_, program_);
}

void GuiRenderer::applyScissor()
{
    if (scissor_) { // clamped at the top-left corner: a region off-screen there loses the off-screen part
        const int x0 = std::max(scissorRegion_.Left(), 0), y0 = std::max(scissorRegion_.Top(), 0);
        const int x1 = std::max(scissorRegion_.Right(), x0), y1 = std::max(scissorRegion_.Bottom(), y0);
        bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0),
                         static_cast<uint16_t>(y1 - y0));
    }
}

void GuiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    auto* geometry = reinterpret_cast<Geometry*>(handle);
    if (!geometry)
        return;
    bgfx::destroy(geometry->vertices);
    bgfx::destroy(geometry->indices);
    delete geometry;
}

Rml::TextureHandle GuiRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
    // <img src> and decorator: image(...) paths, resolved by RmlUi against the document; read from disk or the pak.
    std::string bytes;
    if (!readFile(gui::fromRmlPath(source), bytes))
        return 0; // RmlUi logs "Could not load texture"
    std::optional<MipImage> image = decodeImage(bytes, false);
    if (!image) {
        log::warn(std::format("GUI image '{}' is not a PNG or JPG it can decode", source));
        return 0;
    }
    std::vector<uint8_t>& rgba = image->rgba;
    rgba.resize(size_t(image->width) * image->height * 4); // the first level; the GUI draws at 1:1
    for (size_t i = 0; i < rgba.size(); i += 4) // RmlUi blends premultiplied alpha
        for (size_t c = 0; c < 3; ++c)
            rgba[i + c] = static_cast<uint8_t>((rgba[i + c] * rgba[i + 3] + 127) / 255);
    dimensions = {image->width, image->height};
    return GenerateTexture({rgba.data(), rgba.size()}, dimensions);
}

Rml::TextureHandle GuiRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
{
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(dimensions.x), static_cast<uint16_t>(dimensions.y), false, 1,
                              bgfx::TextureFormat::RGBA8, 0, bgfx::copy(source.data(), static_cast<uint32_t>(source.size())));
    return bgfx::isValid(texture) ? toRml(texture) : 0;
}

void GuiRenderer::ReleaseTexture(Rml::TextureHandle texture)
{
    if (texture)
        bgfx::destroy(toBgfx(texture));
}

void GuiRenderer::EnableScissorRegion(bool enable) { scissor_ = enable; }

void GuiRenderer::SetScissorRegion(Rml::Rectanglei region) { scissorRegion_ = region; }

void GuiRenderer::SetTransform(const Rml::Matrix4f* transform)
{
    if (transform)
        transform_ = *transform;
    else
        transform_.reset();
}

Rml::LayerHandle GuiRenderer::PushLayer()
{
    if (static_cast<size_t>(top_) == layers_.size()) {
        layers_.push_back(bgfx::createFrameBuffer(width_, height_, bgfx::TextureFormat::RGBA8, kLayerFlags));
        log::info(std::format("GUI layer framebuffer {} created ({}x{})", layers_.size(), width_, height_));
    }
    ++top_;
    nextView(top_, true); // RmlUi expects it transparent: the whole layer is cleared, not just the scissor region
    return top_;
}

void GuiRenderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                                  Rml::Span<const Rml::CompiledFilterHandle> /*filters*/)
{
    // ponytail: filters are skipped (CompileFilter is still RmlUi's no-op), so this is a plain composite. source ==
    // destination (RmlUi 6.2 never asks for it) would read the target it draws into; copy via a spare layer if needed.
    if (source == destination || !target(destination))
        return;
    if (bgfx::getAvailTransientVertexBuffer(3, layout_) < 3)
        return;
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 3, layout_);
    // One triangle past the target's corners at 1:1 texels, untransformed (SetTransform is for geometry only).
    const float w = width_, h = height_;
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const float top = bottomLeft ? 1.0f : 0.0f, bottom = bottomLeft ? -1.0f : 2.0f; // v at y = 0 and y = 2h
    const Rml::ColourbPremultiplied white(255, 255, 255, 255);
    const Rml::Vertex triangle[3] = {
        {{0.0f, 0.0f}, white, {0.0f, top}}, {{2.0f * w, 0.0f}, white, {2.0f, top}}, {{0.0f, 2.0f * h}, white, {0.0f, bottom}}};
    bx::memCopy(vertices.data, triangle, sizeof(triangle));
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setTexture(0, sampler_, bgfx::getTexture(frameBuffer(source)));
    applyScissor();
    bgfx::setState(blendMode == Rml::BlendMode::Replace ? BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A : kBlendState);
    bgfx::submit(view_, program_);
}

void GuiRenderer::PopLayer()
{
    if (top_ > 0)
        --top_;
}

Rml::TextureHandle GuiRenderer::SaveLayerAsTexture()
{
    // The top layer inside the scissor region (RmlUi sets one first). Not the base layer: it may be the backbuffer.
    const int x0 = std::clamp(scissorRegion_.Left(), 0, int(width_));
    const int y0 = std::clamp(scissorRegion_.Top(), 0, int(height_));
    const int x1 = std::clamp(scissorRegion_.Right(), x0, int(width_));
    const int y1 = std::clamp(scissorRegion_.Bottom(), y0, int(height_));
    // A view of its own: bgfx blits at the start of a view, so the copy comes after the layer's draws so far.
    if (top_ == 0 || x1 == x0 || y1 == y0 || !nextView(top_, false))
        return 0;
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0), false, 1,
                              bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(texture))
        return 0;
    bgfx::blit(view_, texture, 0, 0, bgfx::getTexture(frameBuffer(top_)), static_cast<uint16_t>(x0),
               static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0));
    return toRml(texture);
}

} // namespace kast
