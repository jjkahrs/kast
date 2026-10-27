#include "gui_render.h"

#include "fs_gui.bin.h"
#include "fs_gui_blur.bin.h"
#include "fs_gui_gradient.bin.h"
#include "fs_gui_matrix.bin.h"
#include "files.h"
#include "gui.h"
#include "log.h"
#include "renderer.h"
#include "vs_gui.bin.h"

#include <RmlUi/Core/Vertex.h>

#include <bx/math.h>

#include <algorithm>
#include <cmath>
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
constexpr uint32_t kLinearClamp = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP; // for resampling a layer
constexpr uint64_t kReplaceState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
constexpr uint64_t kBlendState =
    kReplaceState | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);

bgfx::ProgramHandle guiProgram(const uint8_t* fs, uint32_t fsSize)
{
    return bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_gui, sizeof(vs_gui))),
                               bgfx::createShader(bgfx::makeRef(fs, fsSize)), true);
}

} // namespace

void GuiRenderer::init()
{
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end(); // Rml::Vertex order
    program_ = guiProgram(fs_gui, sizeof(fs_gui));
    matrixProgram_ = guiProgram(fs_gui_matrix, sizeof(fs_gui_matrix));
    blurProgram_ = guiProgram(fs_gui_blur, sizeof(fs_gui_blur));
    gradientProgram_ = guiProgram(fs_gui_gradient, sizeof(fs_gui_gradient));
    sampler_ = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    matrix_ = bgfx::createUniform("u_matrix", bgfx::UniformType::Mat4);
    blur_ = bgfx::createUniform("u_blur", bgfx::UniformType::Vec4);
    weights_ = bgfx::createUniform("u_weights", bgfx::UniformType::Vec4, 2);
    gradient_ = bgfx::createUniform("u_gradient", bgfx::UniformType::Vec4);
    shape_ = bgfx::createUniform("u_shape", bgfx::UniformType::Vec4);
    stopColor_ = bgfx::createUniform("u_stopColor", bgfx::UniformType::Vec4, kMaxGradientStops);
    stopPos_ = bgfx::createUniform("u_stopPos", bgfx::UniformType::Vec4, kMaxGradientStops);
    const uint32_t white = 0xffffffff;
    white_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&white, sizeof(white)));
}

void GuiRenderer::shutdown()
{
    destroyLayers();
    for (bgfx::ProgramHandle* program : {&program_, &matrixProgram_, &blurProgram_, &gradientProgram_}) {
        if (bgfx::isValid(*program))
            bgfx::destroy(*program);
        *program = BGFX_INVALID_HANDLE;
    }
    for (bgfx::UniformHandle* uniform :
         {&sampler_, &matrix_, &blur_, &weights_, &gradient_, &shape_, &stopColor_, &stopPos_}) {
        if (bgfx::isValid(*uniform))
            bgfx::destroy(*uniform);
        *uniform = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(white_))
        bgfx::destroy(white_);
    white_ = BGFX_INVALID_HANDLE;
}

void GuiRenderer::begin(bgfx::ViewId view, bgfx::FrameBufferHandle frameBuffer, int width, int height, float scale,
                        bool shareViews, bool clear, bool redirectBase)
{
    if (width != width_ || height != height_)
        destroyLayers(); // the pool is the GUI's size; recreated as layers are pushed
    base_ = frameBuffer;
    width_ = static_cast<uint16_t>(width);
    height_ = static_cast<uint16_t>(height);
    scale_ = scale > 0.0f ? scale : 1.0f;
    top_ = 0;
    scissor_ = false;
    transform_.reset();
    baseMaskAsked_ = false;
    redirected_ = redirectBase;
    if (redirected_ && !bgfx::isValid(redirect_)) {
        redirect_ = createLayer();
        log::info(std::format("GUI base layer redirected for clip masks ({}x{})", width_, height_));
    }
    bx::mtxOrtho(proj_, 0.0f, static_cast<float>(width) / scale_, static_cast<float>(height) / scale_, 0.0f, 0.0f,
                 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    // Redirected: frameBuffer still gets its view (and clear) first, then the redirect is cleared to transparent.
    const Rml::LayerHandle first = redirected_ ? kHost : 0;
    if (shareViews) {
        if (!nextView(first, clear) || (redirected_ && !nextView(0, true)))
            viewLayer_ = kNoLayer; // out of views: every draw tries for one and is dropped
        return;
    }
    firstView_ = view_ = view;
    viewLayer_ = first;
    setView(view, frameBuffer);
    if (redirected_ && !nextView(0, true))
        viewLayer_ = kNoLayer;
}

bool GuiRenderer::end()
{
    if (redirected_)
        flushBase(false);
    redirected_ = false;
    return baseMaskAsked_;
}

bool GuiRenderer::flushBase(bool more)
{
    if (!nextView(kHost, false))
        return false;
    bgfx::setState(kBlendState);
    drawImage({0, 1.0f}, 1.0f, program_);
    return !more || nextView(0, true);
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
    bgfx::setViewName(view_, layer == 0 || layer == kHost ? "GUI" : "GUI layer");
    bgfx::setViewClear(view_, clear ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE, 0); // views keep their clear across frames
    return true;
}

bgfx::FrameBufferHandle GuiRenderer::frameBuffer(Rml::LayerHandle layer) const
{
    if (layer == 0)
        return redirected_ ? redirect_ : base_;
    return layer == kHost ? base_ : layers_[layer - 1];
}

void GuiRenderer::destroyLayers()
{
    const auto destroy = [](bgfx::FrameBufferHandle layer) {
        bgfx::destroy(bgfx::getTexture(layer)); // its color; the stencil is shared
        bgfx::destroy(layer);
    };
    std::ranges::for_each(layers_, destroy);
    layers_.clear();
    if (bgfx::isValid(redirect_))
        destroy(redirect_);
    redirect_ = BGFX_INVALID_HANDLE;
    if (bgfx::isValid(stencil_))
        bgfx::destroy(stencil_);
    stencil_ = BGFX_INVALID_HANDLE;
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

void GuiRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    submit(geometry, translation, texture ? toBgfx(texture) : white_, program_, kBlendState, clipTest());
}

void GuiRenderer::submit(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, bgfx::TextureHandle texture,
                         bgfx::ProgramHandle program, uint64_t state, uint32_t stencil)
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
    bgfx::setTexture(0, sampler_, texture);
    applyScissor();
    bgfx::setState(state);
    bgfx::setStencil(stencil);
    bgfx::submit(view_, program);
}

Rml::Rectanglei GuiRenderer::scissorPixels() const
{
    if (scale_ == 1.0f)
        return scissorRegion_;
    const Rml::Vector2f min = Rml::Vector2f(scissorRegion_.TopLeft()) * scale_;
    const Rml::Vector2f max = Rml::Vector2f(scissorRegion_.BottomRight()) * scale_;
    return Rml::Rectanglei::FromCorners({static_cast<int>(std::floor(min.x)), static_cast<int>(std::floor(min.y))},
                                        {static_cast<int>(std::ceil(max.x)), static_cast<int>(std::ceil(max.y))});
}

void GuiRenderer::applyScissor()
{
    if (scissor_) { // clamped at the top-left corner: a region off-screen there loses the off-screen part
        const Rml::Rectanglei region = scissorPixels();
        const int x0 = std::max(region.Left(), 0), y0 = std::max(region.Top(), 0);
        const int x1 = std::max(region.Right(), x0), y1 = std::max(region.Bottom(), y0);
        bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0),
                         static_cast<uint16_t>(y1 - y0));
    }
}

void GuiRenderer::applyScissor(float scale)
{
    const Rml::Rectanglei region = scissor_ ? scissorPixels() : Rml::Rectanglei::FromSize({width_, height_});
    const int x0 = std::clamp(static_cast<int>(std::floor(region.Left() * scale)), 0, int(width_));
    const int y0 = std::clamp(static_cast<int>(std::floor(region.Top() * scale)), 0, int(height_));
    const int x1 = std::clamp(static_cast<int>(std::ceil(region.Right() * scale)), x0, int(width_));
    const int y1 = std::clamp(static_cast<int>(std::ceil(region.Bottom() * scale)), y0, int(height_));
    bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1 - x0),
                     static_cast<uint16_t>(y1 - y0));
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

bgfx::FrameBufferHandle GuiRenderer::createLayer()
{
    if (!bgfx::isValid(stencil_))
        stencil_ =
            bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
    const bgfx::TextureHandle textures[] = {
        bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::RGBA8, kLayerFlags), stencil_};
    return bgfx::createFrameBuffer(2, textures, false);
}

void GuiRenderer::ensureLayer(Rml::LayerHandle layer)
{
    while (layers_.size() < static_cast<size_t>(layer)) {
        layers_.push_back(createLayer());
        log::info(std::format("GUI layer framebuffer {} created ({}x{})", layers_.size(), width_, height_));
    }
}

Rml::LayerHandle GuiRenderer::PushLayer()
{
    ensureLayer(++top_);
    nextView(top_, true); // RmlUi expects it transparent: the whole layer is cleared, not just the scissor region
    return top_;
}

void GuiRenderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blendMode,
                                  Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    // ponytail: source == destination (RmlUi 6.2 never asks for it) would read the target it draws into; copy via a
    // spare layer if needed.
    if (source == destination)
        return;
    if (source == 0 && !bgfx::isValid(base_)) {
        // backdrop-filter reads the base layer, but the backbuffer can't be sampled: drawn without the effect, and
        // the host gives later frames a readable target (gui::wantsReadableTarget).
        wantsReadable_ = true;
        return;
    }
    if (source == 0 && redirected_) { // what is under it: the target with the base layer drawn so far on it
        if (!flushBase(true))
            return;
        source = kHost;
    }
    Image image{source, 1.0f};
    for (Rml::CompiledFilterHandle filter : filters)
        if (!applyFilter(*reinterpret_cast<const GuiFilter*>(filter), image))
            return;
    if (!target(destination))
        return;
    applyScissor();
    bgfx::setState(blendMode == Rml::BlendMode::Replace ? kReplaceState : kBlendState);
    drawImage(image, 1.0f, program_, {}, clipTest());
}

void GuiRenderer::drawImage(Image image, float toScale, bgfx::ProgramHandle program, Rml::Vector2f offset,
                            uint32_t stencil)
{
    if (bgfx::getAvailTransientVertexBuffer(3, layout_) < 3)
        return;
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 3, layout_);
    // One triangle past the target's corners in GUI pixels, drawn at toScale; each pixel samples image's texel under
    // it (SetTransform is for geometry only).
    const float w = width_ / scale_, h = height_ / scale_; // in RmlUi units, as proj_
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const auto uv = [&](float x, float y) {
        const float v = (y - offset.y) * image.scale / h;
        return Rml::Vector2f((x - offset.x) * image.scale / w, bottomLeft ? 1.0f - v : v);
    };
    const Rml::ColourbPremultiplied white(255, 255, 255, 255);
    const Rml::Vertex triangle[3] = {{{0.0f, 0.0f}, white, uv(0.0f, 0.0f)},
                                     {{2.0f * w, 0.0f}, white, uv(2.0f * w, 0.0f)},
                                     {{0.0f, 2.0f * h}, white, uv(0.0f, 2.0f * h)}};
    bx::memCopy(vertices.data, triangle, sizeof(triangle));
    float model[16];
    bx::mtxScale(model, toScale, toScale, 1.0f);
    bgfx::setTransform(model);
    bgfx::setVertexBuffer(0, &vertices);
    // The layer's own point sampling for a 1:1 copy, bilinear when scaling.
    bgfx::setTexture(0, sampler_, bgfx::getTexture(frameBuffer(image.layer)),
                     image.scale == toScale ? UINT32_MAX : kLinearClamp);
    bgfx::setStencil(stencil);
    bgfx::submit(view_, program);
}

uint32_t GuiRenderer::clipTest() const
{
    const bool stencil = viewLayer_ == 0 ? redirected_ : viewLayer_ != kHost;
    if (!clipMask_ || !maskInLayer_ || !stencil)
        return BGFX_STENCIL_NONE;
    return BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(clipRef_) | BGFX_STENCIL_FUNC_RMASK(0xff) |
           BGFX_STENCIL_OP_FAIL_S_KEEP | BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_KEEP;
}

void GuiRenderer::EnableClipMask(bool enable) { clipMask_ = enable; }

void GuiRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                                   Rml::Vector2f translation)
{
    baseMaskAsked_ = baseMaskAsked_ || top_ == 0;
    maskInLayer_ = top_ != 0 || redirected_;
    if (!maskInLayer_ || !target(top_))
        return;
    // As RmlUi's GL3 backend: Set / SetInverse clear the stencil to outside / inside and draw the geometry as the
    // other; Intersect counts up where the geometry is, and only the highest count is inside.
    constexpr uint32_t kWrite = BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
                                BGFX_STENCIL_OP_FAIL_Z_KEEP;
    uint32_t write = kWrite | BGFX_STENCIL_OP_PASS_Z_INCR;
    if (operation == Rml::ClipMaskOperation::Intersect) {
        ++clipRef_;
    } else {
        const bool inverse = operation == Rml::ClipMaskOperation::SetInverse;
        clipRef_ = 1;
        bgfx::setState(0); // no color (the spare layer is just some texture not drawn into): the whole stencil
        drawImage({spare({}), 1.0f}, 1.0f, program_, {},
                  kWrite | BGFX_STENCIL_FUNC_REF(inverse ? 1 : 0) | BGFX_STENCIL_OP_PASS_Z_REPLACE);
        write = kWrite | BGFX_STENCIL_FUNC_REF(inverse ? 0 : 1) | BGFX_STENCIL_OP_PASS_Z_REPLACE;
    }
    submit(geometry, translation, white_, program_, 0, write);
}

void GuiRenderer::present(bgfx::ViewId view, bgfx::TextureHandle texture, int width, int height)
{
    if (bgfx::getAvailTransientVertexBuffer(3, layout_) < 3)
        return;
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 3, layout_);
    const bool bottomLeft = bgfx::getCaps()->originBottomLeft;
    const float top = bottomLeft ? 1.0f : 0.0f, bottom = bottomLeft ? -1.0f : 2.0f; // v at y = 0 and y = 2h
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const Rml::ColourbPremultiplied white(255, 255, 255, 255);
    const Rml::Vertex triangle[3] = {
        {{0.0f, 0.0f}, white, {0.0f, top}}, {{2.0f * w, 0.0f}, white, {2.0f, top}}, {{0.0f, 2.0f * h}, white, {0.0f, bottom}}};
    bx::memCopy(vertices.data, triangle, sizeof(triangle));
    float proj[16];
    bx::mtxOrtho(proj, 0.0f, w, h, 0.0f, 0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(view, "GUI present");
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    bgfx::setViewTransform(view, nullptr, proj);
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setTexture(0, sampler_, texture);
    bgfx::setState(kReplaceState);
    bgfx::submit(view, program_);
}

void GuiRenderer::PopLayer()
{
    if (top_ > 0)
        --top_;
}

Rml::TextureHandle GuiRenderer::SaveLayerAsTexture()
{
    // The top layer inside the scissor region (RmlUi sets one first). Not the base layer: it may be the backbuffer.
    const Rml::Rectanglei region = scissorPixels();
    const int x0 = std::clamp(region.Left(), 0, int(width_));
    const int y0 = std::clamp(region.Top(), 0, int(height_));
    const int x1 = std::clamp(region.Right(), x0, int(width_));
    const int y1 = std::clamp(region.Bottom(), y0, int(height_));
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
