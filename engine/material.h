#pragma once

#include <bx/math.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Materials: PBR metallic-roughness parameters (glTF semantics) in .mat files (JSON, see docs/engine/materials.html).
// Texture paths are project-relative. GPU-free: parsing, validation and the path cache; the renderer owns the textures.
namespace kast {

constexpr int kMaterialFileVersion = 1;

enum class AlphaMode { Opaque, Mask, Blend };

struct Material {
    std::array<float, 4> baseColor{0.8f, 0.8f, 0.8f, 1.0f}; // linear RGBA 0..1
    std::string baseColorTexture;                           // sRGB; empty = none
    float metallic = 0.0f;                                  // 0..1
    float roughness = 0.5f;                                 // 0..1
    std::string metallicRoughnessTexture;                   // linear; G = roughness, B = metallic (times the scalars)
    bool emissiveEnabled = false;
    bx::Vec3 emissive{0.0f, 0.0f, 0.0f};                    // linear RGB 0..1, times emissiveIntensity
    float emissiveIntensity = 1.0f;                         // >= 0; above 1 is HDR and can bloom
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f; // 0..1, mask only
};

// What the shader adds after lighting: emissive * emissiveIntensity, or black when emission is off.
bx::Vec3 emissiveRadiance(const Material& material);

// The material of a MeshRenderer with no material path: grey 0.8, dielectric, opaque.
const Material& defaultMaterial();
// What a missing or invalid .mat renders as: unlit magenta (black base, magenta emissive).
const Material& errorMaterial();

// Field names in file order (without "version").
const std::vector<std::string>& materialFieldNames();
// Sets one field from JSON, checked as in a .mat file. False + error ("\"metallic\" must be ...") leaves it unchanged.
bool setMaterialField(Material& material, std::string_view name, const nlohmann::ordered_json& value, std::string& error);
// One field as JSON (colors as arrays, alphaMode by name); nullopt if name is not a field.
std::optional<nlohmann::ordered_json> materialField(const Material& material, std::string_view name);

// Strict: needs "version" == kMaterialFileVersion; unknown keys and invalid values are errors; missing fields get their
// defaults. Replaces out only on success.
bool materialFromJson(const nlohmann::ordered_json& json, Material& out, std::string& error);
// Every field in file order, "version" first.
nlohmann::ordered_json materialToJson(const Material& material);
// Reads and parses a .mat file (disk or mounted pak).
bool loadMaterial(const std::filesystem::path& file, Material& out, std::string& error);

// A per-entity copy of its MeshRenderer's material, made by kast.setMaterialProperty in Play mode. Not registered as a
// component: never saved, never in the Inspector, gone when Stop restores the scene. Renders instead of the .mat.
struct MaterialInstance {
    Material material;
};

// Indices of centers sorted far to near from eye (ties keep list order): the draw order of blended meshes.
std::vector<size_t> backToFront(const std::vector<bx::Vec3>& centers, const bx::Vec3& eye);

// Logs message as a warning the first time it is seen; later identical messages are dropped. For per-frame lookups
// (missing materials and textures) that would otherwise log every frame.
void warnOnce(const std::string& message);

// .mat files by project-relative path, each parsed once and re-parsed when its file's modification time changes
// (disk only: under a pak nothing changes). A missing or invalid file gives errorMaterial() and one warning.
class MaterialLibrary {
public:
    // Forgets every cached material if root differs from the last call's.
    void setRoot(const std::filesystem::path& root);
    // Empty path: defaultMaterial().
    const Material& get(const std::string& path);
    // Re-parses materials whose file changed (or appeared / disappeared) since loaded.
    void reloadChanged();

private:
    struct Entry {
        Material material;
        std::optional<std::filesystem::file_time_type> mtime; // nullopt: no file on disk
    };
    void load(const std::string& path, Entry& entry);

    std::filesystem::path root_;
    std::unordered_map<std::string, Entry> entries_;
};

// The file's modification time, or nullopt if it doesn't exist on disk.
std::optional<std::filesystem::file_time_type> modifiedTime(const std::filesystem::path& file);

} // namespace kast
