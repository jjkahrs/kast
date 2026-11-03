// The Map tab's Terrain sub-tab and the sculpt brushes' Scene view input and hotkeys:
// the Raise/Lower/Flatten tools with their settings, the height range (applied as edited; the clip prompt), heightmap PNG import/export, Generate
// (fBm), Height colors and Create texture & material. The math is mapedit/terrain_edit.h.
#include "map_panel.h"

#include "ui_common.h"

#include <files.h>
#include <log.h>
#include <scene_io.h> // jsonNumber

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal

#include <algorithm>
#include <charconv>
#include <format>
#include <random>

namespace {

namespace km = kast::map;
namespace fs = std::filesystem;

constexpr const char* kClipPopup = "Clip heights?";
constexpr SDL_DialogFileFilter kPngFilter{"16-bit grayscale PNG", "png"};

std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

constexpr float kMinRadius = 1.0f, kMaxRadius = 128.0f;     // metres
constexpr float kMinStrength = 0.1f, kMaxStrength = 50.0f;
constexpr float kBrushStep = 1.25f; // a [ ] or - = press scales radius or strength by this
struct SculptKey {
    ImGuiKey key;
    MapPanel::Tool tool;
    const char* name;
};
constexpr SculptKey kSculptKeys[] = {{ImGuiKey_1, MapPanel::Tool::Raise, "Raise"},
                                     {ImGuiKey_2, MapPanel::Tool::Lower, "Lower"},
                                     {ImGuiKey_3, MapPanel::Tool::Flatten, "Flatten"}};

// Why no brush can change the heights (apply_brush does nothing), or null.
const char* sculptBlocker(const km::MapInfo& info)
{
    const auto range = km::height_range(info);
    return range[1] > range[0] ? nullptr : "the height range is empty: set Max above Min below";
}

// down / up pressed (repeating while held): 1 / kBrushStep or kBrushStep; else 1.
float brushStep(ImGuiKey down, ImGuiKey up)
{
    return ImGui::IsKeyPressed(up) ? kBrushStep : ImGui::IsKeyPressed(down) ? 1.0f / kBrushStep : 1.0f;
}

const char* brushName(mapedit::BrushMode mode)
{
    return mode == mapedit::BrushMode::Raise ? "Raise" : mode == mapedit::BrushMode::Lower ? "Lower" : "Flatten";
}

} // namespace

void MapPanel::drawTerrain(MapSession& session)
{
    mapedit::MapDoc* z = session.doc();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (!info) {
        ImGui::TextDisabled(z ? "map.json is invalid (see Map)" : "No map open");
        return;
    }

    ImGui::SeparatorText("Brush");
    toolButton("Raise", Tool::Raise);
    toolButton("Lower", Tool::Lower);
    toolButton("Flatten", Tool::Flatten);
    ImGui::NewLine();
    const char* why = sculptBlocker(*info);
    ImGui::PushStyleColor(ImGuiCol_Text, why ? kErrorColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (why)
        ImGui::TextWrapped("Can't sculpt: %s", why);
    else
        ImGui::TextWrapped("Left drag in the Scene view sculpts. Keys there: 1 2 3 tool, [ ] radius, - = strength");
    ImGui::PopStyleColor();
    ImGui::SliderFloat("Radius", &brush_.radius, kMinRadius, kMaxRadius, "%.0f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Strength", &strength_, kMinStrength, kMaxStrength, tool_ == Tool::Flatten ? "%.1f /s" : "%.1f m/s",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Falloff", &brush_.falloff, 0.0f, 1.0f, "%.2f");
    ImGui::InputFloat("Flatten to", &brush_.target, 0.0f, 0.0f, "%.2f m");
    ImGui::TextDisabled("Flatten: Shift+click the terrain to pick the height");

    // Applied as each field is left (Enter, Tab, a click elsewhere); a range that would clip heights asks first.
    ImGui::SeparatorText("Height range");
    const auto range = km::height_range(*info);
    if (!ImGui::IsAnyItemActive() && !askRange_ && !rangeEdited_) {
        rangeMin_ = range[0];
        rangeMax_ = range[1];
    }
    for (auto [label, value] : {std::pair{"Min", &rangeMin_}, std::pair{"Max", &rangeMax_}}) {
        if (ImGui::InputFloat(label, value, 0.0f, 0.0f, "%.2f m"))
            rangeEdited_ = true;
        if (ImGui::IsItemDeactivatedAfterEdit())
            editRange(*z, rangeMin_, rangeMax_);
    }
    if (rangeEdited_ && !(rangeMin_ < rangeMax_))
        ImGui::TextColored(kErrorColor, "Max must be above Min");

    ImGui::SeparatorText("Heightmap");
    if (!info->has_tile(tile_))
        tile_ = {info->tiles[0].x0, info->tiles[0].z0};
    ImGui::Text("Tile %s (click the terrain to choose)", mapedit::tile_name(tile_).c_str());
    ImGui::BeginDisabled(pngDialogOpen_);
    // ponytail: no parent window, as the Systems panel's script dialog: the dialog is not modal to the editor.
    if (ImGui::Button("Import PNG...")) {
        pngDialogOpen_ = true;
        pngExport_ = false;
        SDL_ShowOpenFileDialog(onPngChosen, this, nullptr, &kPngFilter, 1, nullptr, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export PNG...")) {
        pngDialogOpen_ = true;
        pngExport_ = true;
        SDL_ShowSaveFileDialog(onPngChosen, this, nullptr, &kPngFilter, 1, nullptr);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("257 x 257, 16-bit grayscale: 0 = height min, 65535 = max");

    drawGenerate(*z, *info);
    drawTerrainColors(*z);
}

void MapPanel::drawGenerate(mapedit::MapDoc& z, const km::MapInfo& info)
{
    ImGui::SeparatorText("Generate");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &noise_.seed);
    ImGui::SameLine();
    if (ImGui::Button("Randomize"))
        noise_.seed = std::random_device{}();
    ImGui::SliderFloat("Scale", &noise_.scale, 16.0f, 4096.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt("Octaves", &noise_.octaves, 1, 16);
    ImGui::SliderFloat("Persistence", &noise_.persistence, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Lacunarity", &noise_.lacunarity, 1.0f, 4.0f, "%.2f");
    const auto range = km::height_range(info);
    ImGui::BeginDisabled(range[1] <= range[0]);
    if (ImGui::Button("Generate heights")) {
        z.edit([&](mapedit::MapState& s) {
            mapedit::generate_heights(info, s, noise_);
            // The spawn stays on the ground (it is on the map: the manifest parsed).
            s.manifest["spawn"]["pos"][1] = kast::jsonNumber(*mapedit::ground_y(info, s, info.spawn[0], info.spawn[2]));
        });
        kast::log::info(std::format("Map: generated heights (seed {}, scale {} m, {} octaves)", noise_.seed,
                                    noise_.scale, noise_.octaves));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Replaces every tile's heights, over the whole height range");
}

void MapPanel::drawTerrainColors(mapedit::MapDoc& z)
{
    ImGui::SeparatorText("Height colors");
    std::string err;
    // Edits go to the manifest as they happen and are one undo step from the first change until no widget is active
    // (a drag, a color picker).
    if (!colorsEditing_ && !ImGui::IsAnyItemActive())
        gradient_ = mapedit::parse_gradient(z.state.manifest, err).value_or(mapedit::default_gradient());
    bool changed = false;
    std::optional<size_t> remove;
    for (size_t i = 0; i < gradient_.size(); ++i) {
        ImGui::PushID(int(i));
        float pct = gradient_[i].pos * 100.0f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::DragFloat("##pos", &pct, 0.25f, 0.0f, 100.0f, "%.1f %%", ImGuiSliderFlags_AlwaysClamp)) {
            gradient_[i].pos = pct / 100.0f;
            changed = true;
        }
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("##color", gradient_[i].rgb.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::BeginDisabled(gradient_.size() <= 2);
        if (ImGui::Button("Remove"))
            remove = i;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (changed) {
        if (!colorsEditing_)
            z.begin();
        colorsEditing_ = true;
        z.state.manifest["terrainColors"] = mapedit::gradient_json(gradient_);
    }
    if (colorsEditing_ && !ImGui::IsAnyItemActive()) {
        z.commit();
        colorsEditing_ = false;
    }
    if (remove && !colorsEditing_) {
        gradient_.erase(gradient_.begin() + std::ptrdiff_t(*remove));
        z.edit([&](mapedit::MapState& s) { s.manifest["terrainColors"] = mapedit::gradient_json(gradient_); });
    }
    if (ImGui::Button("Add stop") && !colorsEditing_) {
        // Halfway between the two highest stops, in the color already there: the look does not change until edited.
        std::vector<float> pos;
        for (const mapedit::ColorStop& c : gradient_)
            pos.push_back(c.pos);
        std::ranges::sort(pos);
        const float at = (pos[pos.size() - 2] + pos.back()) / 2.0f;
        gradient_.push_back({at, mapedit::gradient_color(gradient_, at)});
        z.edit([&](mapedit::MapState& s) { s.manifest["terrainColors"] = mapedit::gradient_json(gradient_); });
    }
    ImGui::TextDisabled("Position: %% of the height range (0 = min, 100 = max)");

    ImGui::SeparatorText("Terrain texture");
    const std::string material = mapedit::terrain_material_path(z.folder);
    if (ImGui::Button("Create texture & material")) {
        // Assigns the generated material with "terrainUv": "map", adding the default colors if there are none; save
        // writes (and every later save re-bakes) the texture and material.
        const bool hasColors = mapedit::parse_gradient(z.state.manifest, err).has_value();
        z.edit([&](mapedit::MapState& s) {
            if (!hasColors)
                s.manifest["terrainColors"] = mapedit::gradient_json(mapedit::default_gradient());
            s.manifest["terrainUv"] = "map";
            s.manifest["material"] = material;
        });
        kast::log::info(std::format("Map: terrain material {} and {} assigned; written on save", material,
                                    mapedit::terrain_texture_path(z.folder)));
    }
    if (z.state.manifest.value("material", std::string()) == material)
        ImGui::TextWrapped("Uses %s and %s, written on every save", material.c_str(),
                           mapedit::terrain_texture_path(z.folder).c_str());
    else
        ImGui::TextDisabled("Bakes these colors into a texture spanning the map");
}

void MapPanel::setHeightRange(mapedit::MapDoc& z, float min, float max)
{
    std::string err;
    const auto info = z.info(err);
    if (!info || !(min <= max)) {
        kast::log::error("Map: height range: min must be <= max");
        return;
    }
    size_t clipped = 0;
    z.edit([&](mapedit::MapState& s) { clipped = mapedit::set_height_range(*info, s, min, max); });
    kast::log::info(std::format("Map: height range now {} .. {} m{}", min, max,
                                clipped ? std::format(" ({} samples clipped)", clipped) : ""));
}

void MapPanel::editRange(mapedit::MapDoc& z, float min, float max)
{
    std::string err;
    const auto info = z.info(err);
    rangeMin_ = min;
    rangeMax_ = max;
    rangeEdited_ = true;
    if (!info || !(min < max)) // the tab says why
        return;
    const auto range = km::height_range(*info);
    if (min == range[0] && max == range[1]) {
        rangeEdited_ = false;
        return;
    }
    rangeClips_ = mapedit::count_clipped(*info, z.state, min, max);
    if (rangeClips_ > 0) {
        askRange_ = true;
        kast::log::info(std::format("Map: height range {} .. {} m would clip {} samples: asking first", min, max,
                                    rangeClips_));
        return;
    }
    setHeightRange(z, min, max);
    rangeEdited_ = false;
}

bool MapPanel::editRange(MapSession& session, std::string_view spec)
{
    const size_t comma = spec.find(',');
    float min = 0, max = 0;
    const auto number = [](std::string_view text, float& out) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && end == text.data() + text.size();
    };
    if (comma == std::string_view::npos || !number(spec.substr(0, comma), min) || !number(spec.substr(comma + 1), max))
        return false;
    if (mapedit::MapDoc* z = session.doc())
        editRange(*z, min, max);
    return true;
}

void MapPanel::sculptHotkeys(bool sceneActive)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool sculpting = tool_ == Tool::Raise || tool_ == Tool::Lower || tool_ == Tool::Flatten;
    if (!sceneActive || !(sculpting || (tab_ == Tab::Terrain && tabFront_)) || io.WantTextInput || io.KeyCtrl ||
        io.KeyAlt || ImGui::GetTopMostPopupModal())
        return;
    for (const SculptKey& k : kSculptKeys)
        if (ImGui::IsKeyPressed(k.key, false) && tool_ != k.tool) {
            tool_ = k.tool;
            kast::log::info(std::format("Map: {} tool (key {})", k.name, ImGui::GetKeyName(k.key)));
        }
    if (const float f = brushStep(ImGuiKey_LeftBracket, ImGuiKey_RightBracket); f != 1.0f) {
        brush_.radius = std::clamp(brush_.radius * f, kMinRadius, kMaxRadius);
        kast::log::info(std::format("Map: brush radius {:.0f} m", brush_.radius));
    }
    if (const float f = brushStep(ImGuiKey_Minus, ImGuiKey_Equal); f != 1.0f) {
        strength_ = std::clamp(strength_ * f, kMinStrength, kMaxStrength);
        kast::log::info(std::format("Map: brush strength {:.1f}", strength_));
    }
}

void MapPanel::drawHeightRangePrompt(MapSession& session)
{
    if (askRange_ && !ImGui::IsPopupOpen(kClipPopup))
        ImGui::OpenPopup(kClipPopup);
    if (!ImGui::BeginPopupModal(kClipPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("%zu samples are outside %.2f .. %.2f m and will be clipped to it.", rangeClips_, rangeMin_, rangeMax_);
    if (ImGui::Button("Apply")) {
        if (mapedit::MapDoc* z = session.doc())
            setHeightRange(*z, rangeMin_, rangeMax_);
        askRange_ = rangeEdited_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        askRange_ = rangeEdited_ = false; // the fields go back to the map's range
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapPanel::onPngChosen(void* userdata, const char* const* files, int /*filter*/)
{
    auto* self = static_cast<MapPanel*>(userdata);
    std::lock_guard lock(self->pngMutex_);
    self->pngChosen_ = true;
    self->pngPath_ = files && files[0] ? files[0] : "";
    if (!files)
        kast::log::error(std::format("File dialog failed: {}", SDL_GetError()));
}

void MapPanel::pollPngDialog(MapSession& session)
{
    std::string path;
    {
        std::lock_guard lock(pngMutex_);
        if (!std::exchange(pngChosen_, false))
            return;
        path = std::move(pngPath_);
    }
    pngDialogOpen_ = false;
    mapedit::MapDoc* z = session.doc();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (path.empty() || !info) // cancelled, or the map went away (Play, another scene) while the dialog was open
        return;
    fs::path file(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    const std::string tile = mapedit::tile_name(tile_);
    if (!info->has_tile(tile_)) {
        kast::log::error(std::format("Map: heightmap PNG: tile {} is not on the map", tile));
        return;
    }
    if (pngExport_) {
        if (file.extension() != ".png")
            file += ".png";
        if (kast::writeFileAtomic(file, mapedit::encode_png16(mapedit::tile_heights(z->state, tile_))))
            kast::log::info(std::format("Map: exported tile {} to {}", tile, utf8(file)));
        else
            kast::log::error("Map: export PNG: cannot write " + utf8(file));
        return;
    }
    std::string bytes;
    bool ok = kast::readFile(file, bytes);
    if (!ok)
        err = "cannot read the file";
    else
        z->edit([&](mapedit::MapState& s) { ok = mapedit::import_png16(*info, s, tile_, bytes, err); });
    if (ok)
        kast::log::info(std::format("Map: imported {} into tile {}", utf8(file), tile));
    else
        kast::log::error(std::format("Map: import PNG {}: {}", utf8(file), err));
}

void MapPanel::sculptInput(mapedit::MapDoc& z, const km::MapInfo& info, bool click)
{
    const ImGuiIO& io = ImGui::GetIO();
    brush_.mode = tool_ == Tool::Raise   ? mapedit::BrushMode::Raise
                  : tool_ == Tool::Lower ? mapedit::BrushMode::Lower
                                         : mapedit::BrushMode::Flatten;
    if (click && hover_ && io.KeyShift && tool_ == Tool::Flatten) {
        brush_.target = (*hover_)[1];
        return;
    }
    if (const char* why = sculptBlocker(info); click && why) {
        kast::log::warn(std::string("Map: can't sculpt: ") + why);
        return;
    }
    if (click && hover_ && !stroking_) {
        z.begin(); // the whole stroke is one undo step (endStroke commits it)
        stroking_ = true;
    }
    if (stroking_ && hover_ && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        mapedit::apply_brush(info, z.state, (*hover_)[0], (*hover_)[2], brush_, strength_ * io.DeltaTime);
}

void MapPanel::endStroke(MapSession& session)
{
    const bool down = session.doc() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    mapedit::MapDoc* z = session.doc();
    if (drag_.kind != Drag::Kind::None && !(down && tool_ == Tool::None)) {
        drag_ = {};
        if (z && z->editing()) {
            z->commit();
            kast::log::info("Map: moved " + dragLog_);
        }
    }
    const bool brush = scattering_ ? tool_ == Tool::Scatter
                                   : tool_ == Tool::Raise || tool_ == Tool::Lower || tool_ == Tool::Flatten;
    if (!stroking_ || (brush && down))
        return;
    stroking_ = false;
    if (z && z->editing()) {
        z->commit();
        if (scattering_)
            kast::log::info(std::format("Map: {} stroke ({} visual(s))", scatterErase_ ? "Erase" : "Scatter",
                                        strokeCount_));
        else
            kast::log::info(std::format("Map: {} stroke", brushName(brush_.mode)));
    }
    scattering_ = false;
}
