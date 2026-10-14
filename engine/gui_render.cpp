#include "gui_render.h"

#include "fs_gui.bin.h"
#include "log.h"
#include "vs_gui.bin.h"

#include <RmlUi/Core/Vertex.h>

#include <bx/math.h>

#include <algorithm>
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
    view_ = view;
    scissor_ = false;
    transform_.reset();
    float proj[16];
    bx::mtxOrtho(proj, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f, 1000.0f, 0.0f,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewFrameBuffer(view, frameBuffer);
    bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    bgfx::setViewTransform(view, nullptr, proj);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::touch(view);
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
    if (!geometry)
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
    if (scissor_) { // clamped at the top-left corner: a region off-screen there loses the off-screen part
        const int x0 = std::max(scissorRegion_.Left(), 0), y0 = std::max(scissorRegion_.Top(), 0);
        const int x1 = std::max(scissorRegion_.Right(), x0), y1 = std::max(scissorRegion_.Bottom(), y0);
        bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0),
                         static_cast<uint16_t>(y1 - y0));
    }
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
    bgfx::submit(view_, program_);
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

Rml::TextureHandle GuiRenderer::LoadTexture(Rml::Vector2i&, const Rml::String& source)
{
    // ponytail: no image loading; <img> and image decorators are out of scope. Decode with bimg here when needed.
    log::warn(std::format("GUI images are not supported yet: '{}'", source));
    return 0;
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

} // namespace kast
