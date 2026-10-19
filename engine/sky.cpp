#include "sky.h"

#include "files.h"
#include "material.h"
#include "project.h"
#include "renderer.h"

#include "fs_sky.bin.h"
#include "vs_sky.bin.h"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

namespace kast {
namespace {

// Depth-tested against the cleared far plane (1): fills only where no opaque mesh wrote depth.
constexpr uint64_t kSkyState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL;
// bgfx cube sides are +X, -X, +Y, -Y, +Z, -Z in D3D's left-handed space; fs_sky.sc samples it with z flipped, so
// Kast's +Z face goes in the -Z side and back.
constexpr size_t kFaceOfSide[6] = {0, 1, 2, 3, 5, 4};

} // namespace

bx::Vec3 proceduralSkyColor(const SceneSky& sky, float dirY)
{
    return bx::lerp(sky.horizon, dirY >= 0.0f ? sky.top : sky.bottom, std::sqrt(std::abs(dirY)));
}

std::string skyFacesError(const std::array<MipImage, 6>& faces)
{
    for (size_t i = 0; i < faces.size(); ++i) {
        const MipImage& face = faces[i];
        if (face.width != face.height)
            return std::format("face {} is {}x{}, not square", kSkyFaceNames[i], face.width, face.height);
        if (face.width != faces[0].width)
            return std::format("face {} is {}x{}, but px is {}x{}", kSkyFaceNames[i], face.width, face.height,
                               faces[0].width, faces[0].height);
    }
    return {};
}

void SkyRenderer::init()
{
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    const float triangle[3][3] = {{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}}; // past the corners
    triangle_ = bgfx::createVertexBuffer(bgfx::copy(triangle, sizeof(triangle)), layout);
    program_ = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_sky, sizeof(vs_sky))),
                                   bgfx::createShader(bgfx::makeRef(fs_sky, sizeof(fs_sky))), true);
    invViewProj_ = bgfx::createUniform("u_skyInvViewProj", bgfx::UniformType::Mat4);
    top_ = bgfx::createUniform("u_skyTop", bgfx::UniformType::Vec4);
    horizon_ = bgfx::createUniform("u_skyHorizon", bgfx::UniformType::Vec4);
    bottom_ = bgfx::createUniform("u_skyBottom", bgfx::UniformType::Vec4);
    sampler_ = bgfx::createUniform("s_sky", bgfx::UniformType::Sampler);
}

void SkyRenderer::shutdown()
{
    unload();
    bgfx::destroy(triangle_);
    bgfx::destroy(program_);
    for (bgfx::UniformHandle uniform : {invViewProj_, top_, horizon_, bottom_, sampler_})
        bgfx::destroy(uniform);
}

void SkyRenderer::unload()
{
    if (bgfx::isValid(cube_))
        bgfx::destroy(cube_);
    cube_ = BGFX_INVALID_HANDLE;
    loaded_ = false;
    error_.clear();
}

void SkyRenderer::load(const std::array<std::string, 6>& faces, const std::filesystem::path& root)
{
    unload();
    loaded_ = true;
    faces_ = faces;
    root_ = root;
    const auto fail = [&](const std::string& why) {
        error_ = why;
        warnOnce(std::format("Sky cubemap: {}; using the procedural sky", why));
    };
    std::array<MipImage, 6> images;
    for (size_t i = 0; i < faces.size(); ++i) {
        std::string why = "no project";
        std::optional<std::filesystem::path> file;
        if (faces[i].empty())
            why = "no image set";
        else if (!root.empty())
            file = resolveProjectPath(root, faces[i], why);
        std::string bytes;
        std::optional<MipImage> image;
        if (file && !readFile(*file, bytes))
            why = "cannot read the file";
        else if (file && !(image = decodeImage(bytes, true)))
            why = "cannot decode it as PNG or JPG";
        if (!image)
            return fail(std::format("face {} '{}': {}", kSkyFaceNames[i], faces[i], why));
        images[i] = std::move(*image);
    }
    if (const std::string why = skyFacesError(images); !why.empty())
        return fail(why);
    std::vector<uint8_t> data; // per side, its mips (bgfx's cube layout)
    for (size_t face : kFaceOfSide)
        data.insert(data.end(), images[face].rgba.begin(), images[face].rgba.end());
    cube_ = bgfx::createTextureCube(images[0].width, images[0].levels > 1, 1, bgfx::TextureFormat::RGBA8,
                                    BGFX_TEXTURE_SRGB | BGFX_SAMPLER_UVW_CLAMP,
                                    bgfx::copy(data.data(), static_cast<uint32_t>(data.size())));
}

// ponytail: each Renderer (Scene view, Game view) holds its own copy of the cube; share them if skies get big.
void SkyRenderer::draw(const SceneSky& sky, const std::filesystem::path& projectRoot, bgfx::ViewId view,
                       const float* viewMtx, const float* projMtx)
{
    if (sky.mode != SkyMode::Cubemap)
        unload(); // a scene or mode switch frees the cube
    if (sky.mode == SkyMode::Off)
        return;
    if (sky.mode == SkyMode::Cubemap && (!loaded_ || faces_ != sky.faces || root_ != projectRoot))
        load(sky.faces, projectRoot);

    float rotation[16], viewProj[16], inverse[16];
    std::copy_n(viewMtx, 16, rotation);
    rotation[12] = rotation[13] = rotation[14] = 0.0f; // the sky doesn't move with the camera
    bx::mtxMul(viewProj, rotation, projMtx);
    bx::mtxInverse(inverse, viewProj);
    const bool cubemap = bgfx::isValid(cube_);
    const float top[4] = {sky.top.x, sky.top.y, sky.top.z, cubemap ? 1.0f : 0.0f};
    const float horizon[4] = {sky.horizon.x, sky.horizon.y, sky.horizon.z, 0.0f};
    const float bottom[4] = {sky.bottom.x, sky.bottom.y, sky.bottom.z, 0.0f};
    bgfx::setUniform(invViewProj_, inverse);
    bgfx::setUniform(top_, top);
    bgfx::setUniform(horizon_, horizon);
    bgfx::setUniform(bottom_, bottom);
    if (cubemap)
        bgfx::setTexture(0, sampler_, cube_);
    bgfx::setVertexBuffer(0, triangle_);
    bgfx::setState(kSkyState);
    bgfx::submit(view, program_);
}

} // namespace kast
