#pragma once

#include <bgfx/bgfx.h>

struct ImDrawData;

// Minimal Dear ImGui renderer backend for bgfx (Direct3D 11 shaders only).
namespace imgui_bgfx {

void init(bgfx::ViewId view);
void render(ImDrawData* drawData);
void shutdown();

} // namespace imgui_bgfx
