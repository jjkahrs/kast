#include "terrain_edit.h"

#include <scene_io.h> // jsonNumber

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace zone {
namespace {

namespace km = kast::map;
constexpr float kInf = std::numeric_limits<float>::infinity();

Vec3 sub(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Moller-Trumbore, both faces; the distance along dir or a negative value on a miss.
float hit_triangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 e1 = sub(b, a), e2 = sub(c, a), p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return -1;
    const Vec3 s = sub(o, a);
    const float u = dot(s, p) / det;
    if (u < 0 || u > 1) return -1;
    const Vec3 q = cross(s, e1);
    const float v = dot(d, q) / det;
    if (v < 0 || u + v > 1) return -1;
    return dot(e2, q) / det;
}

// The ground triangles of 1 m cell (cx, cz) (map metres), as for_each_triangle splits it; t of the first hit or -1.
float hit_cell(const km::MapInfo& info, const ZoneState& state, int cx, int cz, Vec3 o, Vec3 d) {
    const TileKey t{int32_t(std::floor(cx / km::kTileSize)), int32_t(std::floor(cz / km::kTileSize))};
    if (!info.has_tile(t)) return -1;
    const km::Heights& h = tile_heights(state, t);
    const int u = cx - t.x * int(km::kTileSize), v = cz - t.z * int(km::kTileSize);
    const auto y = [&](int du, int dv) { return km::sample_height(h[size_t(v + dv) * km::kTileSamples + size_t(u + du)], info); };
    const float x0 = float(cx), z0 = float(cz);
    const Vec3 p00{x0, y(0, 0), z0}, p10{x0 + 1, y(1, 0), z0}, p01{x0, y(0, 1), z0 + 1}, p11{x0 + 1, y(1, 1), z0 + 1};
    const float a = hit_triangle(o, d, p00, p01, p11), b = hit_triangle(o, d, p00, p11, p10);
    if (a >= 0 && b >= 0) return std::min(a, b);
    return std::max(a, b);
}

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint16_t quantize(float y, float lo, float span) {
    if (span <= 0) return 0;
    return uint16_t(std::clamp(std::lround(double(y - lo) / span * 65535.0), 0l, 65535l));
}

// A declared tile holding a map sample, and the sample's index in it.
struct Holder {
    TileKey tile;
    size_t index;
};

// The declared tiles holding map sample (gx, gz): one, two on an edge, four on a corner. Returns how many.
int holders(const km::MapInfo& info, int gx, int gz, Holder out[4]) {
    constexpr int n = int(km::kTileSize);
    const int tx = floor_div(gx, n), tz = floor_div(gz, n);
    int count = 0;
    for (int dz = 0; dz <= (gz - tz * n == 0 ? 1 : 0); ++dz)
        for (int dx = 0; dx <= (gx - tx * n == 0 ? 1 : 0); ++dx) {
            const TileKey t{tx - dx, tz - dz};
            if (!info.has_tile(t)) continue;
            const int u = gx - t.x * n, v = gz - t.z * n;
            out[count++] = {t, size_t(v) * km::kTileSamples + size_t(u)};
        }
    return count;
}

template <typename Fn>
void for_each_declared(const km::MapInfo& info, Fn&& fn) {
    TileSet seen; // rects may overlap
    for (const km::TileRect& r : info.tiles)
        for (int32_t z = r.z0; z <= r.z1; ++z)
            for (int32_t x = r.x0; x <= r.x1; ++x)
                if (seen.insert({x, z}).second) fn(TileKey{x, z});
}

// PNG chunk CRC (ISO 3309).
uint32_t crc32(const unsigned char* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char((v >> shift) & 0xFF));
}

void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, uint32_t(data.size()));
    const std::string body = std::string(type, 4) + data;
    png += body;
    put32(png, crc32(reinterpret_cast<const unsigned char*>(body.data()), body.size()));
}

} // namespace

const km::Heights& tile_heights(const ZoneState& state, TileKey t) {
    static const km::Heights flat = km::flat_heights();
    const auto it = state.heights.find(t);
    return it == state.heights.end() ? flat : *it->second;
}

std::optional<float> ground_y(const km::MapInfo& info, const ZoneState& state, float x, float z) {
    const TileKey t = km::tile_of(x, z);
    if (!info.has_tile(t)) return std::nullopt;
    return km::ground(tile_heights(state, t), info, x - t.x * km::kTileSize, z - t.z * km::kTileSize);
}

std::optional<Vec3> raycast(const km::MapInfo& info, const ZoneState& state, Vec3 o, Vec3 d, float maxDist) {
    if (info.tiles.empty()) return std::nullopt;
    // Clip to the box around every declared tile (x/z only; the ground can be anywhere in y).
    float lo[2] = {kInf, kInf}, hi[2] = {-kInf, -kInf};
    for (const km::TileRect& r : info.tiles) {
        lo[0] = std::min(lo[0], r.x0 * km::kTileSize);
        lo[1] = std::min(lo[1], r.z0 * km::kTileSize);
        hi[0] = std::max(hi[0], (r.x1 + 1) * km::kTileSize);
        hi[1] = std::max(hi[1], (r.z1 + 1) * km::kTileSize);
    }
    float t0 = 0, t1 = maxDist;
    for (int a = 0; a < 2; ++a) {
        const float od = o[a * 2], dd = d[a * 2];
        if (std::fabs(dd) < 1e-9f) {
            if (od < lo[a] || od >= hi[a]) return std::nullopt;
            continue;
        }
        float ta = (lo[a] - od) / dd, tb = (hi[a] - od) / dd;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t0 > t1) return std::nullopt;

    // 2D DDA over the 1 m cells the ray crosses (Amanatides & Woo).
    const float sx = o[0] + d[0] * t0, sz = o[2] + d[2] * t0;
    int cx = int(std::floor(std::clamp(sx, lo[0], hi[0] - 1e-3f))), cz = int(std::floor(std::clamp(sz, lo[1], hi[1] - 1e-3f)));
    const int stepX = d[0] > 0 ? 1 : -1, stepZ = d[2] > 0 ? 1 : -1;
    const float dx = std::fabs(d[0]) < 1e-9f ? kInf : 1 / std::fabs(d[0]);
    const float dz = std::fabs(d[2]) < 1e-9f ? kInf : 1 / std::fabs(d[2]);
    float nextX = dx == kInf ? kInf : t0 + (d[0] > 0 ? (cx + 1 - sx) : (sx - cx)) * dx;
    float nextZ = dz == kInf ? kInf : t0 + (d[2] > 0 ? (cz + 1 - sz) : (sz - cz)) * dz;
    for (float t = t0; t <= t1;) {
        if (const float hit = hit_cell(info, state, cx, cz, o, d); hit >= 0 && hit <= maxDist)
            return Vec3{o[0] + d[0] * hit, o[1] + d[1] * hit, o[2] + d[2] * hit};
        if (nextX < nextZ) {
            t = nextX;
            nextX += dx;
            cx += stepX;
        } else {
            t = nextZ;
            nextZ += dz;
            cz += stepZ;
        }
    }
    return std::nullopt;
}

float brush_weight(const Brush& brush, float d) {
    if (d >= brush.radius) return 0;
    const float inner = brush.radius * (1 - std::clamp(brush.falloff, 0.0f, 1.0f));
    if (d <= inner) return 1;
    const float t = (brush.radius - d) / (brush.radius - inner);
    return t * t * (3 - 2 * t);
}

TileSet apply_brush(const km::MapInfo& info, ZoneState& state, float x, float z, const Brush& brush, float amount) {
    const auto [lo, hi] = km::height_range(info);
    const float span = hi - lo;
    if (span <= 0 || brush.radius <= 0) return {};
    TileMap<std::shared_ptr<km::Heights>> fresh; // this dab's new buffers
    const auto read = [&](const Holder& h) {
        const auto it = fresh.find(h.tile);
        return it != fresh.end() ? (*it->second)[h.index] : tile_heights(state, h.tile)[h.index];
    };
    Holder hs[4];
    for (int gz = int(std::ceil(z - brush.radius)); gz <= int(std::floor(z + brush.radius)); ++gz)
        for (int gx = int(std::ceil(x - brush.radius)); gx <= int(std::floor(x + brush.radius)); ++gx) {
            const float w = brush_weight(brush, std::hypot(float(gx) - x, float(gz) - z));
            const int count = w > 0 ? holders(info, gx, gz, hs) : 0;
            if (count == 0) continue;
            const uint16_t s = read(hs[0]);
            const float y = lo + float(s) / 65535.0f * span;
            float ny = y;
            switch (brush.mode) {
            case BrushMode::Raise: ny = y + amount * w; break;
            case BrushMode::Lower: ny = y - amount * w; break;
            case BrushMode::Flatten: ny = y + (brush.target - y) * std::clamp(amount * w, 0.0f, 1.0f); break;
            }
            const uint16_t ns = quantize(ny, lo, span);
            if (ns == s) continue;
            for (int i = 0; i < count; ++i) {
                auto& buffer = fresh[hs[i].tile];
                if (!buffer) buffer = std::make_shared<km::Heights>(tile_heights(state, hs[i].tile));
                (*buffer)[hs[i].index] = ns;
            }
        }
    TileSet touched;
    for (auto& [t, buffer] : fresh) {
        state.heights[t] = std::move(buffer);
        touched.insert(t);
    }
    return touched;
}

size_t count_clipped(const km::MapInfo& info, const ZoneState& state, float newMin, float newMax) {
    size_t clipped = 0;
    for_each_declared(info, [&](TileKey t) {
        for (uint16_t s : tile_heights(state, t)) {
            const float y = km::sample_height(s, info);
            clipped += y < newMin || y > newMax;
        }
    });
    return clipped;
}

size_t set_height_range(const km::MapInfo& info, ZoneState& state, float newMin, float newMax) {
    const size_t clipped = count_clipped(info, state, newMin, newMax);
    for_each_declared(info, [&](TileKey t) {
        const km::Heights& old = tile_heights(state, t);
        auto fresh = std::make_shared<km::Heights>(old.size());
        for (size_t i = 0; i < old.size(); ++i) (*fresh)[i] = quantize(km::sample_height(old[i], info), newMin, newMax - newMin);
        const bool flat = std::all_of(fresh->begin(), fresh->end(), [](uint16_t s) { return s == 0; });
        if (flat && !state.heights.contains(t)) return; // still flat at the (new) minimum: no file needed
        state.heights[t] = std::move(fresh);
    });
    state.manifest["heightRange"] = Json::array({kast::jsonNumber(newMin), kast::jsonNumber(newMax)});
    return clipped;
}

std::string encode_png16(const km::Heights& heights) {
    constexpr uint32_t n = km::kTileSamples;
    std::string raw; // per row: filter 0, then big-endian 16-bit samples
    raw.reserve(size_t(n) * (1 + 2 * n));
    for (uint32_t v = 0; v < n; ++v) {
        raw.push_back(0);
        for (uint32_t u = 0; u < n; ++u) {
            const uint16_t s = heights[size_t(v) * n + u];
            raw.push_back(char(s >> 8));
            raw.push_back(char(s & 0xFF));
        }
    }
    std::string z = {char(0x78), char(0x01)}; // zlib header: deflate, no compression
    for (size_t at = 0; at < raw.size();) {
        const size_t len = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + len == raw.size() ? 1 : 0); // a stored block; 1 = the last
        z.push_back(char(len & 0xFF));
        z.push_back(char(len >> 8));
        z.push_back(char(~len & 0xFF));
        z.push_back(char((~len >> 8) & 0xFF));
        z.append(raw, at, len);
        at += len;
    }
    uint32_t a = 1, b = 0; // Adler-32
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, n);
    put32(ihdr, n);
    ihdr += {char(16), char(0), char(0), char(0), char(0)}; // 16-bit grayscale, deflate, adaptive filters, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool import_png16(const km::MapInfo& info, ZoneState& state, TileKey tile, std::string_view png, std::string& err) {
    auto heights = km::decode_heights(png, ".png", err);
    if (!heights) return false;
    const auto imported = std::make_shared<const km::Heights>(std::move(*heights));
    state.heights[tile] = imported;
    constexpr int n = int(km::kTileSize);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const TileKey nb{tile.x + dx, tile.z + dz};
            if ((dx == 0 && dz == 0) || !info.has_tile(nb)) continue;
            auto copy = std::make_shared<km::Heights>(tile_heights(state, nb));
            bool changed = false;
            // Neighbour sample (u, v) is the imported tile's (u + dx * 256, v + dz * 256) where that is on the tile.
            for (int v = 0; v <= n; ++v)
                for (int u = 0; u <= n; ++u) {
                    const int iu = u + dx * n, iv = v + dz * n;
                    if (iu < 0 || iu > n || iv < 0 || iv > n) continue;
                    uint16_t& s = (*copy)[size_t(v) * km::kTileSamples + size_t(u)];
                    const uint16_t want = (*imported)[size_t(iv) * km::kTileSamples + size_t(iu)];
                    changed |= s != want;
                    s = want;
                }
            if (changed) state.heights[nb] = std::move(copy);
        }
    return true;
}

TileSet declared_tiles(const km::MapInfo& info) {
    TileSet tiles;
    for_each_declared(info, [&](TileKey t) { tiles.insert(t); });
    return tiles;
}

Json tile_rects(const TileSet& tiles) {
    // Runs per row: z -> [x0, x1] pairs, in order.
    std::map<int32_t, std::vector<std::array<int32_t, 2>>> rows;
    for (TileKey t : tiles) { // sorted by z then x
        auto& runs = rows[t.z];
        if (!runs.empty() && runs.back()[1] == t.x - 1) runs.back()[1] = t.x;
        else runs.push_back({t.x, t.x});
    }
    Json rects = Json::array();
    std::map<std::array<int32_t, 2>, size_t> open; // a run -> its rect, while the rows above keep it
    int32_t lastZ = 0;
    bool first = true;
    for (const auto& [z, runs] : rows) {
        std::map<std::array<int32_t, 2>, size_t> next;
        for (const auto& run : runs) {
            const auto it = open.find(run);
            if (!first && z == lastZ + 1 && it != open.end()) {
                rects[it->second][3] = z;
                next[run] = it->second;
            } else {
                rects.push_back(Json::array({run[0], z, run[1], z}));
                next[run] = rects.size() - 1;
            }
        }
        open = std::move(next);
        lastZ = z;
        first = false;
    }
    return rects;
}

bool add_tile(const km::MapInfo& info, ZoneState& state, TileKey t) {
    TileSet tiles = declared_tiles(info);
    if (!tiles.insert(t).second) return false;
    state.manifest["tiles"] = tile_rects(tiles);
    auto fresh = std::make_shared<km::Heights>(km::flat_heights());
    bool shaped = false;
    constexpr int n = int(km::kTileSize);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const TileKey nb{t.x + dx, t.z + dz};
            if ((dx == 0 && dz == 0) || !info.has_tile(nb)) continue;
            const km::Heights& from = tile_heights(state, nb);
            // Our sample (u, v) is the neighbour's (u - dx * 256, v - dz * 256) where that is on it.
            for (int v = 0; v <= n; ++v)
                for (int u = 0; u <= n; ++u) {
                    const int nu = u - dx * n, nv = v - dz * n;
                    if (nu < 0 || nu > n || nv < 0 || nv > n) continue;
                    const uint16_t s = from[size_t(nv) * km::kTileSamples + size_t(nu)];
                    (*fresh)[size_t(v) * km::kTileSamples + size_t(u)] = s;
                    shaped |= s != 0;
                }
        }
    if (shaped) state.heights[t] = std::move(fresh);
    return true;
}

bool remove_tile(const km::MapInfo& info, ZoneState& state, TileKey t) {
    TileSet tiles = declared_tiles(info);
    if (tiles.size() <= 1 || !tiles.erase(t)) return false;
    state.manifest["tiles"] = tile_rects(tiles);
    state.heights.erase(t);
    state.spawns.erase(t);
    return true;
}

} // namespace zone
