#include "zone_session.h"

#include <files.h>
#include <log.h>

#include <algorithm>
#include <format>

void ZoneSession::scan(const std::filesystem::path& root)
{
    project_ = root.empty() ? zone::Project{} : zone::scan_project(root);
    materials_.clear();
    if (root.empty())
        return;
    for (const std::filesystem::path& file : kast::listFiles(root))
        if (file.extension() == ".mat") {
            const std::u8string rel = file.lexically_relative(root).generic_u8string();
            materials_.emplace_back(rel.begin(), rel.end());
        }
}

void ZoneSession::update(const Document& doc)
{
    if (doc.play != PlayState::Stopped || doc.prefabEdit)
        return;
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    terrain_ = terrains.empty() ? entt::null : terrains.front();
    const std::filesystem::path root = doc.project ? doc.project->root : std::filesystem::path();
    if (root != project_.root)
        scan(root); // the Zone tab's map picker lists them even without a Terrain entity
    if (root.empty() || terrain_ == entt::null) {
        loaded_.reset();
        zone_.reset();
        error_.clear();
        problems_.clear();
        return;
    }
    const Key key{root, doc.scene.registry().get<kast::Terrain>(terrain_).map};
    if (loaded_ == key)
        return;
    loaded_ = key;
    zone_.reset();
    error_.clear();
    problems_.clear();
    scan(root); // maps made on disk since
    const auto map = std::ranges::find_if(project_.maps, [&](const zone::ProjectMap& m) { return m.info.id == key.map; });
    if (map == project_.maps.end())
        error_ = std::format("no map with id {} under maps/", key.map);
    else if (std::string err; !(zone_ = zone::load_map(project_, map->folder, err)))
        error_ = std::format("maps/{}: {}", map->folder, err);
    if (!error_.empty()) {
        kast::log::error("Zone: the Terrain map did not load: " + error_);
        return;
    }
    problems_ = zone::validate(project_, *zone_);
    kast::log::info(std::format("Zone: loaded maps/{} (map {}){}", zone_->folder, key.map,
                                problems_.empty() ? "" : std::format(" with {} problem(s)", problems_.size())));
    for (const zone::ZoneError& e : problems_)
        kast::log::warn(std::format("Zone: {}: {}", e.file, e.message));
}

bool ZoneSession::save(bool recycle)
{
    if (!zone_)
        return false;
    problems_ = zone::save_map(project_, *zone_, recycle);
    if (!problems_.empty()) {
        kast::log::error(std::format("Zone: save of maps/{} blocked: {} problem(s), nothing written (Zone tab > Problems)",
                                     zone_->folder, problems_.size()));
        for (const zone::ZoneError& e : problems_)
            kast::log::error(std::format("Zone: {}: {}", e.file, e.message));
        return false;
    }
    kast::log::info("Zone: saved maps/" + zone_->folder);
    scan(project_.root); // a new map, or a generated terrain material, is on disk now
    return true;
}

bool ZoneSession::newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth,
                         std::string& error)
{
    std::optional<zone::ZoneDoc> made = zone::new_map(project_, folder, name, id, width, depth, error);
    if (!made)
        return false;
    zone_ = std::move(made);
    loaded_ = Key{project_.root, id};
    error_.clear();
    problems_.clear();
    kast::log::info(std::format("Zone: new map maps/{} (id {}, {}x{} tiles); Save writes it", folder, id, width, depth));
    return true;
}
