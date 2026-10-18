#include "prefab.h"

#include "components.h"
#include "log.h"
#include "scene_io.h"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <set>

namespace kast {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kNameField = "Name"; // the pseudo-component the revert / apply functions use for the entity name

EntityId idOf(const Scene& scene, entt::entity entity) { return scene.registry().get<PersistentId>(entity).value; }

// entity and its descendants, depth-first in child order.
std::vector<entt::entity> subtree(const Scene& scene, entt::entity entity)
{
    std::vector<entt::entity> out;
    auto visit = [&](auto& self, entt::entity e) -> void {
        out.push_back(e);
        for (entt::entity child : scene.children(e))
            self(self, child);
    };
    visit(visit, entity);
    return out;
}

// entity is part of the instance whose root has id rootId (at the outermost level).
bool linkedTo(const Scene& scene, entt::entity entity, EntityId rootId)
{
    const auto* link = scene.registry().try_get<PrefabLink>(entity);
    return link && link->hops.front().root == rootId;
}

const PrefabInstance* instanceAt(const Scene& scene, EntityId rootId)
{
    const entt::entity root = findById(scene, rootId);
    return root == entt::null ? nullptr : scene.registry().try_get<PrefabInstance>(root);
}

// Sets (or with value null removes) one field, a whole component or the name ("Name") of entity.
bool setEntityState(Scene& scene, entt::entity entity, const std::string& component, const std::string& field,
                    const Json* value, std::string& error)
{
    entt::registry& registry = scene.registry();
    if (component == kNameField) {
        registry.get<Name>(entity).value = value->get<std::string>();
        return true;
    }
    const ComponentType type = findAnyComponentType(component);
    if (!type) {
        error = std::format("\"{}\" has no valid definition", component);
        return false;
    }
    if (!value) {
        if (hasComponent(registry, entity, type))
            removeComponent(registry, entity, type);
        return true;
    }
    if (field.empty())
        return setComponentJson(registry, entity, type, *value, error);
    if (!hasComponent(registry, entity, type)) {
        error = std::format("'{}' has no \"{}\" to set \"{}\" on: apply the whole component",
                            registry.get<Name>(entity).value, component, field);
        return false;
    }
    Json json = componentToJson(registry, entity, type);
    json[field] = *value;
    return setComponentJson(registry, entity, type, json, error);
}

// Loads the prefab path as a scene, runs edit on it and saves it.
bool editPrefabFile(const std::filesystem::path& projectRoot, const std::string& path,
                    const std::function<bool(Scene&, std::string&)>& edit, std::string& error)
{
    Scene prefab;
    return loadPrefabScene(projectRoot, path, prefab, error) && edit(prefab, error) &&
           savePrefab(prefab, projectRoot, path, error);
}

// Rewrites root's instance record in scene's JSON and reloads scene from it.
bool editInstanceRecord(Scene& scene, entt::entity root, const std::filesystem::path& projectRoot,
                        const std::function<void(Json& entities, size_t record)>& edit, std::string& error)
{
    Json json = sceneToJson(scene);
    Json& entities = json["entities"];
    const EntityId rootId = idOf(scene, root);
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i].contains("prefab") && entities[i]["id"] == rootId) {
            edit(entities, i);
            Scene out;
            if (!sceneFromJson(json, out, error, nullptr, projectRoot))
                return false;
            scene = std::move(out);
            return true;
        }
    }
    error = "not a prefab instance root";
    return false;
}

// Why root's subtree can't become (part of) a prefab file: it holds a missing-prefab placeholder, whose reference
// a prefab couldn't keep. Empty if it can.
std::string placeholderError(const Scene& scene, entt::entity root)
{
    for (entt::entity entity : subtree(scene, root))
        if (isMissingPrefab(scene, entity))
            return std::format("'{}' is a missing-prefab placeholder ('{}'): fix or delete it first",
                               scene.registry().get<Name>(entity).value,
                               scene.registry().get<PrefabInstance>(entity).path);
    return {};
}

// root's subtree as a prefab scene. keep: entities of root's own instance keep their prefab-local ids and lose that
// link; every other entity gets a fresh id, past previous's ids (the prefab being replaced: overrides elsewhere may
// name its deleted entities). Links to instances inside the subtree are kept. locals: each subtree
// entity's id in the result, depth-first.
Scene extractPrefab(const Scene& scene, entt::entity root, bool keep, const PrefabAsset* previous,
                    std::vector<std::pair<entt::entity, EntityId>>& locals)
{
    const entt::registry& from = scene.registry();
    const EntityId rootId = idOf(scene, root);
    const std::vector<entt::entity> entities = subtree(scene, root);
    Scene out;
    std::map<EntityId, EntityId> ids; // scene -> prefab-local
    if (previous)
        for (const PrefabAsset::Entity& entity : previous->entities)
            out.reserveId(entity.id);
    if (keep)
        for (entt::entity entity : entities)
            if (linkedTo(scene, entity, rootId)) {
                ids[idOf(scene, entity)] = from.get<PrefabLink>(entity).hops.front().local;
                out.reserveId(ids[idOf(scene, entity)]);
            }
    for (entt::entity entity : entities)
        if (!ids.contains(idOf(scene, entity)))
            ids[idOf(scene, entity)] = out.newId();

    std::map<entt::entity, entt::entity> copies;
    for (entt::entity entity : entities) {
        const EntityId id = ids.at(idOf(scene, entity));
        const entt::entity copy =
            out.createWithId(id, from.get<Name>(entity).value, entity == root ? entt::null : copies.at(scene.parent(entity)));
        copies[entity] = copy;
        copyComponents(from, entity, out.registry(), copy);
        locals.emplace_back(entity, id);
        const auto* link = from.try_get<PrefabLink>(entity);
        if (!link)
            continue;
        PrefabLink kept;
        for (const PrefabHop& hop : link->hops)
            if (!(keep && hop.root == rootId) && ids.contains(hop.root))
                kept.hops.push_back({ids.at(hop.root), hop.local});
        if (kept.hops.empty())
            continue;
        const bool nestedRoot = std::ranges::any_of(kept.hops, [&](const PrefabHop& hop) { return hop.root == id; });
        out.registry().emplace<PrefabLink>(copy, std::move(kept));
        if (const auto* instance = from.try_get<PrefabInstance>(entity); instance && instance->asset && nestedRoot)
            out.registry().emplace<PrefabInstance>(copy, *instance);
    }
    return out;
}

// After extractPrefab wrote path: root becomes (again) an instance of it, every subtree entity linked by locals.
void relink(Scene& scene, entt::entity root, const std::vector<std::pair<entt::entity, EntityId>>& locals,
            const std::string& path, std::shared_ptr<const PrefabAsset> asset)
{
    entt::registry& registry = scene.registry();
    const EntityId rootId = idOf(scene, root);
    for (const auto& [entity, local] : locals) {
        if (linkedTo(scene, entity, rootId))
            continue; // same local id
        std::vector<PrefabHop> hops{{rootId, local}};
        if (const auto* link = registry.try_get<PrefabLink>(entity))
            hops.insert(hops.end(), link->hops.begin(), link->hops.end());
        registry.emplace_or_replace<PrefabLink>(entity, std::move(hops));
    }
    registry.emplace_or_replace<PrefabInstance>(root, path, std::move(asset), nullptr);
}

} // namespace

const PrefabAsset::Entity* PrefabAsset::find(EntityId id) const
{
    const auto it = std::ranges::find(entities, id, &Entity::id);
    return it == entities.end() ? nullptr : &*it;
}

bool PrefabAsset::contains(const std::string& prefab) const
{
    return path == prefab ||
           std::ranges::any_of(nested, [&](const auto& entry) { return entry.second->contains(prefab); });
}

entt::entity findById(const Scene& scene, EntityId id)
{
    for (auto [entity, persistent] : scene.registry().view<PersistentId>().each())
        if (persistent.value == id)
            return entity;
    return entt::null;
}

bool isPrefabRoot(const Scene& scene, entt::entity entity)
{
    const auto* instance = scene.registry().try_get<PrefabInstance>(entity);
    return instance && instance->asset && linkedTo(scene, entity, idOf(scene, entity));
}

bool isMissingPrefab(const Scene& scene, entt::entity entity)
{
    const auto* instance = scene.registry().try_get<PrefabInstance>(entity);
    return instance && !instance->asset;
}

entt::entity prefabRoot(const Scene& scene, entt::entity entity)
{
    const auto* link = scene.registry().try_get<PrefabLink>(entity);
    return link ? findById(scene, link->hops.front().root) : entt::null;
}

const PrefabAsset::Entity* prefabBase(const Scene& scene, entt::entity entity)
{
    const auto* link = scene.registry().try_get<PrefabLink>(entity);
    const PrefabInstance* instance = link ? instanceAt(scene, link->hops.front().root) : nullptr;
    return instance && instance->asset ? instance->asset->find(link->hops.front().local) : nullptr;
}

std::vector<std::string> prefabChain(const Scene& scene, entt::entity entity)
{
    std::vector<std::string> paths;
    if (const auto* link = scene.registry().try_get<PrefabLink>(entity))
        for (const PrefabHop& hop : link->hops)
            if (const PrefabInstance* instance = instanceAt(scene, hop.root))
                paths.push_back(instance->path);
    return paths;
}

Json prefabOverrides(const Scene& scene, entt::entity entity)
{
    const PrefabAsset::Entity* base = prefabBase(scene, entity);
    if (!base)
        return nullptr;
    const entt::registry& registry = scene.registry();
    const bool root = isPrefabRoot(scene, entity);
    Json overrides = diffPrefabEntity(registry.get<Name>(entity).value, entityComponentsToJson(registry, entity), *base, root);
    if (root && overrides.contains("components")) {
        overrides["components"].erase(entt::resolve<Transform>().name());
        if (overrides["components"].empty())
            overrides.erase("components");
    }
    return overrides;
}

std::vector<const PrefabAsset::Entity*> deletedPrefabChildren(const Scene& scene, entt::entity root)
{
    std::vector<const PrefabAsset::Entity*> deleted;
    const auto* instance = scene.registry().try_get<PrefabInstance>(root);
    if (!instance || !instance->asset)
        return deleted;
    const EntityId rootId = idOf(scene, root);
    std::set<EntityId> present, absent;
    for (entt::entity entity : subtree(scene, root))
        if (linkedTo(scene, entity, rootId))
            present.insert(scene.registry().get<PrefabLink>(entity).hops.front().local);
    for (const PrefabAsset::Entity& base : instance->asset->entities) {
        if (present.contains(base.id))
            continue;
        if (!absent.contains(base.parent))
            deleted.push_back(&base);
        absent.insert(base.id);
    }
    return deleted;
}

std::vector<entt::entity> addedPrefabChildren(const Scene& scene, entt::entity root)
{
    std::vector<entt::entity> added;
    const EntityId rootId = idOf(scene, root);
    auto visit = [&](auto& self, entt::entity entity) -> void {
        for (entt::entity child : scene.children(entity)) {
            if (linkedTo(scene, child, rootId))
                self(self, child);
            else
                added.push_back(child);
        }
    };
    visit(visit, root);
    return added;
}

entt::entity duplicateLinked(Scene& scene, entt::entity entity)
{
    const std::vector<entt::entity> before = subtree(scene, entity);
    const entt::entity copy = scene.duplicate(entity);
    const std::vector<entt::entity> after = subtree(scene, copy);
    entt::registry& registry = scene.registry();
    std::map<EntityId, EntityId> ids;
    for (size_t i = 0; i < before.size(); ++i)
        ids[idOf(scene, before[i])] = idOf(scene, after[i]);
    for (size_t i = 0; i < before.size(); ++i) {
        if (isMissingPrefab(scene, before[i]) && !registry.all_of<PrefabLink>(before[i]))
            log::warn(std::format("Duplicate: '{}' is a missing-prefab placeholder; its copy is a plain entity",
                                  registry.get<Name>(before[i]).value));
        const auto* link = registry.try_get<PrefabLink>(before[i]);
        if (!link)
            continue;
        PrefabLink kept; // only links to instances copied whole
        for (const PrefabHop& hop : link->hops)
            if (ids.contains(hop.root))
                kept.hops.push_back({ids.at(hop.root), hop.local});
        if (kept.hops.empty())
            continue;
        const EntityId id = idOf(scene, after[i]);
        const bool root = std::ranges::any_of(kept.hops, [&](const PrefabHop& hop) { return hop.root == id; });
        std::optional<PrefabInstance> instance;
        if (const auto* source = registry.try_get<PrefabInstance>(before[i]); source && root)
            instance = *source; // copied before emplacing: the pool may move
        registry.emplace<PrefabLink>(after[i], std::move(kept));
        if (instance)
            registry.emplace<PrefabInstance>(after[i], std::move(*instance));
    }
    return copy;
}

const char* reparentBlocker(const Scene& scene, entt::entity entity)
{
    const auto* link = scene.registry().try_get<PrefabLink>(entity);
    return link && link->hops.front().root != idOf(scene, entity)
               ? "It is part of a prefab instance: unpack the instance to move its entities"
               : nullptr;
}

bool revertPrefabField(Scene& scene, entt::entity entity, const std::string& component, const std::string& field)
{
    const PrefabAsset::Entity* base = prefabBase(scene, entity);
    if (!base)
        return false;
    entt::registry& registry = scene.registry();
    if (component == kNameField) {
        if (isPrefabRoot(scene, entity) || registry.get<Name>(entity).value == base->name)
            return false;
        registry.get<Name>(entity).value = base->name;
        return true;
    }
    const ComponentType type = findAnyComponentType(component);
    if (!type || !hasComponent(registry, entity, type) || !base->components.contains(component) ||
        !base->components[component].contains(field))
        return false;
    Json json = componentToJson(registry, entity, type);
    if (json[field] == base->components[component][field])
        return false;
    json[field] = base->components[component][field];
    std::string error;
    return setComponentJson(registry, entity, type, json, error);
}

bool revertPrefabComponent(Scene& scene, entt::entity entity, const std::string& component)
{
    const PrefabAsset::Entity* base = prefabBase(scene, entity);
    const ComponentType type = findAnyComponentType(component);
    if (!base || !type)
        return false;
    entt::registry& registry = scene.registry();
    const bool has = hasComponent(registry, entity, type);
    if (!base->components.contains(component)) { // added
        if (has)
            removeComponent(registry, entity, type);
        return has;
    }
    if (has && componentToJson(registry, entity, type) == base->components[component])
        return false;
    std::string error;
    return setComponentJson(registry, entity, type, base->components[component], error);
}

bool revertDeletedChild(Scene& scene, entt::entity root, EntityId local, const std::filesystem::path& projectRoot,
                        std::string& error)
{
    return editInstanceRecord(scene, root, projectRoot, [&](Json& entities, size_t record) {
        Json& entries = entities[record]["instance"];
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if ((*it)["source"] == local && it->contains("deleted")) {
                entries.erase(it);
                return;
            }
    }, error);
}

bool revertAllOverrides(Scene& scene, entt::entity root, const std::filesystem::path& projectRoot, std::string& error)
{
    const char* transform = entt::resolve<Transform>().name();
    return editInstanceRecord(scene, root, projectRoot, [&](Json& entities, size_t record) {
        std::set<EntityId> inside; // the instance's entities, then the added children found under them
        Json kept = Json::array();
        for (const Json& entry : entities[record]["instance"]) {
            if (entry.contains("deleted"))
                continue;
            inside.insert(entry["id"].get<EntityId>());
            Json reverted = {{"source", entry["source"]}, {"id", entry["id"]}};
            if (entry["id"] == entities[record]["id"] && entry.contains("components") && entry["components"].contains(transform))
                reverted["components"] = {{transform, entry["components"][transform]}};
            kept.push_back(std::move(reverted));
        }
        entities[record]["instance"] = std::move(kept);
        // Saved depth-first: an added child comes after its parent, and its own children after it.
        Json remaining = Json::array();
        for (size_t i = 0; i < entities.size(); ++i) {
            const Json& entity = entities[i];
            if (i > record && entity["parent"].is_number_unsigned() && inside.contains(entity["parent"].get<EntityId>())) {
                inside.insert(entity["id"].get<EntityId>());
                if (entity.contains("instance"))
                    for (const Json& entry : entity["instance"])
                        if (entry.contains("id"))
                            inside.insert(entry["id"].get<EntityId>());
                continue;
            }
            remaining.push_back(entity);
        }
        entities = std::move(remaining);
    }, error);
}

bool applyPrefabOverride(Scene& scene, entt::entity entity, const std::string& component, const std::string& field,
                         size_t hop, const std::filesystem::path& projectRoot, std::string& error)
{
    const auto* link = scene.registry().try_get<PrefabLink>(entity);
    if (!link || hop >= link->hops.size()) {
        error = "not part of a prefab instance at that nesting level";
        return false;
    }
    const entt::registry& registry = scene.registry();
    std::optional<Json> value; // nullopt: the component is gone
    if (component == kNameField) {
        value = registry.get<Name>(entity).value;
    } else if (const ComponentType type = findAnyComponentType(component); type && hasComponent(registry, entity, type)) {
        Json json = componentToJson(registry, entity, type);
        value = field.empty() ? json : json[field];
    } else if (!field.empty() || !type) {
        error = std::format("'{}' has no \"{}\"", registry.get<Name>(entity).value, component);
        return false;
    }
    std::vector<std::pair<std::string, EntityId>> levels; // prefab path, entity's id in it; outermost first
    for (size_t i = 0; i <= hop; ++i) {
        const PrefabInstance* instance = instanceAt(scene, link->hops[i].root);
        if (!instance || !instance->asset) {
            error = "its prefab didn't load";
            return false;
        }
        levels.emplace_back(instance->path, link->hops[i].local);
    }
    // Innermost first: each outer prefab then loads the new value and, set to the same, drops its override.
    for (size_t i = levels.size(); i-- > 0;) {
        const auto& [path, local] = levels[i];
        const bool ok = editPrefabFile(projectRoot, path, [&](Scene& prefab, std::string& why) {
            const entt::entity target = findById(prefab, local);
            if (target == entt::null) {
                why = std::format("'{}' no longer has that entity", path);
                return false;
            }
            return setEntityState(prefab, target, component, field, value ? &*value : nullptr, why);
        }, error);
        if (!ok)
            return false;
        log::info(std::format("Applied {}{}{} to '{}'", component, field.empty() ? "" : ".", field, path));
    }
    return reloadPrefabs(scene, projectRoot, error);
}

bool applyDeletedChild(Scene& scene, entt::entity root, EntityId local, const std::filesystem::path& projectRoot,
                       std::string& error)
{
    const auto* instance = scene.registry().try_get<PrefabInstance>(root);
    if (!instance || !instance->asset) {
        error = "not a prefab instance root";
        return false;
    }
    const std::string path = instance->path;
    return editPrefabFile(projectRoot, path, [&](Scene& prefab, std::string& why) {
        const entt::entity target = findById(prefab, local);
        if (target == entt::null || prefab.parent(target) == entt::null) {
            why = std::format("'{}' has no such child", path);
            return false;
        }
        prefab.destroy(target);
        return true;
    }, error) && reloadPrefabs(scene, projectRoot, error);
}

bool applyAllOverrides(Scene& scene, entt::entity root, const std::filesystem::path& projectRoot, std::string& error)
{
    if (!isPrefabRoot(scene, root)) {
        error = "not a prefab instance root";
        return false;
    }
    if (error = placeholderError(scene, root); !error.empty())
        return false;
    const PrefabInstance instance = scene.registry().get<PrefabInstance>(root);
    std::vector<std::pair<entt::entity, EntityId>> locals;
    Scene prefab = extractPrefab(scene, root, true, instance.asset.get(), locals);
    // The root's Name and Transform are the instance's own: the prefab keeps its.
    const entt::entity prefabRootEntity = prefab.children(entt::null).front();
    const PrefabAsset::Entity& base = instance.asset->entities.front();
    prefab.registry().get<Name>(prefabRootEntity).value = base.name;
    setComponentJson(prefab.registry(), prefabRootEntity, ComponentType(entt::resolve<Transform>()),
                     base.components.at(entt::resolve<Transform>().name()), error);
    std::shared_ptr<const PrefabAsset> asset;
    if (!savePrefab(prefab, projectRoot, instance.path, error) || !loadPrefab(projectRoot, instance.path, asset, error))
        return false;
    relink(scene, root, locals, instance.path, std::move(asset));
    log::info(std::format("Applied all overrides to '{}'", instance.path));
    return reloadPrefabs(scene, projectRoot, error);
}

void unpackPrefab(Scene& scene, entt::entity root)
{
    entt::registry& registry = scene.registry();
    const EntityId rootId = idOf(scene, root);
    for (entt::entity entity : subtree(scene, root)) {
        if (!linkedTo(scene, entity, rootId))
            continue;
        std::vector<PrefabHop>& hops = registry.get<PrefabLink>(entity).hops;
        hops.erase(hops.begin());
        if (hops.empty())
            registry.remove<PrefabLink>(entity);
    }
    registry.remove<PrefabInstance>(root);
}

bool createPrefab(Scene& scene, entt::entity entity, const std::string& path, const std::filesystem::path& projectRoot,
                  std::string& error)
{
    if (scene.registry().all_of<PrefabLink>(entity)) {
        error = std::format("'{}' is already part of a prefab instance", scene.registry().get<Name>(entity).value);
        return false;
    }
    if (error = placeholderError(scene, entity); !error.empty())
        return false;
    std::vector<std::pair<entt::entity, EntityId>> locals;
    const Scene prefab = extractPrefab(scene, entity, false, nullptr, locals);
    std::shared_ptr<const PrefabAsset> asset;
    if (!savePrefab(prefab, projectRoot, path, error) || !loadPrefab(projectRoot, path, asset, error))
        return false;
    relink(scene, entity, locals, path, std::move(asset));
    log::info(std::format("Created prefab '{}'", path));
    return true;
}

std::string prefabCycleError(const Scene& scene, const std::string& path)
{
    for (auto [entity, instance] : scene.registry().view<PrefabInstance>().each())
        if (instance.asset && instance.asset->contains(path))
            return std::format("'{}' is an instance of '{}'{}: a prefab can't contain itself",
                               scene.registry().get<Name>(entity).value, instance.path,
                               instance.path == path ? "" : std::format(", which contains '{}'", path));
    return {};
}

bool reloadPrefabs(Scene& scene, const std::filesystem::path& projectRoot, std::string& error)
{
    Scene out;
    SceneLoadReport report;
    if (!sceneFromJson(sceneToJson(scene), out, error, &report, projectRoot))
        return false;
    for (const std::string& warning : report.warnings)
        log::warn(warning);
    scene = std::move(out);
    return true;
}

} // namespace kast
