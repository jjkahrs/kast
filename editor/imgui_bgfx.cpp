#include "imgui_bgfx.h"

#include "fs_imgui.bin.h"
#include "vs_imgui.bin.h"

#include <bx/math.h>
#include <imgui.h>

#include <cstring>

namespace imgui_bgfx {
namespace {

bgfx::ViewId g_view = 0;
bgfx::VertexLayout g_layout;
bgfx::ProgramHandle g_program = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_sampler = BGFX_INVALID_HANDLE;

bgfx::TextureHandle toHandle(ImTextureID id) { return {static_cast<uint16_t>(id)}; }

void destroyTexture(ImTextureData* tex)
{
    bgfx::destroy(toHandle(tex->GetTexID()));
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

void uploadRect(ImTextureData* tex, int x, int y, int w, int h)
{
    const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(w * h * tex->BytesPerPixel));
    const size_t rowBytes = static_cast<size_t>(w * tex->BytesPerPixel);
    for (int row = 0; row < h; ++row)
        std::memcpy(mem->data + row * rowBytes, tex->GetPixelsAt(x, y + row), rowBytes);
    bgfx::updateTexture2D(toHandle(tex->GetTexID()), 0, 0, static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                          static_cast<uint16_t>(w), static_cast<uint16_t>(h), mem);
}

void updateTexture(ImTextureData* tex)
{
    switch (tex->Status) {
    case ImTextureStatus_WantCreate: {
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        // Created without data so it stays updatable (bgfx makes textures with initial data immutable).
        bgfx::TextureHandle handle = bgfx::createTexture2D(static_cast<uint16_t>(tex->Width), static_cast<uint16_t>(tex->Height),
                                                           false, 1, bgfx::TextureFormat::RGBA8);
        tex->SetTexID(static_cast<ImTextureID>(handle.idx));
        uploadRect(tex, 0, 0, tex->Width, tex->Height);
        tex->SetStatus(ImTextureStatus_OK);
        break;
    }
    case ImTextureStatus_WantUpdates:
        for (const ImTextureRect& r : tex->Updates)
            uploadRect(tex, r.x, r.y, r.w, r.h);
        tex->SetStatus(ImTextureStatus_OK);
        break;
    case ImTextureStatus_WantDestroy:
        if (tex->UnusedFrames > 0)
            destroyTexture(tex);
        break;
    default:
        break;
    }
}

} // namespace

void init(bgfx::ViewId view)
{
    g_view = view;
    g_layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    g_program = bgfx::createProgram(bgfx::createShader(bgfx::makeRef(vs_imgui, sizeof(vs_imgui))),
                                    bgfx::createShader(bgfx::makeRef(fs_imgui, sizeof(fs_imgui))), true);
    g_sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_bgfx";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // a list past 64k vertices (16-bit indices) splits
}

void render(ImDrawData* drawData)
{
    if (drawData->Textures)
        for (ImTextureData* tex : *drawData->Textures)
            if (tex->Status != ImTextureStatus_OK)
                updateTexture(tex);

    const ImVec2 pos = drawData->DisplayPos;
    const ImVec2 scale = drawData->FramebufferScale;
    const float fbWidth = drawData->DisplaySize.x * scale.x;
    const float fbHeight = drawData->DisplaySize.y * scale.y;
    if (fbWidth <= 0.0f || fbHeight <= 0.0f)
        return;

    float ortho[16];
    bx::mtxOrtho(ortho, pos.x, pos.x + drawData->DisplaySize.x, pos.y + drawData->DisplaySize.y, pos.y,
                 0.0f, 1000.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewName(g_view, "ImGui");
    bgfx::setViewMode(g_view, bgfx::ViewMode::Sequential);
    bgfx::setViewTransform(g_view, nullptr, ortho);
    bgfx::setViewRect(g_view, 0, 0, static_cast<uint16_t>(fbWidth), static_cast<uint16_t>(fbHeight));

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
        BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                       BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);

    for (const ImDrawList* list : drawData->CmdLists) {
        const auto numVertices = static_cast<uint32_t>(list->VtxBuffer.Size);
        const auto numIndices = static_cast<uint32_t>(list->IdxBuffer.Size);
        // ponytail: drops the rest of the frame if transient buffers run out; raise bgfx limits if UI ever gets that big.
        if (bgfx::getAvailTransientVertexBuffer(numVertices, g_layout) < numVertices ||
            bgfx::getAvailTransientIndexBuffer(numIndices) < numIndices)
            break;

        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, numVertices, g_layout);
        bgfx::allocTransientIndexBuffer(&tib, numIndices, sizeof(ImDrawIdx) == 4);
        std::memcpy(tvb.data, list->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, list->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            const float x0 = bx::max((cmd.ClipRect.x - pos.x) * scale.x, 0.0f);
            const float y0 = bx::max((cmd.ClipRect.y - pos.y) * scale.y, 0.0f);
            const float x1 = bx::min((cmd.ClipRect.z - pos.x) * scale.x, fbWidth);
            const float y1 = bx::min((cmd.ClipRect.w - pos.y) * scale.y, fbHeight);
            if (x1 <= x0 || y1 <= y0)
                continue;

            bgfx::setScissor(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                             static_cast<uint16_t>(x1 - x0), static_cast<uint16_t>(y1 - y0));
            bgfx::setState(state);
            bgfx::setTexture(0, g_sampler, toHandle(cmd.GetTexID()));
            bgfx::setVertexBuffer(0, &tvb, cmd.VtxOffset, numVertices - cmd.VtxOffset);
            bgfx::setIndexBuffer(&tib, cmd.IdxOffset, cmd.ElemCount);
            bgfx::submit(g_view, g_program);
        }
    }
}

void shutdown()
{
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1 && tex->Status != ImTextureStatus_Destroyed)
            destroyTexture(tex);
    bgfx::destroy(g_sampler);
    bgfx::destroy(g_program);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
}

} // namespace imgui_bgfx
