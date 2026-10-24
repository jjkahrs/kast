#include "lightmap.h"

#include "renderer.h"

#include <bimg/bimg.h>
#include <bx/error.h>
#include <bx/readerwriter.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr char kMeshesMagic[4] = {'K', 'L', 'M', 'M'};

class StringWriter : public bx::WriterI {
public:
    explicit StringWriter(std::string& out) : out_(out) {}
    int32_t write(const void* data, int32_t size, bx::Error*) override
    {
        out_.append(static_cast<const char*>(data), static_cast<size_t>(size));
        return size;
    }

private:
    std::string& out_;
};

// Little-endian POD reads that fail softly (ok false) past the end.
struct Reader {
    std::string_view bytes;
    size_t at = 0;
    bool ok = true;
    template <typename T>
    T pod()
    {
        T value{};
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    std::string string()
    {
        const auto n = pod<uint32_t>();
        if (!ok || bytes.size() - at < n) {
            ok = false;
            return {};
        }
        std::string s(bytes.substr(at, n));
        at += n;
        return s;
    }
    template <typename T>
    bool array(std::vector<T>& out, size_t count)
    {
        if (!ok || count > (bytes.size() - at) / sizeof(T)) {
            ok = false;
            return false;
        }
        out.resize(count);
        std::memcpy(out.data(), bytes.data() + at, count * sizeof(T));
        at += count * sizeof(T);
        return true;
    }
};

template <typename T>
void put(std::string& out, const T& value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void putString(std::string& out, const std::string& s)
{
    put(out, static_cast<uint32_t>(s.size()));
    out += s;
}

template <typename T>
void putArray(std::string& out, const std::vector<T>& values)
{
    out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
}

std::string hex(uint64_t value) { return std::format("{:016x}", value); }

bool parseHex(const Json& value, uint64_t& out)
{
    if (!value.is_string() || value.get<std::string>().size() != 16)
        return false;
    const std::string s = value.get<std::string>();
    out = 0;
    for (char c : s) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0)
            return false;
        out = out * 16 + uint64_t(digit);
    }
    return true;
}

bool finite(const Json& value) { return value.is_number() && std::isfinite(value.get<double>()); }

} // namespace

DiffuseGi selectDiffuseGi(bool lightmapped, bool probeGrid)
{
    if (lightmapped)
        return DiffuseGi::Lightmap;
    return probeGrid ? DiffuseGi::ProbeGrid : DiffuseGi::Environment;
}

bool lightShinesOn(LightBakeMode mode, bool lightmapped) { return !(lightmapped && mode == LightBakeMode::Full); }

bx::Vec3 lightmapDiffuse(const bx::Vec3& irradiance, const std::array<float, 4>& direction, const bx::Vec3& n,
                         bool directional)
{
    if (!directional)
        return irradiance;
    // As fs_mesh.sc: half-Lambert toward the mean direction, over the stored half-Lambert at the surface's own normal.
    const float bend = (bx::dot(n, {direction[0], direction[1], direction[2]}) * 0.5f + 0.5f) / std::max(direction[3], 0.25f);
    return bx::mul(irradiance, bend);
}

std::optional<Sh9> sampleProbeGrid(const ProbeGrid& grid, const bx::Vec3& point)
{
    if (grid.empty())
        return std::nullopt;
    float f[3];
    int lo[3];
    const float p[3] = {point.x - grid.origin.x, point.y - grid.origin.y, point.z - grid.origin.z};
    for (int a = 0; a < 3; ++a) {
        const float cell = std::clamp(p[a] / grid.spacing, 0.0f, float(std::max(grid.counts[a] - 1, 0)));
        lo[a] = std::min(int(cell), std::max(grid.counts[a] - 2, 0));
        f[a] = grid.counts[a] > 1 ? cell - float(lo[a]) : 0.0f;
    }
    // Trilinear over the valid corners; on (or right next to) an invalid probe, the valid corners of the cell evenly.
    for (const bool even : {false, true}) {
        Sh9 sum{};
        float total = 0.0f;
        for (int corner = 0; corner < 8; ++corner) {
            const int c[3] = {corner & 1, (corner >> 1) & 1, (corner >> 2) & 1};
            float weight = 1.0f;
            int at[3];
            for (int a = 0; a < 3; ++a) {
                weight *= even ? 1.0f : c[a] ? f[a] : 1.0f - f[a];
                at[a] = std::min(lo[a] + c[a], grid.counts[a] - 1);
            }
            const size_t i = grid.index(at[0], at[1], at[2]);
            if (weight <= 0.0f || !grid.valid[i])
                continue;
            for (size_t k = 0; k < sum.size(); ++k)
                sum[k] += grid.sh[i][k] * weight;
            total += weight;
        }
        if (total < 1e-4f)
            continue;
        for (float& v : sum)
            v /= total;
        return sum;
    }
    return std::nullopt;
}
std::string writeLightmapMeshes(const std::vector<LightmapMesh>& meshes)
{
    std::string out(kMeshesMagic, sizeof(kMeshesMagic));
    put(out, static_cast<uint32_t>(kLightmapFormatVersion));
    put(out, static_cast<uint32_t>(meshes.size()));
    for (const LightmapMesh& mesh : meshes) {
        putString(out, mesh.key);
        putString(out, mesh.source);
        put(out, static_cast<uint32_t>(mesh.submeshes.size()));
        for (const LightmapSubmesh& sub : mesh.submeshes) {
            put(out, sub.sourceVertices);
            put(out, static_cast<uint32_t>(sub.xref.size()));
            putArray(out, sub.xref);
            putArray(out, sub.uv);
            put(out, static_cast<uint32_t>(sub.indices.size()));
            putArray(out, sub.indices);
        }
    }
    return out;
}

bool readLightmapMeshes(std::string_view bytes, std::vector<LightmapMesh>& out, std::string& error)
{
    Reader r{bytes};
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kMeshesMagic, 4) != 0) {
        error = "meshes.klmm is not a lightmap meshes file";
        return false;
    }
    r.at = 4;
    if (r.pod<uint32_t>() != kLightmapFormatVersion) {
        error = std::format("meshes.klmm is not version {}", kLightmapFormatVersion);
        return false;
    }
    std::vector<LightmapMesh> meshes;
    const uint32_t count = r.pod<uint32_t>();
    for (uint32_t m = 0; r.ok && m < count; ++m) {
        LightmapMesh mesh;
        mesh.key = r.string();
        mesh.source = r.string();
        const uint32_t submeshes = r.pod<uint32_t>();
        for (uint32_t s = 0; r.ok && s < submeshes; ++s) {
            LightmapSubmesh sub;
            sub.sourceVertices = r.pod<uint32_t>();
            const uint32_t vertices = r.pod<uint32_t>();
            r.array(sub.xref, vertices);
            r.array(sub.uv, size_t(vertices) * 2);
            r.array(sub.indices, r.pod<uint32_t>());
            if (r.ok && (sub.indices.size() % 3 != 0 ||
                         std::ranges::any_of(sub.indices, [&](uint32_t i) { return i >= vertices; }) ||
                         std::ranges::any_of(sub.xref, [&](uint32_t i) { return i >= sub.sourceVertices; }))) {
                error = "meshes.klmm has an index out of range";
                return false;
            }
            mesh.submeshes.push_back(std::move(sub));
        }
        meshes.push_back(std::move(mesh));
    }
    if (!r.ok || r.at != bytes.size()) {
        error = r.ok ? "meshes.klmm has bytes past its end" : "meshes.klmm is cut short";
        return false;
    }
    out = std::move(meshes);
    return true;
}

std::string lightmapSidecarJson(const LightmapSidecar& sidecar)
{
    Json json;
    json["version"] = kLightmapFormatVersion;
    json["hash"] = sidecar.hash;
    json["atlases"] = Json::array();
    for (const LightmapAtlas& atlas : sidecar.atlases)
        json["atlases"].push_back({{"size", {atlas.width, atlas.height}},
                                   {"irradiance", atlas.irradiance},
                                   {"direction", atlas.direction},
                                   {"irradianceHash", hex(atlas.irradianceHash)},
                                   {"directionHash", hex(atlas.directionHash)}});
    json["meshes"] = {{"file", sidecar.meshes}, {"hash", hex(sidecar.meshesHash)}};
    json["entities"] = Json::array();
    for (const LightmapEntity& entity : sidecar.entities)
        json["entities"].push_back({{"id", entity.id},
                                    {"mesh", entity.mesh},
                                    {"atlas", entity.atlas},
                                    {"scaleOffset", entity.scaleOffset}});
    const ProbeGrid& grid = sidecar.grid;
    Json probes = Json::array();
    for (size_t i = 0; i < grid.sh.size(); ++i) {
        // ponytail: probes inline in the json (8192 at most); a binary file beside it if grids must get bigger.
        Json sh = Json::array();
        for (float v : grid.sh[i])
            sh.push_back(v);
        probes.push_back({{"valid", grid.valid[i] != 0}, {"sh", std::move(sh)}});
    }
    json["probeGrid"] = {{"origin", {grid.origin.x, grid.origin.y, grid.origin.z}},
                         {"spacing", grid.spacing},
                         {"counts", grid.counts},
                         {"probes", std::move(probes)}};
    return json.dump() + "\n"; // compact: the probe grid makes it long
}

bool parseLightmapSidecar(std::string_view text, LightmapSidecar& out, std::string& error)
{
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer()) {
        error = "lightmaps.json is not a lightmap sidecar";
        return false;
    }
    if (json["version"].get<int>() != kLightmapFormatVersion) {
        error = std::format("lightmaps.json is version {}, expected {} (bake again)", json["version"].dump(),
                            kLightmapFormatVersion);
        return false;
    }
    const auto malformed = [&](const char* what) {
        error = std::format("lightmaps.json has a malformed \"{}\"", what);
        return false;
    };
    LightmapSidecar s;
    if (!json.contains("hash") || !json["hash"].is_string())
        return malformed("hash");
    s.hash = json["hash"].get<std::string>();
    if (!json.contains("atlases") || !json["atlases"].is_array())
        return malformed("atlases");
    for (const Json& a : json["atlases"]) {
        LightmapAtlas atlas;
        if (!a.is_object() || !a.contains("size") || !a["size"].is_array() || a["size"].size() != 2 ||
            !a["size"][0].is_number_unsigned() || !a["size"][1].is_number_unsigned() ||
            a["size"][0].get<unsigned>() - 1 >= kLightmapAtlasSize || a["size"][1].get<unsigned>() - 1 >= kLightmapAtlasSize ||
            !a.contains("irradiance") || !a["irradiance"].is_string() || !a.contains("direction") ||
            !a["direction"].is_string() || !a.contains("irradianceHash") || !parseHex(a["irradianceHash"], atlas.irradianceHash) ||
            !a.contains("directionHash") || !parseHex(a["directionHash"], atlas.directionHash))
            return malformed("atlases");
        atlas.width = a["size"][0].get<uint16_t>();
        atlas.height = a["size"][1].get<uint16_t>();
        atlas.irradiance = a["irradiance"].get<std::string>();
        atlas.direction = a["direction"].get<std::string>();
        // File names beside the json only: never a path out of the folder.
        for (const std::string& name : {atlas.irradiance, atlas.direction})
            if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.starts_with("."))
                return malformed("atlases");
        s.atlases.push_back(std::move(atlas));
    }
    const Json& meshes = json.contains("meshes") ? json["meshes"] : Json();
    if (!meshes.is_object() || !meshes.contains("file") || !meshes["file"].is_string() || !meshes.contains("hash") ||
        !parseHex(meshes["hash"], s.meshesHash))
        return malformed("meshes");
    s.meshes = meshes["file"].get<std::string>();
    if (s.meshes.empty() || s.meshes.find_first_of("/\\:") != std::string::npos || s.meshes.starts_with("."))
        return malformed("meshes");
    if (!json.contains("entities") || !json["entities"].is_array())
        return malformed("entities");
    for (const Json& e : json["entities"]) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_number_unsigned() || !e.contains("mesh") ||
            !e["mesh"].is_string() || !e.contains("atlas") || !e["atlas"].is_number_unsigned() ||
            e["atlas"].get<size_t>() >= s.atlases.size() || !e.contains("scaleOffset") || !e["scaleOffset"].is_array() ||
            e["scaleOffset"].size() != 4 || !std::ranges::all_of(e["scaleOffset"], finite))
            return malformed("entities");
        LightmapEntity entity{.id = e["id"].get<EntityId>(), .mesh = e["mesh"].get<std::string>(), .atlas = e["atlas"].get<uint32_t>()};
        for (size_t i = 0; i < 4; ++i)
            entity.scaleOffset[i] = e["scaleOffset"][i].get<float>();
        s.entities.push_back(std::move(entity));
    }
    const Json& g = json.contains("probeGrid") ? json["probeGrid"] : Json();
    if (!g.is_object() || !g.contains("origin") || !g["origin"].is_array() || g["origin"].size() != 3 ||
        !std::ranges::all_of(g["origin"], finite) || !g.contains("spacing") || !finite(g["spacing"]) ||
        !(g["spacing"].get<float>() > 0.0f) || !g.contains("counts") || !g["counts"].is_array() || g["counts"].size() != 3 ||
        !std::ranges::all_of(g["counts"], [](const Json& c) { return c.is_number_unsigned() && c.get<size_t>() <= kMaxGridProbes; }) ||
        !g.contains("probes") || !g["probes"].is_array())
        return malformed("probeGrid");
    s.grid.origin = {g["origin"][0].get<float>(), g["origin"][1].get<float>(), g["origin"][2].get<float>()};
    s.grid.spacing = g["spacing"].get<float>();
    for (int a = 0; a < 3; ++a)
        s.grid.counts[a] = g["counts"][a].get<int>();
    const size_t probes = size_t(s.grid.counts[0]) * s.grid.counts[1] * s.grid.counts[2];
    if (g["probes"].size() != probes || probes > kMaxGridProbes)
        return malformed("probeGrid");
    for (const Json& p : g["probes"]) {
        if (!p.is_object() || !p.contains("valid") || !p["valid"].is_boolean() || !p.contains("sh") || !p["sh"].is_array() ||
            p["sh"].size() != 27 || !std::ranges::all_of(p["sh"], finite))
            return malformed("probeGrid");
        Sh9 sh;
        for (size_t i = 0; i < 27; ++i)
            sh[i] = p["sh"][i].get<float>();
        s.grid.sh.push_back(sh);
        s.grid.valid.push_back(p["valid"].get<bool>() ? 1 : 0);
    }
    out = std::move(s);
    return true;
}

std::string terrainLightmapsJson(const TerrainLightmaps& index)
{
    Json json;
    json["version"] = kLightmapFormatVersion;
    json["hash"] = index.hash;
    json["size"] = index.size;
    json["tiles"] = Json::array();
    for (const TerrainLightmaps::Tile& tile : index.tiles)
        json["tiles"].push_back({{"tile", {tile.tx, tile.tz}},
                                 {"irradianceHash", hex(tile.irradianceHash)},
                                 {"directionHash", hex(tile.directionHash)}});
    return json.dump(2) + "\n";
}

bool parseTerrainLightmaps(std::string_view text, TerrainLightmaps& out, std::string& error)
{
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer()) {
        error = "lightmaps.json is not a terrain lightmap index";
        return false;
    }
    if (json["version"].get<int>() != kLightmapFormatVersion) {
        error = std::format("lightmaps.json is version {}, expected {} (bake again)", json["version"].dump(),
                            kLightmapFormatVersion);
        return false;
    }
    TerrainLightmaps index;
    if (!json.contains("hash") || !json["hash"].is_string() || !json.contains("size") || !json["size"].is_number_unsigned() ||
        json["size"].get<unsigned>() - 1 >= kLightmapAtlasSize || !json.contains("tiles") || !json["tiles"].is_array()) {
        error = "lightmaps.json is malformed";
        return false;
    }
    index.hash = json["hash"].get<std::string>();
    index.size = json["size"].get<int>();
    for (const Json& t : json["tiles"]) {
        TerrainLightmaps::Tile tile;
        if (!t.is_object() || !t.contains("tile") || !t["tile"].is_array() || t["tile"].size() != 2 ||
            !t["tile"][0].is_number_integer() || !t["tile"][1].is_number_integer() || !t.contains("irradianceHash") ||
            !parseHex(t["irradianceHash"], tile.irradianceHash) || !t.contains("directionHash") ||
            !parseHex(t["directionHash"], tile.directionHash)) {
            error = "lightmaps.json has a malformed tile";
            return false;
        }
        tile.tx = t["tile"][0].get<int>();
        tile.tz = t["tile"][1].get<int>();
        index.tiles.push_back(tile);
    }
    out = std::move(index);
    return true;
}

bool checkTerrainLightmap(const TerrainLightmaps& index, int tx, int tz, std::string_view irradiance,
                          std::string_view direction, std::string& error)
{
    const auto tile = std::ranges::find_if(index.tiles, [&](const TerrainLightmaps::Tile& t) { return t.tx == tx && t.tz == tz; });
    if (tile == index.tiles.end()) {
        error = "not baked (the map's lightmaps.json doesn't list it)";
        return false;
    }
    if (lightmapFileHash(irradiance) != tile->irradianceHash || lightmapFileHash(direction) != tile->directionHash) {
        error = "its files don't match the map's lightmaps.json (bake again)";
        return false;
    }
    const auto size = static_cast<uint16_t>(index.size);
    return checkLightmapKtx(irradiance, size, size, error) && checkLightmapKtx(direction, size, size, error);
}

std::string lightmapSidecarPath(const std::string& scenePath)
{
    const std::filesystem::path scene(reinterpret_cast<const char8_t*>(scenePath.c_str()));
    const std::filesystem::path file = scene.parent_path() / (scene.stem().u8string() + u8"_lightmaps") / u8"lightmaps.json";
    const std::u8string text = file.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string terrainLightmapFolder(const std::string& mapFolder) { return mapFolder + "/lightmaps"; }

std::string terrainLightmapFile(int tx, int tz, bool direction)
{
    return std::format("{}_{}{}.ktx", tx, tz, direction ? "_dir" : "");
}

uint64_t lightmapFileHash(std::string_view bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (char c : bytes) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string lightmapKtx(uint16_t width, uint16_t height, const std::vector<float>& rgba)
{
    std::vector<uint16_t> halves(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i)
        halves[i] = bx::halfFromFloat(rgba[i]);
    std::string ktx;
    StringWriter writer(ktx);
    bx::Error err;
    bimg::imageWriteKtx(&writer, bimg::TextureFormat::RGBA16F, false, width, height, 1, 1, 1, false, halves.data(), &err);
    return ktx;
}

bool checkLightmapKtx(std::string_view bytes, uint16_t width, uint16_t height, std::string& error)
{
    const std::optional<GpuImageInfo> info = parseGpuImage(bytes, false, error);
    if (!info)
        return false;
    if (info->format != bgfx::TextureFormat::RGBA16F || info->width != width || info->height != height || info->mips != 1) {
        error = std::format("not an RGBA16F {} x {} lightmap (bake again)", width, height);
        return false;
    }
    return true;
}

std::string lightmapSceneHash(const Scene& scene)
{
    const entt::registry& registry = scene.registry();
    std::map<EntityId, std::string> parts; // by id: the order entities were created in doesn't count
    const auto matrix = [&](entt::entity entity) {
        float world[16];
        scene.worldMatrix(entity, world);
        std::string s;
        for (float v : world)
            s += std::format("{:.5g},", v);
        return s;
    };
    const auto id = [&](entt::entity entity) { return registry.get<PersistentId>(entity).value; };
    for (auto [entity, mesh] : registry.view<MeshRenderer>().each())
        if (mesh.giMode == GiMode::Static)
            parts[id(entity)] += std::format("mesh {} {} {} {}|", int(mesh.primitive), mesh.material, mesh.lightmapScale,
                                             matrix(entity));
    for (auto [entity, model] : registry.view<ModelRenderer>().each()) {
        if (model.giMode != GiMode::Static)
            continue;
        std::string materials;
        for (const std::string& m : model.materials)
            materials += m + ";";
        parts[id(entity)] += std::format("model {} {} {} {}|", model.model, materials, model.lightmapScale, matrix(entity));
    }
    const auto color = [](const bx::Vec3& c) { return std::format("{:.5g},{:.5g},{:.5g}", c.x, c.y, c.z); };
    for (auto [entity, light] : registry.view<DirectionalLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime)
            parts[id(entity)] += std::format("sun {} {} {} {}|", int(light.bakeMode), color(light.color), light.intensity,
                                             matrix(entity));
    for (auto [entity, light] : registry.view<PointLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime)
            parts[id(entity)] += std::format("point {} {} {} {} {}|", int(light.bakeMode), color(light.color),
                                             light.intensity, light.range, matrix(entity));
    for (auto [entity, light] : registry.view<SpotLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime)
            parts[id(entity)] += std::format("spot {} {} {} {} {} {} {}|", int(light.bakeMode), color(light.color),
                                             light.intensity, light.range, light.innerAngle, light.outerAngle,
                                             matrix(entity));
    for (auto [entity, light] : registry.view<AreaLight>().each())
        if (light.bakeMode != LightBakeMode::Realtime)
            parts[id(entity)] += std::format("area {} {} {} {} {} {} {} {}|", int(light.bakeMode), color(light.color),
                                             light.intensity, light.width, light.height, light.range, light.twoSided,
                                             matrix(entity));
    for (auto [entity, terrain] : registry.view<Terrain>().each())
        if (terrain.lightmap)
            parts[id(entity)] += std::format("terrain {}|", terrain.map);
    std::string all;
    for (const auto& [key, text] : parts)
        all += std::format("{}:{}\n", key, text);
    const SceneLighting& lighting = scene.lighting;
    const SceneSky& sky = scene.sky;
    const SceneLightmaps& lm = lighting.lightmaps;
    all += std::format("ambient {} {}\nsky {} {} {} {} {}", color(lighting.ambientColor), lighting.ambientIntensity,
                       int(sky.mode), color(sky.top), color(sky.horizon), color(sky.bottom), sky.cube);
    for (const std::string& face : sky.faces)
        all += "," + face;
    all += std::format("\nsettings {} {} {} {}", lm.texelsPerMeter, lm.bounces, lm.samplesPerTexel, lm.probeSpacing);
    return hex(lightmapFileHash(all));
}

} // namespace kast
