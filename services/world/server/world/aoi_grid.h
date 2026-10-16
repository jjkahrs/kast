// Uniform grid for interest management: which ids are within view radius of a point (x/z plane).
#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace kast {

struct CellKey {
    int32_t cx = 0, cz = 0;
    bool operator==(const CellKey&) const = default;
};

class AoiGrid {
public:
    AoiGrid(float cellSize, float viewRadius) : cell_(cellSize), radius_(viewRadius) {}

    CellKey cell_of(float x, float z) const;
    void insert(uint32_t id, CellKey c);
    void remove(uint32_t id, CellKey c);
    // Ids within view radius of (x, z), sorted ascending; `self` is excluded. posOf(id, x, z) gives current positions.
    template <typename PosOf>
    std::vector<uint32_t> query(float x, float z, uint32_t self, PosOf&& posOf) const;

private:
    static uint64_t pack(CellKey c) { return (uint64_t(uint32_t(c.cx)) << 32) | uint32_t(c.cz); }
    float cell_, radius_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

// a and b sorted ascending. entered = b \ a, left = a \ b.
void diff_sorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, std::vector<uint32_t>& entered,
                 std::vector<uint32_t>& left);

// ids sorted ascending: adds id if absent / removes it if present, keeping the order.
inline void insert_sorted(std::vector<uint32_t>& ids, uint32_t id) {
    if (auto it = std::lower_bound(ids.begin(), ids.end(), id); it == ids.end() || *it != id) ids.insert(it, id);
}
inline void erase_sorted(std::vector<uint32_t>& ids, uint32_t id) {
    if (auto it = std::lower_bound(ids.begin(), ids.end(), id); it != ids.end() && *it == id) ids.erase(it);
}

template <typename PosOf>
std::vector<uint32_t> AoiGrid::query(float x, float z, uint32_t self, PosOf&& posOf) const {
    std::vector<uint32_t> out;
    const CellKey lo = cell_of(x - radius_, z - radius_), hi = cell_of(x + radius_, z + radius_);
    const float r2 = radius_ * radius_;
    for (int32_t cx = lo.cx; cx <= hi.cx; ++cx)
        for (int32_t cz = lo.cz; cz <= hi.cz; ++cz) {
            auto it = cells_.find(pack({cx, cz}));
            if (it == cells_.end()) continue;
            for (uint32_t id : it->second) {
                if (id == self) continue;
                float ox = 0, oz = 0;
                posOf(id, ox, oz);
                if ((ox - x) * (ox - x) + (oz - z) * (oz - z) <= r2) out.push_back(id);
            }
        }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace kast
