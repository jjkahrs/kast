#include "zone_doc.h"

namespace zone {

bool same(const ZoneState& a, const ZoneState& b) {
    return a.manifest == b.manifest && a.areas == b.areas && a.spawns == b.spawns && a.objects == b.objects &&
           a.interactables == b.interactables && a.heights == b.heights;
}

std::string tile_name(TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); }

std::optional<kast::map::MapInfo> ZoneDoc::info(std::string& err) const {
    return kast::map::parse_manifest(nlohmann::json(state.manifest), err);
}

void ZoneDoc::begin() { pending_ = state; }

void ZoneDoc::commit() {
    if (!pending_) return;
    if (!same(*pending_, state)) {
        undo_.push_back(std::move(*pending_));
        redo_.clear();
    }
    pending_.reset();
}

void ZoneDoc::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(state));
    state = std::move(undo_.back());
    undo_.pop_back();
}

void ZoneDoc::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(state));
    state = std::move(redo_.back());
    redo_.pop_back();
}

} // namespace zone
