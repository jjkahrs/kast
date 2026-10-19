#include "zone_session.h"

#include <log.h>

#include <algorithm>
#include <format>

void ZoneSession::update(const Document& doc)
{
    if (doc.play != PlayState::Stopped || doc.prefabEdit)
        return;
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    terrain_ = terrains.empty() ? entt::null : terrains.front();
    if (!doc.project || terrain_ == entt::null) {
        loaded_.reset();
        zone_.reset();
        project_ = {};
        error_.clear();
        return;
    }
    const Key key{doc.project->root, doc.scene.registry().get<kast::Terrain>(terrain_).map};
    if (loaded_ == key)
        return;
    loaded_ = key;
    zone_.reset();
    error_.clear();
    project_ = zone::scan_project(key.root);
    const auto map = std::ranges::find_if(project_.maps, [&](const zone::ProjectMap& m) { return m.info.id == key.map; });
    if (map == project_.maps.end())
        error_ = std::format("no map with id {} under maps/", key.map);
    else if (std::string err; !(zone_ = zone::load_map(project_, map->folder, err)))
        error_ = std::format("maps/{}: {}", map->folder, err);
    if (!error_.empty())
        kast::log::error("Zone: the Terrain map did not load: " + error_);
    else
        kast::log::info(std::format("Zone: loaded maps/{} (map {})", zone_->folder, key.map));
}
