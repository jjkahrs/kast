// The tile grid area the Map tab and the Zone tab share (zone::GridView): fitted to the map at first, the mouse wheel
// zooms about the mouse (out to the fitted view at most), a drag with any button pans (within the map), a left click
// without a drag picks a cell; --grid-view centres it.
#pragma once

#include <zone/terrain_edit.h>

#include <imgui.h>

#include <optional>
#include <string>

class TileGrid {
public:
    static constexpr float kGap = 2.0f;     // pixels between cells
    static constexpr float kMinCell = 2.0f; // pixels: a 64 x 64 map (66 cells in the Zone tab) whole in a narrow panel
    static constexpr float kZoomStep = 1.25f; // cell size per wheel notch
    static constexpr float kMaxZoom = 2.0f;   // cells zoom up to this many times the widest they fit (two-digit names)

    // Before area(): the view for bounds on an area cols x rows cells, as wide as fits width pixels (cells up to maxCell,
    // zoomed up to kMaxZoom times that).
    // Fits again when the map (key) or width changes, or bounds do while the view is not zoomed or panned.
    void fit(const std::string& key, const kast::map::TileRect& bounds, int cols, int rows, float width, float maxCell);
    // --grid-view: the next fit centres the view on tile t at zoom times the fitted cells.
    void look(zone::TileKey t, float zoom) { look_ = Look{t, zoom}; }
    struct Input {
        std::optional<zone::TileKey> hover; // the cell under the mouse (not in a gap)
        bool clicked = false;               // a left click on it, not the end of a drag
    };
    // The area as one item at the cursor; zooms and pans it.
    Input area(const char* id);

    const zone::GridView& view() const { return view_; }
    // The cells of the fitted bounds the area shows.
    kast::map::TileRect visible() const { return zone::visible_cells(view_, bounds_, size_); }
    ImVec2 min() const { return origin_; }
    ImVec2 max() const { return {origin_.x + size_[0], origin_.y + size_[1]}; }
    // Tile t's cell on screen: its top-left corner.
    ImVec2 corner(zone::TileKey t) const;

private:
    struct Look {
        zone::TileKey tile;
        float zoom;
    };
    std::string key_;
    kast::map::TileRect bounds_{};
    float width_ = 0, maxCell_ = 0;
    zone::GridView fitted_, view_;
    std::array<float, 2> size_{};
    ImVec2 origin_{};
    bool dragged_ = false; // the held button moved past the drag threshold: no click on release
    std::optional<Look> look_;
};
