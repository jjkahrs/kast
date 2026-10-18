// The zone builder's document: one map's files as JSON (edited in place, so keys the tool does not know survive), its
// tile heights, and undo/redo as whole-state snapshots. No file IO (zone_io.h) and no window.
#pragma once
#include <heights.h>
#include <map.h>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zone {

using Json = nlohmann::ordered_json; // keeps the files' key order
using kast::map::TileKey;
using Vec3 = std::array<float, 3>;

// Tiles sorted by z then x, as the engine streams them.
struct TileLess {
    bool operator()(TileKey a, TileKey b) const { return a.z != b.z ? a.z < b.z : a.x < b.x; }
};
template <typename T>
using TileMap = std::map<TileKey, T, TileLess>;

// Shared between snapshots until a tile is edited (copy on write), so an undo step costs only the tiles it touched.
using HeightsPtr = std::shared_ptr<const kast::map::Heights>;

// Everything one undo step restores.
struct ZoneState {
    Json manifest;               // map.json
    Json areas;                  // areas.json; null: no file
    TileMap<Json> spawns;        // .server/spawns/<tx>_<tz>.json
    TileMap<HeightsPtr> heights; // tiles with a heights file or edits; absent: flat at heightMin
};

// Same content: JSON equal, heights the same buffers (an edit always makes a new one).
bool same(const ZoneState& a, const ZoneState& b);

// A problem in one file, as the world server reports it ("maps/meadow/areas.json", "area \"x\": ...").
struct ZoneError {
    std::string file, message;
};

// "tx_tz"
std::string tile_name(TileKey t);

class ZoneDoc {
public:
    std::string folder;              // maps/<folder>
    ZoneState state;                 // what the tool shows and saves
    ZoneState saved;                 // what is on disk (empty for a new map)
    TileMap<std::string> heightExt;  // the heights file on disk per tile: ".r16" or ".png"

    // The manifest parsed; nullopt with err set if it is invalid.
    std::optional<kast::map::MapInfo> info(std::string& err) const;
    bool dirty() const { return !same(state, saved); }

    // Undo: begin() before an edit (a sculpt stroke: on mouse down), commit() after (mouse up). A commit that changed
    // nothing adds no step. edit() does both around fn.
    void begin();
    void commit();
    template <typename Fn>
    void edit(Fn&& fn) {
        begin();
        fn(state);
        commit();
    }
    // Between begin() and commit(): mid-stroke.
    bool editing() const { return pending_.has_value(); }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void undo();
    void redo();

private:
    // ponytail: unbounded history; cap it if long sessions run out of memory (a sculpt step is ~130 KB per tile).
    std::vector<ZoneState> undo_, redo_;
    std::optional<ZoneState> pending_;
};

} // namespace zone
