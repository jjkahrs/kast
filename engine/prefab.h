#pragma once

#include "scene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Prefabs: .prefab assets (one entity subtree, see docs/engine/coordinates-and-scenes.html) and the instances of them
// in scenes and other prefabs. An instance is ordinary entities (every system sees real components) plus PrefabLink /
// PrefabInstance; its overrides are never stored separately: they are the difference between the entities and the
// prefab they came from (PrefabInstance::asset), worked out when saving and by the Inspector.
namespace kast {

constexpr const char* kPrefabExtension = ".prefab";

// One level of prefab nesting an entity belongs to: the instance root's PersistentId and the entity's id in that
// root's prefab.
struct PrefabHop {
    EntityId root = 0;
    EntityId local = 0;
};

// A loaded .prefab with its nested instances resolved: the read-only base its instances are compared with.
struct PrefabAsset {
    struct Entity {
        EntityId id = 0;     // prefab-local; stays the same across edits of the prefab
        EntityId parent = 0; // prefab-local; 0 for the root
        std::string name;
        nlohmann::ordered_json components; // object: type name -> JSON, in save order
        std::vector<PrefabHop> hops;       // nested instances it is part of, outermost first (prefab-local roots)
        std::string missingPrefab;         // a placeholder for a nested instance that didn't load: its prefab
    };
    std::string path;              // project-relative
    std::vector<Entity> entities;  // depth-first in child order; entities[0] is the root
    std::map<EntityId, std::shared_ptr<const PrefabAsset>> nested; // nested instance roots (prefab-local) -> prefab

    const Entity* find(EntityId id) const;
    // path is this prefab or one nested in it at any depth.
    bool contains(const std::string& prefab) const;
};

// On every entity of a prefab instance, the root included. hops is never empty; hops[0].root is the outermost
// instance root, and on that root hops[0].root is its own id.
struct PrefabLink {
    std::vector<PrefabHop> hops;
};

// On every instance root, nested ones included.
struct PrefabInstance {
    std::string path; // project-relative .prefab
    std::shared_ptr<const PrefabAsset> asset; // null: a placeholder, the prefab didn't load
    nlohmann::ordered_json placeholder;       // a placeholder's instance record, saved back unchanged (scene level)
};

// A child of a placeholder whose saved parent is one of the placeholder's unloaded entities: saved with that parent.
struct PlaceholderChild {
    EntityId parent = 0;
};

entt::entity findById(const Scene& scene, EntityId id); // entt::null if none
// entity is the outermost root of an instance (not a placeholder).
bool isPrefabRoot(const Scene& scene, entt::entity entity);
// A placeholder for an instance (or nested instance) whose prefab didn't load.
bool isMissingPrefab(const Scene& scene, entt::entity entity);
// The outermost instance root entity belongs to, or entt::null if it isn't part of an instance.
entt::entity prefabRoot(const Scene& scene, entt::entity entity);
// What entity came from in its outermost instance's prefab, or null.
const PrefabAsset::Entity* prefabBase(const Scene& scene, entt::entity entity);
// The prefab path of each nesting level of entity, outermost first (empty if it isn't part of an instance).
std::vector<std::string> prefabChain(const Scene& scene, entt::entity entity);

// entity's overrides of its outermost prefab (the scene-file instance entry without "source" / "id"): "name",
// "components" (changed fields per component), "added" (whole components it has and the prefab doesn't, or whose value
// isn't an object) and "removed" (component names), each only when not empty. The root's Name and Transform belong to
// the instance and are never listed. null if entity isn't part of an instance.
nlohmann::ordered_json prefabOverrides(const Scene& scene, entt::entity entity);
// The prefab entities of root's instance that the scene no longer has (deleted children), topmost only.
std::vector<const PrefabAsset::Entity*> deletedPrefabChildren(const Scene& scene, entt::entity root);
// Entities under root's instance that aren't from its prefab (added children), topmost only.
std::vector<entt::entity> addedPrefabChildren(const Scene& scene, entt::entity root);

// A new linked instance of asset under parent (entt::null = root) with fresh ids, at the prefab root's transform.
// entt::null + error if it would be deeper than kMaxHierarchyDepth.
entt::entity instantiatePrefab(Scene& scene, const std::shared_ptr<const PrefabAsset>& asset, entt::entity parent,
                               std::string& error);
// Scene::duplicate, keeping the instances inside the copied subtree linked (a copied prefab child becomes an added
// child: its link to an instance outside the copy is dropped).
entt::entity duplicateLinked(Scene& scene, entt::entity entity);
// Why entity can't be moved to another parent: it is a prefab child (unpack first). Null if it can.
const char* reparentBlocker(const Scene& scene, entt::entity entity);

// Reverts (in memory) one field or one component of an instance entity to its prefab's value: a removed component comes
// back, an added one goes. False if there is nothing to revert.
bool revertPrefabField(Scene& scene, entt::entity entity, const std::string& component, const std::string& field);
bool revertPrefabComponent(Scene& scene, entt::entity entity, const std::string& component);

// The operations below rewrite scene (or prefab files) and reload it from JSON: entity handles are stale afterwards
// (ids stay). projectRoot resolves prefab paths. False + error leaves scene unchanged (files may already be written for
// the Apply family; the next reload shows them).

// Brings back a deleted prefab child of root's instance (local: its prefab-local id).
bool revertDeletedChild(Scene& scene, entt::entity root, EntityId local, const std::filesystem::path& projectRoot,
                        std::string& error);
// Reverts every override of root's instance: fields, components, deleted children come back, added children go. The
// root's Name and Transform stay.
bool revertAllOverrides(Scene& scene, entt::entity root, const std::filesystem::path& projectRoot, std::string& error);
// Writes entity's current value of one field (field empty: the whole component, or its absence) into the prefab of
// nesting level hop (0 = outermost) and clears the matching override in every level outside it, then reloads scene.
bool applyPrefabOverride(Scene& scene, entt::entity entity, const std::string& component, const std::string& field,
                         size_t hop, const std::filesystem::path& projectRoot, std::string& error);
// Removes a deleted prefab child (local) of root's instance from its outermost prefab.
bool applyDeletedChild(Scene& scene, entt::entity root, EntityId local, const std::filesystem::path& projectRoot,
                       std::string& error);
// Writes root's whole instance (overrides, added and deleted children; not the root's Name and Transform) into its
// outermost prefab.
bool applyAllOverrides(Scene& scene, entt::entity root, const std::filesystem::path& projectRoot, std::string& error);
// Breaks root's link: its entities become plain, keeping their values; nested instances inside stay linked. In memory.
void unpackPrefab(Scene& scene, entt::entity root);
// Writes entity's subtree as a new prefab at path (project-relative) and makes entity an instance of it. Refused if
// entity is already part of an instance, its subtree holds a missing-prefab placeholder or an instance of path itself.
bool createPrefab(Scene& scene, entt::entity entity, const std::string& path, const std::filesystem::path& projectRoot,
                  std::string& error);
// Why scene can't be saved as the prefab path: an instance in it contains path (a cycle). Empty if it can.
std::string prefabCycleError(const Scene& scene, const std::string& path);
// Replaces scene with a reload of itself, so every instance picks up its prefab's file as it is now.
bool reloadPrefabs(Scene& scene, const std::filesystem::path& projectRoot, std::string& error);

} // namespace kast
