#include "map_session.h"

#include "asset_ids.h"

#include <files.h>
#include <log.h>
#include <prefab.h> // kPrefabExtension
#include <zone_scenes.h>

#include <algorithm>
#include <format>

void MapSession::scan(const std::filesystem::path& root)
{
    project_ = root.empty() ? mapedit::Project{} : mapedit::scan_project(root);
    materials_.clear();
    prefabs_.clear();
    npcs_.clear();
    if (root.empty())
        return;
    for (const std::filesystem::path& file : kast::listFiles(root)) {
        const std::u8string rel = file.lexically_relative(root).generic_u8string();
        if (file.extension() == ".mat")
            materials_.emplace_back(rel.begin(), rel.end());
        if (file.extension() == kast::kPrefabExtension)
            prefabs_.emplace_back(rel.begin(), rel.end());
    }
    npcs_ = mapedit::npc_names(root);
}

void MapSession::update(const Document& doc)
{
    if (doc.play != PlayState::Stopped || doc.prefabEdit)
        return;
    const auto terrains = doc.scene.registry().view<kast::Terrain>();
    terrain_ = terrains.empty() ? entt::null : terrains.front();
    const std::filesystem::path root = doc.project ? doc.project->root : std::filesystem::path();
    if (root != project_.root)
        scan(root); // the Map tab's map picker lists them even without a Terrain entity
    if (root.empty() || terrain_ == entt::null) {
        loaded_.reset();
        doc_.reset();
        error_.clear();
        problems_.clear();
        return;
    }
    const Key key{root, doc.scene.registry().get<kast::Terrain>(terrain_).map};
    if (loaded_ == key)
        return;
    loaded_ = key;
    doc_.reset();
    error_.clear();
    problems_.clear();
    scan(root); // maps made on disk since
    const auto map = std::ranges::find_if(project_.maps, [&](const mapedit::ProjectMap& m) { return m.info.id == key.map; });
    if (map == project_.maps.end())
        error_ = std::format("no map with id {} under maps/", key.map);
    else if (std::string err; !(doc_ = mapedit::load_map(project_, map->folder, err)))
        error_ = std::format("maps/{}: {}", map->folder, err);
    if (!error_.empty()) {
        kast::log::error("Map: the Terrain map did not load: " + error_);
        return;
    }
    problems_ = mapedit::validate(project_, *doc_);
    kast::log::info(std::format("Map: loaded maps/{} (map {}){}", doc_->folder, key.map,
                                problems_.empty() ? "" : std::format(" with {} problem(s)", problems_.size())));
    for (const mapedit::MapError& e : problems_)
        kast::log::warn(std::format("Map: {}: {}", e.file, e.message));
}

void MapSession::remap(const std::string& from, const std::string& to)
{
    if (!doc_)
        return;
    for (mapedit::MapState* state : {&doc_->state, &doc_->saved}) {
        remapAssetPaths(state->manifest, from, to);
        for (auto& [tile, objects] : state->objects)
            remapAssetPaths(objects, from, to);
    }
}

bool MapSession::save()
{
    if (!doc_)
        return false;
    renamed_.clear();
    const std::map<std::string, std::string> renames = doc_->state.zoneRenames;
    problems_ = mapedit::save_map(project_, *doc_, recycle_);
    if (!problems_.empty()) {
        kast::log::error(std::format("Map: save of maps/{} blocked: {} problem(s), nothing written (Map tab > Problems)",
                                     doc_->folder, problems_.size()));
        for (const mapedit::MapError& e : problems_)
            kast::log::error(std::format("Map: {}: {}", e.file, e.message));
        return false;
    }
    kast::log::info("Map: saved maps/" + doc_->folder);
    for (const auto& [zone, onDisk] : renames) {
        renamed_.emplace_back(kast::zoneScenePath(doc_->folder, onDisk), kast::zoneScenePath(doc_->folder, zone));
        kast::log::info(std::format("Map: zone {} renamed from {}: its scene is {}", zone, onDisk, renamed_.back().second));
    }
    scan(project_.root); // a new map, or a generated terrain material, is on disk now
    return true;
}

bool MapSession::check()
{
    if (!doc_)
        return false;
    problems_ = mapedit::validate(project_, *doc_);
    for (const mapedit::MapError& e : problems_)
        kast::log::warn(std::format("Map: {}: {}", e.file, e.message));
    return problems_.empty();
}

bool MapSession::open(const std::string& folder, std::string& error)
{
    const auto map = std::ranges::find_if(project_.maps, [&](const mapedit::ProjectMap& m) { return m.folder == folder; });
    std::optional<mapedit::MapDoc> loaded;
    if (map == project_.maps.end())
        error = std::format("no map maps/{}", folder);
    else
        loaded = mapedit::load_map(project_, folder, error);
    if (!loaded)
        return false;
    doc_ = std::move(loaded);
    loaded_ = Key{project_.root, map->info.id};
    error_.clear();
    problems_ = mapedit::validate(project_, *doc_);
    kast::log::info(std::format("Map: opened maps/{} (map {}){}", folder, map->info.id,
                                problems_.empty() ? "" : std::format(" with {} problem(s)", problems_.size())));
    return true;
}

bool MapSession::newMap(const std::string& folder, const std::string& name, uint16_t id, int width, int depth,
                         std::string& error)
{
    std::optional<mapedit::MapDoc> made = mapedit::new_map(project_, folder, name, id, width, depth, error);
    if (!made)
        return false;
    doc_ = std::move(made);
    loaded_ = Key{project_.root, id};
    error_.clear();
    problems_.clear();
    kast::log::info(std::format("Map: new map maps/{} (id {}, {}x{} tiles); Save writes it", folder, id, width, depth));
    return true;
}
