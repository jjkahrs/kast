// Navtile files (<map>/.server/nav/<tx>_<tz>.navtile, baked by tools/mapbake, loaded by the server's nav.cpp): this
// 12-byte little-endian header, then one Detour tile (dtCreateNavMeshData) for tile (tx, tz) of the map.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace kast::map {

constexpr char kNavtileMagic[4] = {'K', 'N', 'A', 'V'};
constexpr uint32_t kNavtileVersion = 1; // bump when the bake's input or layout changes meaning
constexpr size_t kNavtileHeaderBytes = 12;

struct NavtileHeader {
    uint32_t version = kNavtileVersion;
    uint32_t dtVersion = 0; // DT_NAVMESH_VERSION of the Detour that baked it
};

inline std::string navtile_header(uint32_t dtVersion) {
    std::string out(kNavtileMagic, 4);
    for (uint32_t v : {kNavtileVersion, dtVersion})
        for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xff);
    return out;
}

// The header of a navtile's bytes; nullopt with err set if it is too short or not a navtile. Versions are the reader's
// to check.
inline std::optional<NavtileHeader> parse_navtile_header(std::string_view bytes, std::string& err) {
    if (bytes.size() < kNavtileHeaderBytes || std::memcmp(bytes.data(), kNavtileMagic, 4) != 0) {
        err = "not a navtile (bad magic, want 'KNAV')";
        return std::nullopt;
    }
    const auto u32 = [&](size_t at) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(bytes[at + i])) << (8 * i);
        return v;
    };
    return NavtileHeader{u32(4), u32(8)};
}

} // namespace kast::map
