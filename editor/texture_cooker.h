#pragma once

#include <texture_cook.h>

#include <string>
#include <string_view>

// The editor's kast::TextureCooker: runs the texturec.exe next to this program (bgfx's texture compiler, from vcpkg) on
// the image: mips, format as given, --linear for Linear usage, --normalmap for Normal. Never linked into the player.
bool cookTextureWithTexturec(std::string_view image, kast::TextureUsage usage, bgfx::TextureFormat::Enum format,
                             std::string& ktx, std::string& error);
// The texture cache's version (TextureCooks::setCooker): texturec.exe's size and time, so a rebuilt one misses the cache.
std::string texturecVersion();
