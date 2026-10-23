#pragma once

#include "scene.h"
#include "texture_cook.h"

#include <bgfx/bgfx.h>

#include <array>
#include <filesystem>
#include <string>

namespace kast {

struct MipImage; // renderer.h

// CPU mirror of fs_sky.sc's gradient, for self_check: horizon blended toward top (dirY > 0) or bottom (dirY < 0) by
// sqrt(|dirY|), where dirY is the unit view direction's y (-1 straight down .. 1 straight up).
bx::Vec3 proceduralSkyColor(const SceneSky& sky, float dirY);

// Why six decoded faces (kSkyFaceNames order) can't make one cube: the first face that is not square or not the size
// of px, named. Empty if they can.
std::string skyFacesError(const std::array<MipImage, 6>& faces);
// Six decoded faces that make a cube (skyFacesError empty) as the RGBA8 cube KTX the sky cooks from (cubeKtx).
std::string skyCubeKtx(const std::array<MipImage, 6>& faces);

// Draws a SceneSky behind everything else in a view (Renderer::render calls it between opaque and blended meshes):
// a full-screen triangle at the far plane, depth-tested so it only fills pixels no mesh covered. Uses the camera's
// rotation only. The cubemap's six faces, or its one cube .ktx / .dds (SceneSky::cube, as stored, its own mips), load
// once per set of paths (sRGB, so the output is linear); a face that is missing, unreadable, undecodable, not square or
// a different size, or a cube file that is corrupt, 2D or in a format the GPU lacks, warns once and the procedural sky
// draws instead. The six faces are cooked to one compressed cube (TextureCooks, as Color): in the editor an RGBA8 cube
// draws until the cook is in; the player loads the cube Build cooked (skyPakPath).
class SkyRenderer {
public:
    void init();
    void shutdown();
    // Faces load from projectRoot (empty: none, so a cubemap falls back).
    void draw(const SceneSky& sky, const std::filesystem::path& projectRoot, bgfx::ViewId view, const float* viewMtx,
              const float* projMtx);
    // Why the last cubemap drawn fell back to the procedural sky ("" = it didn't, or no cubemap was drawn).
    const std::string& error() const { return error_; }

private:
    void load(const SceneSky& sky, const std::filesystem::path& root);
    void loadCube(const std::string& path, const std::filesystem::path& root);
    // If the cook of the faces is in: its cube replaces cube_ (true), or it failed (warned once; cube_ stays).
    bool takeCooked();
    void unload();

    bgfx::VertexBufferHandle triangle_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle invViewProj_ = BGFX_INVALID_HANDLE, top_ = BGFX_INVALID_HANDLE, horizon_ = BGFX_INVALID_HANDLE,
                        bottom_ = BGFX_INVALID_HANDLE, sampler_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle cube_ = BGFX_INVALID_HANDLE; // invalid: not loaded, or failed (error_)
    bool loaded_ = false;                            // faces_ / cubeFile_ / root_ say what cube_ (or error_) is for
    std::array<std::string, 6> faces_;
    std::string cubeFile_;
    std::filesystem::path root_;
    std::string error_;
    std::shared_future<CookedTexture> cooking_; // valid: cube_ is the RGBA8 cube until this cook is in
};

} // namespace kast
