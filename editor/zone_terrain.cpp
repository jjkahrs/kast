// The Zone tab's Terrain section and the sculpt brushes' Scene view input:
// Raise/Lower/Flatten settings, the height range (with the clip prompt), heightmap PNG import/export, Generate
// (fBm), Height colors and Create texture & material. The math is zone/terrain_edit.h.
#include "zone_panel.h"

#include "ui_common.h"

#include <files.h>
#include <log.h>
#include <scene_io.h> // jsonNumber

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal

#include <algorithm>
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

const char* brushName(zone::BrushMode mode)
{
    return mode == zone::BrushMode::Raise ? "Raise" : mode == zone::BrushMode::Lower ? "Lower" : "Flatten";
}

} // namespace

void ZonePanel::drawTerrain(ZoneSession& zone)
{
    if (!header("Terrain", "Terrain"))
        return;
    zone::ZoneDoc* z = zone.zone();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (!info) {
        ImGui::TextDisabled(z ? "map.json is invalid (see Map)" : "No zone open");
        return;
    }

    ImGui::SeparatorText("Brush");
    ImGui::TextDisabled("Raise / Lower / Flatten at the top; left drag paints in the Scene view");
    ImGui::SliderFloat("Radius", &brush_.radius, 1.0f, 128.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Strength", &strength_, 0.1f, 50.0f, tool_ == Tool::Flatten ? "%.1f /s" : "%.1f m/s",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Falloff", &brush_.falloff, 0.0f, 1.0f, "%.2f");
    ImGui::InputFloat("Flatten to", &brush_.target, 0.0f, 0.0f, "%.2f m");
    ImGui::TextDisabled("Flatten: Shift+click the terrain to pick the height");

    ImGui::SeparatorText("Height range");
    const auto range = km::height_range(*info);
    if (!ImGui::IsAnyItemActive() && !askRange_ && !rangeEdited_) {
        rangeMin_ = range[0];
        rangeMax_ = range[1];
    }
    if (ImGui::InputFloat("Min", &rangeMin_, 0.0f, 0.0f, "%.2f m"))
        rangeEdited_ = true;
    if (ImGui::InputFloat("Max", &rangeMax_, 0.0f, 0.0f, "%.2f m"))
        rangeEdited_ = true;
    ImGui::BeginDisabled((rangeMin_ == range[0] && rangeMax_ == range[1]) || rangeMin_ > rangeMax_);
    if (ImGui::Button("Apply range")) {
        rangeClips_ = zone::count_clipped(*info, z->state, rangeMin_, rangeMax_);
        if (rangeClips_ == 0) {
            setHeightRange(*z, rangeMin_, rangeMax_);
            rangeEdited_ = false;
        } else {
            askRange_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        rangeEdited_ = false;
    if (range[1] <= range[0])
        ImGui::TextColored(kErrorColor, "Set a range (max > min) to sculpt");

    ImGui::SeparatorText("Heightmap");
    if (!info->has_tile(tile_))
        tile_ = {info->tiles[0].x0, info->tiles[0].z0};
    ImGui::Text("Tile %s (click the terrain to choose)", zone::tile_name(tile_).c_str());
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

void ZonePanel::drawGenerate(zone::ZoneDoc& z, const km::MapInfo& info)
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
        z.edit([&](zone::ZoneState& s) {
            zone::generate_heights(info, s, noise_);
            // The spawn stays on the ground (it is on the map: the manifest parsed).
            s.manifest["spawn"]["pos"][1] = kast::jsonNumber(*zone::ground_y(info, s, info.spawn[0], info.spawn[2]));
        });
        kast::log::info(std::format("Zone: generated heights (seed {}, scale {} m, {} octaves)", noise_.seed,
                                    noise_.scale, noise_.octaves));
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Replaces every tile's heights, over the whole height range");
}

void ZonePanel::drawTerrainColors(zone::ZoneDoc& z)
{
    ImGui::SeparatorText("Height colors");
    std::string err;
    // Edits go to the manifest as they happen and are one undo step from the first change until no widget is active
    // (a drag, a color picker).
    if (!colorsEditing_ && !ImGui::IsAnyItemActive())
        gradient_ = zone::parse_gradient(z.state.manifest, err).value_or(zone::default_gradient());
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
        z.state.manifest["terrainColors"] = zone::gradient_json(gradient_);
    }
    if (colorsEditing_ && !ImGui::IsAnyItemActive()) {
        z.commit();
        colorsEditing_ = false;
    }
    if (remove && !colorsEditing_) {
        gradient_.erase(gradient_.begin() + std::ptrdiff_t(*remove));
        z.edit([&](zone::ZoneState& s) { s.manifest["terrainColors"] = zone::gradient_json(gradient_); });
    }
    if (ImGui::Button("Add stop") && !colorsEditing_) {
        // Halfway between the two highest stops, in the color already there: the look does not change until edited.
        std::vector<float> pos;
        for (const zone::ColorStop& c : gradient_)
            pos.push_back(c.pos);
        std::ranges::sort(pos);
        const float at = (pos[pos.size() - 2] + pos.back()) / 2.0f;
        gradient_.push_back({at, zone::gradient_color(gradient_, at)});
        z.edit([&](zone::ZoneState& s) { s.manifest["terrainColors"] = zone::gradient_json(gradient_); });
    }
    ImGui::TextDisabled("Position: %% of the height range (0 = min, 100 = max)");

    ImGui::SeparatorText("Terrain texture");
    const std::string material = zone::terrain_material_path(z.folder);
    if (ImGui::Button("Create texture & material")) {
        // Assigns the generated material with "terrainUv": "map", adding the default colors if there are none; save
        // writes (and every later save re-bakes) the texture and material.
        const bool hasColors = zone::parse_gradient(z.state.manifest, err).has_value();
        z.edit([&](zone::ZoneState& s) {
            if (!hasColors)
                s.manifest["terrainColors"] = zone::gradient_json(zone::default_gradient());
            s.manifest["terrainUv"] = "map";
            s.manifest["material"] = material;
        });
        kast::log::info(std::format("Zone: terrain material {} and {} assigned; written on save", material,
                                    zone::terrain_texture_path(z.folder)));
    }
    if (z.state.manifest.value("material", std::string()) == material)
        ImGui::TextWrapped("Uses %s and %s, written on every save", material.c_str(),
                           zone::terrain_texture_path(z.folder).c_str());
    else
        ImGui::TextDisabled("Bakes these colors into a texture spanning the map");
}

void ZonePanel::setHeightRange(zone::ZoneDoc& z, float min, float max)
{
    std::string err;
    const auto info = z.info(err);
    if (!info || !(min <= max)) {
        kast::log::error("Zone: height range: min must be <= max");
        return;
    }
    size_t clipped = 0;
    z.edit([&](zone::ZoneState& s) { clipped = zone::set_height_range(*info, s, min, max); });
    kast::log::info(std::format("Zone: height range now {} .. {} m{}", min, max,
                                clipped ? std::format(" ({} samples clipped)", clipped) : ""));
}

void ZonePanel::drawHeightRangePrompt(ZoneSession& zone)
{
    if (askRange_ && !ImGui::IsPopupOpen(kClipPopup))
        ImGui::OpenPopup(kClipPopup);
    if (!ImGui::BeginPopupModal(kClipPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("%zu samples are outside %.2f .. %.2f m and will be clipped to it.", rangeClips_, rangeMin_, rangeMax_);
    if (ImGui::Button("Apply")) {
        if (zone::ZoneDoc* z = zone.zone())
            setHeightRange(*z, rangeMin_, rangeMax_);
        askRange_ = rangeEdited_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        askRange_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ZonePanel::onPngChosen(void* userdata, const char* const* files, int /*filter*/)
{
    auto* self = static_cast<ZonePanel*>(userdata);
    std::lock_guard lock(self->pngMutex_);
    self->pngChosen_ = true;
    self->pngPath_ = files && files[0] ? files[0] : "";
    if (!files)
        kast::log::error(std::format("File dialog failed: {}", SDL_GetError()));
}

void ZonePanel::pollPngDialog(ZoneSession& zone)
{
    std::string path;
    {
        std::lock_guard lock(pngMutex_);
        if (!std::exchange(pngChosen_, false))
            return;
        path = std::move(pngPath_);
    }
    pngDialogOpen_ = false;
    zone::ZoneDoc* z = zone.zone();
    std::string err;
    const auto info = z ? z->info(err) : std::nullopt;
    if (path.empty() || !info) // cancelled, or the zone went away (Play, another scene) while the dialog was open
        return;
    fs::path file(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    const std::string tile = zone::tile_name(tile_);
    if (!info->has_tile(tile_)) {
        kast::log::error(std::format("Zone: heightmap PNG: tile {} is not on the map", tile));
        return;
    }
    if (pngExport_) {
        if (file.extension() != ".png")
            file += ".png";
        if (kast::writeFileAtomic(file, zone::encode_png16(zone::tile_heights(z->state, tile_))))
            kast::log::info(std::format("Zone: exported tile {} to {}", tile, utf8(file)));
        else
            kast::log::error("Zone: export PNG: cannot write " + utf8(file));
        return;
    }
    std::string bytes;
    bool ok = kast::readFile(file, bytes);
    if (!ok)
        err = "cannot read the file";
    else
        z->edit([&](zone::ZoneState& s) { ok = zone::import_png16(*info, s, tile_, bytes, err); });
    if (ok)
        kast::log::info(std::format("Zone: imported {} into tile {}", utf8(file), tile));
    else
        kast::log::error(std::format("Zone: import PNG {}: {}", utf8(file), err));
}

void ZonePanel::sculptInput(zone::ZoneDoc& z, const km::MapInfo& info, bool click)
{
    const ImGuiIO& io = ImGui::GetIO();
    brush_.mode = tool_ == Tool::Raise   ? zone::BrushMode::Raise
                  : tool_ == Tool::Lower ? zone::BrushMode::Lower
                                         : zone::BrushMode::Flatten;
    if (click && hover_ && io.KeyShift && tool_ == Tool::Flatten) {
        brush_.target = (*hover_)[1];
        return;
    }
    if (click && hover_ && !stroking_) {
        z.begin(); // the whole stroke is one undo step (endStroke commits it)
        stroking_ = true;
    }
    if (stroking_ && hover_ && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        zone::apply_brush(info, z.state, (*hover_)[0], (*hover_)[2], brush_, strength_ * io.DeltaTime);
}

void ZonePanel::endStroke(ZoneSession& zone)
{
    const bool down = zone.zone() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    zone::ZoneDoc* z = zone.zone();
    if (drag_.kind != Drag::Kind::None && !(down && tool_ == Tool::None)) {
        drag_ = {};
        if (z && z->editing()) {
            z->commit();
            kast::log::info("Zone: moved " + dragLog_);
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
            kast::log::info(std::format("Zone: {} stroke ({} visual(s))", scatterErase_ ? "Erase" : "Scatter",
                                        strokeCount_));
        else
            kast::log::info(std::format("Zone: {} stroke", brushName(brush_.mode)));
    }
    scattering_ = false;
}
