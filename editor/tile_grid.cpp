#include "tile_grid.h"

#include <algorithm>
#include <cmath>

namespace km = kast::map;

void TileGrid::fit(const std::string& key, const km::TileRect& bounds, int cols, int rows, float width, float maxCell)
{
    const zone::GridView fitted = zone::fit_grid(bounds, cols, rows, width, kGap, kMinCell, maxCell);
    const bool same = bounds.x0 == bounds_.x0 && bounds.z0 == bounds_.z0 && bounds.x1 == bounds_.x1 && bounds.z1 == bounds_.z1;
    const bool moved = view_.cell != fitted_.cell || view_.left != fitted_.left || view_.top != fitted_.top;
    if (key != key_ || width != width_ || maxCell != maxCell_ || (!same && !moved))
        view_ = fitted;
    key_ = key;
    bounds_ = bounds;
    width_ = width;
    maxCell_ = maxCell;
    fitted_ = fitted;
    size_ = zone::grid_size(fitted, cols, rows);
    if (look_)
        view_ = zone::centre_grid(fitted, look_->tile, look_->zoom, size_, fitted.cell, maxCell * kMaxZoom);
    look_.reset();
    view_ = zone::pan_grid(view_, {0, 0}, bounds_, size_); // a tile removed at the edge: no empty cells past it
}

TileGrid::Input TileGrid::area(const char* id)
{
    origin_ = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {size_[0], size_[1]},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        for (const ImGuiMouseButton b : {ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle})
            dragged_ = dragged_ || ImGui::IsMouseDragging(b);
        if (dragged_)
            view_ = zone::pan_grid(view_, {io.MouseDelta.x, io.MouseDelta.y}, bounds_, size_);
    }
    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // the wheel zooms the grid, not scrolls the window
        if (io.MouseWheel != 0.0f) {
            const std::array<float, 2> at{io.MousePos.x - origin_.x, io.MousePos.y - origin_.y};
            view_ = zone::zoom_grid(view_, std::pow(kZoomStep, io.MouseWheel), at, fitted_.cell, maxCell_ * kMaxZoom);
            view_ = zone::pan_grid(view_, {0, 0}, bounds_, size_);
        }
    }
    Input input;
    if (hovered || ImGui::IsItemActive())
        if (const auto t = zone::cell_at(view_, {io.MousePos.x - origin_.x, io.MousePos.y - origin_.y});
            t && t->x >= bounds_.x0 && t->x <= bounds_.x1 && t->z >= bounds_.z0 && t->z <= bounds_.z1)
            input.hover = t;
    input.clicked = pressed && !dragged_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && input.hover;
    if (!ImGui::IsItemActive())
        dragged_ = false;
    return input;
}

ImVec2 TileGrid::corner(zone::TileKey t) const
{
    const std::array<float, 2> p = zone::cell_corner(view_, t);
    return {origin_.x + p[0], origin_.y + p[1]};
}
