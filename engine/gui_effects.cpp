// GuiRenderer's filters (CompileFilter, the passes CompositeLayers runs) and gradient shaders (CompileShader,
// RenderShader). The filter math follows RmlUi 6's GL3 reference backend.
#include "gui_render.h"

#include "log.h"

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Math.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <set>
#include <string>

namespace kast {

struct GuiFilter {
    enum class Type { Blur, DropShadow, ColorMatrix, Mask } type;
    float sigma = 0.0f;           // Blur, DropShadow
    Rml::Vector2f offset;         // DropShadow
    Rml::Matrix4f matrix;         // ColorMatrix; DropShadow: its tint (alpha to the premultiplied color)
    Rml::LayerHandle mask = 0;    // Mask: the layer holding it, at full scale
};

namespace {

struct GuiGradient {
    float function = 0.0f; // fs_gui_gradient.sc's u_gradient.x: 0 linear, 1 radial, 2 conic
    bool repeating = false;
    Rml::Vector2f p, v;    // u_shape
    int count = 0;
    float colors[16][4] = {}; // GuiRenderer::kMaxGradientStops
    float positions[16][4] = {};
};

constexpr uint64_t kPassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
constexpr uint64_t kOverState =
    kPassState | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
constexpr uint64_t kMaskState = kPassState | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_SRC_ALPHA);
constexpr float kMaxPassSigma = 3.0f; // texels; fs_gui_blur.sc reaches 6 (2 sigma)

void warnOnce(const std::string& message)
{
    static std::set<std::string> warned;
    if (warned.insert(message).second)
        log::warn(message);
}

bool finite(float value) { return std::isfinite(value); }

// Gaussian weights for offsets 0..6, normalised over -6..6 (RmlUi's SetBlurWeights, wider).
void blurWeights(float sigma, float out[8])
{
    float sum = 0.0f;
    for (int i = 0; i < 7; ++i) {
        out[i] = sigma < 0.1f ? float(i == 0) : std::exp(-float(i * i) / (2.0f * sigma * sigma));
        sum += (i == 0 ? 1.0f : 2.0f) * out[i];
    }
    for (int i = 0; i < 7; ++i)
        out[i] /= sum;
    out[7] = 0.0f;
}

} // namespace

Rml::CompiledFilterHandle GuiRenderer::CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters)
{
    GuiFilter filter{GuiFilter::Type::ColorMatrix};
    const float value = Rml::Get(parameters, "value", 1.0f);
    const float rev = 1.0f - value;
    if (name == "opacity") {
        filter.matrix = Rml::Matrix4f::Diag(value, value, value, value); // premultiplied: alpha and color alike
    } else if (name == "brightness") {
        filter.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
    } else if (name == "contrast") {
        const float grey = 0.5f - 0.5f * value;
        filter.matrix = Rml::Matrix4f::Diag(value, value, value, 1.0f);
        filter.matrix.SetColumn(3, Rml::Vector4f(grey, grey, grey, 1.0f));
    } else if (name == "invert") {
        const float amount = std::clamp(value, 0.0f, 1.0f);
        const float inverted = 1.0f - 2.0f * amount;
        filter.matrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
        filter.matrix.SetColumn(3, Rml::Vector4f(amount, amount, amount, 1.0f));
    } else if (name == "grayscale") {
        const Rml::Vector3f g = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        filter.matrix = Rml::Matrix4f::FromRows({g.x + rev, g.y, g.z, 0.0f}, {g.x, g.y + rev, g.z, 0.0f},
                                                {g.x, g.y, g.z + rev, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    } else if (name == "sepia") {
        const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        filter.matrix = Rml::Matrix4f::FromRows({r.x + rev, r.y, r.z, 0.0f}, {g.x, g.y + rev, g.z, 0.0f},
                                                {b.x, b.y, b.z + rev, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    } else if (name == "hue-rotate") {
        // value in radians; https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        const float s = std::sin(value), c = std::cos(value);
        filter.matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.0f},
            {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.0f},
            {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f});
    } else if (name == "saturate") {
        filter.matrix = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    } else if (name == "blur") {
        filter.type = GuiFilter::Type::Blur;
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
    } else if (name == "drop-shadow") {
        filter.type = GuiFilter::Type::DropShadow;
        filter.sigma = Rml::Get(parameters, "sigma", 0.0f);
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
        const Rml::ColourbPremultiplied color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
        const Rml::Vector4f zero(0.0f);
        const Rml::Vector4f tint = Rml::Vector4f(color.red, color.green, color.blue, color.alpha) / 255.0f;
        filter.matrix = Rml::Matrix4f::FromColumns(zero, zero, zero, tint);
    } else {
        warnOnce(std::format("GUI filter '{}' is not supported: elements using it draw without it", name));
        return 0;
    }
    const bool usesValue = filter.type == GuiFilter::Type::ColorMatrix;
    if ((usesValue && !finite(value)) || !finite(filter.sigma) || filter.sigma < 0.0f || !finite(filter.offset.x) ||
        !finite(filter.offset.y)) {
        warnOnce(std::format("GUI filter '{}' has invalid parameters: elements using it draw without it", name));
        return 0;
    }
    return reinterpret_cast<Rml::CompiledFilterHandle>(new GuiFilter(filter));
}

Rml::CompiledFilterHandle GuiRenderer::SaveLayerAsMaskImage()
{
    // As RmlUi's GL3 backend (which copies the layer to a mask buffer): RmlUi pops this layer and composites the
    // element's with the mask as its last filter, drawing nothing in between, so the layer is kept as it is.
    if (top_ == 0 || !bgfx::isValid(layers_[top_ - 1])) {
        warnOnce("GUI mask-image has no layer to save its mask in: elements using it draw unmasked");
        return 0;
    }
    maskLayer_ = top_;
    GuiFilter filter{GuiFilter::Type::Mask};
    filter.mask = top_;
    return reinterpret_cast<Rml::CompiledFilterHandle>(new GuiFilter(filter));
}

void GuiRenderer::ReleaseFilter(Rml::CompiledFilterHandle filter) { delete reinterpret_cast<GuiFilter*>(filter); }

Rml::LayerHandle GuiRenderer::spare(std::initializer_list<Rml::LayerHandle> busy)
{
    // ponytail: three scratch layers above the top one cover every filter (drop-shadow needs its input, the shadow
    // and the result at once).
    Rml::LayerHandle layer = top_ + 1;
    while (layer == maskLayer_ || std::find(busy.begin(), busy.end(), layer) != busy.end())
        ++layer;
    ensureLayer(layer);
    return layer;
}

bool GuiRenderer::applyFilter(const GuiFilter& filter, Image& image)
{
    switch (filter.type) {
    case GuiFilter::Type::Blur:
        return blur(filter.sigma, image, image.layer);
    case GuiFilter::Type::ColorMatrix:
        return colorMatrix(filter.matrix.data(), image, image.layer);
    case GuiFilter::Type::DropShadow: {
        Image shadow = image;
        if (!colorMatrix(filter.matrix.data(), shadow, image.layer, filter.offset) ||
            (filter.sigma >= 0.5f && !blur(filter.sigma, shadow, image.layer)))
            return false;
        const Rml::LayerHandle out = spare({image.layer, shadow.layer});
        if (!nextView(out, true))
            return false;
        for (const Image& layer : {shadow, image}) { // the element over its shadow
            applyScissor(image.scale);
            bgfx::setState(kOverState);
            drawImage(layer, image.scale, program_);
        }
        image.layer = out;
        return true;
    }
    case GuiFilter::Type::Mask: { // a copy of the element times the mask's alpha, the mask resampled to its scale
        const Rml::LayerHandle out = spare({image.layer});
        if (!nextView(out, true))
            return false;
        applyScissor(image.scale);
        bgfx::setState(kPassState);
        drawImage(image, image.scale, program_);
        applyScissor(image.scale);
        bgfx::setState(kMaskState); // the copy's color and alpha times the mask's alpha
        drawImage({filter.mask, 1.0f}, image.scale, program_);
        image.layer = out;
        return true;
    }
    }
    return false;
}

bool GuiRenderer::colorMatrix(const float* matrix, Image& image, Rml::LayerHandle keep, Rml::Vector2f offset)
{
    const Rml::LayerHandle out = spare({image.layer, keep});
    if (!nextView(out, true))
        return false;
    bgfx::setUniform(matrix_, matrix); // column-major, as bgfx's mul(u_matrix, v) wants
    applyScissor(image.scale);
    bgfx::setState(kPassState);
    drawImage(image, image.scale, matrixProgram_, offset);
    image.layer = out;
    return true;
}

bool GuiRenderer::blur(float sigma, Image& image, Rml::LayerHandle keep)
{
    // As RmlUi's GL3 backend: halve the picture (bilinear) until under kMaxPassSigma texels of blur remain, then one
    // vertical and one horizontal Gaussian pass at that size. The result stays small; the next draw scales it up.
    // ponytail: content of the source layer outside the scissor region blurs in too (RmlUi clamps it out); clamp
    // the taps to the region if that ever shows.
    const int halvings = std::clamp(std::bit_width(static_cast<unsigned>(sigma * (2.0f / kMaxPassSigma))) - 1, 0, 10);
    const float scale = 1.0f / static_cast<float>(1 << halvings);
    while (image.scale > scale) {
        const Rml::LayerHandle out = spare({image.layer, keep});
        if (!nextView(out, true))
            return false;
        applyScissor(image.scale * 0.5f);
        bgfx::setState(kPassState);
        drawImage(image, image.scale * 0.5f, program_);
        image = {out, image.scale * 0.5f};
    }
    float weights[8];
    blurWeights(std::min(sigma * image.scale, kMaxPassSigma), weights);
    for (const Rml::Vector2f direction : {Rml::Vector2f(0.0f, 1.0f), Rml::Vector2f(1.0f, 0.0f)}) {
        const Rml::LayerHandle out = spare({image.layer, keep});
        if (!nextView(out, true))
            return false;
        const float texel[4] = {direction.x / width_, direction.y / height_, 0.0f, 0.0f};
        bgfx::setUniform(blur_, texel);
        bgfx::setUniform(weights_, weights, 2);
        applyScissor(image.scale);
        bgfx::setState(kPassState);
        drawImage(image, image.scale, blurProgram_);
        image.layer = out;
    }
    return true;
}

Rml::CompiledShaderHandle GuiRenderer::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    GuiGradient gradient;
    gradient.repeating = Rml::Get(parameters, "repeating", false);
    if (name == "linear-gradient") {
        gradient.p = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        gradient.v = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - gradient.p;
    } else if (name == "radial-gradient") {
        gradient.function = 1.0f;
        gradient.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        gradient.v = Rml::Vector2f(1.0f) / Rml::Vector2f(std::max(radius.x, 0.001f), std::max(radius.y, 0.001f));
    } else if (name == "conic-gradient") {
        gradient.function = 2.0f;
        gradient.p = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        gradient.v = {std::cos(angle), std::sin(angle)};
    } else {
        warnOnce(std::format("GUI shader '{}' is not supported: elements using it draw without it", name));
        return 0;
    }
    const auto stops = parameters.find("color_stop_list");
    if (stops == parameters.end() || stops->second.GetType() != Rml::Variant::COLORSTOPLIST ||
        stops->second.GetReference<Rml::ColorStopList>().empty()) {
        warnOnce(std::format("GUI {} has no color stops: elements using it draw without it", name));
        return 0;
    }
    // ponytail: stops past kMaxGradientStops are dropped (as RmlUi's GL3 backend does); raise MAX_STOPS in
    // fs_gui_gradient.sc with it if a design needs more.
    const Rml::ColorStopList& list = stops->second.GetReference<Rml::ColorStopList>();
    gradient.count = std::min(static_cast<int>(list.size()), kMaxGradientStops);
    for (int i = 0; i < gradient.count; ++i) {
        for (int c = 0; c < 4; ++c)
            gradient.colors[i][c] = list[i].color[c] / 255.0f;
        gradient.positions[i][0] = list[i].position.number; // RmlUi resolves them to 0..1 numbers
    }
    return reinterpret_cast<Rml::CompiledShaderHandle>(new GuiGradient(gradient));
}

void GuiRenderer::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                               Rml::Vector2f translation, Rml::TextureHandle /*texture*/)
{
    const auto* gradient = reinterpret_cast<const GuiGradient*>(shader);
    if (!gradient)
        return;
    const float params[4] = {gradient->function, gradient->repeating ? 1.0f : 0.0f, static_cast<float>(gradient->count),
                             0.0f};
    const float shape[4] = {gradient->p.x, gradient->p.y, gradient->v.x, gradient->v.y};
    bgfx::setUniform(gradient_, params);
    bgfx::setUniform(shape_, shape);
    bgfx::setUniform(stopColor_, gradient->colors, static_cast<uint16_t>(gradient->count));
    bgfx::setUniform(stopPos_, gradient->positions, static_cast<uint16_t>(gradient->count));
    submit(geometry, translation, white_, gradientProgram_, kOverState, clipTest());
}

void GuiRenderer::ReleaseShader(Rml::CompiledShaderHandle shader) { delete reinterpret_cast<GuiGradient*>(shader); }

} // namespace kast
