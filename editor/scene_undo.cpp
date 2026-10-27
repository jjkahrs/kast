#include "scene_undo.h"

#include "document.h"

#include <components.h>
#include <log.h>
#include <prefab.h>
#include <scene_io.h>

#include <algorithm>
#include <format>

namespace {

using Json = nlohmann::ordered_json;

// to's T on t becomes a copy of from's on s (or goes, if s has none).
template <typename T>
void copyOptional(const entt::registry& from, entt::entity s, entt::registry& to, entt::entity t)
{
    if (const T* value = from.try_get<T>(s)) {
        T copy = *value; // first: emplacing may move the source pool
        to.emplace_or_replace<T>(t, std::move(copy));
    } else {
        to.remove<T>(t);
    }
}

// Entity t (in to) gets s's name, components and prefab links (from), losing components s doesn't have.
void syncEntity(const entt::registry& from, entt::entity s, entt::registry& to, entt::entity t)
{
    for (const kast::ComponentType& type : kast::allComponentTypes())
        if (kast::hasComponent(to, t, type) && !kast::hasComponent(from, s, type))
            kast::removeComponent(to, t, type);
    if (!from.all_of<kast::PreservedComponents>(s))
        to.remove<kast::PreservedComponents>(t);
    kast::copyComponents(from, s, to, t);
    to.get<kast::Name>(t).value = from.get<kast::Name>(s).value;
    copyOptional<kast::PrefabLink>(from, s, to, t);
    copyOptional<kast::PrefabInstance>(from, s, to, t);
    copyOptional<kast::PlaceholderChild>(from, s, to, t);
}

kast::EntityId idOf(const kast::Scene& scene, entt::entity entity)
{
    return scene.registry().get<kast::PersistentId>(entity).value;
}

// A copy of source (in from) with the same id under parent (in to); deep: with its subtree.
entt::entity copyEntity(const kast::Scene& from, entt::entity source, kast::Scene& to, entt::entity parent, bool deep)
{
    const entt::entity copy = to.createWithId(idOf(from, source), "", parent);
    if (copy == entt::null)
        return copy; // past kMaxHierarchyDepth: can't happen for a subtree that was in a scene
    syncEntity(from.registry(), source, to.registry(), copy);
    if (deep)
        for (entt::entity child : from.children(source))
            copyEntity(from, child, to, copy, true);
    return copy;
}

Json describeEntity(const kast::Scene& scene, entt::entity entity, bool deep)
{
    const entt::registry& registry = scene.registry();
    Json json = {{"id", idOf(scene, entity)},
                 {"name", registry.get<kast::Name>(entity).value},
                 {"components", kast::entityComponentsToJson(registry, entity)}};
    if (const auto* link = registry.try_get<kast::PrefabLink>(entity))
        for (const kast::PrefabHop& hop : link->hops)
            json["hops"].push_back({hop.root, hop.local});
    if (const auto* instance = registry.try_get<kast::PrefabInstance>(entity))
        json["instance"] = {instance->path, instance->placeholder};
    if (const auto* child = registry.try_get<kast::PlaceholderChild>(entity))
        json["placeholderParent"] = child->parent;
    if (deep)
        for (entt::entity child : scene.children(entity))
            json["children"].push_back(describeEntity(scene, child, true));
    return json;
}

} // namespace

SceneState::Record* SceneState::find(kast::EntityId id)
{
    const auto it = std::ranges::find(records_, id, &Record::id);
    return it == records_.end() ? nullptr : &*it;
}

bool SceneState::covers(kast::EntityId id) const { return std::ranges::find(records_, id, &Record::id) != records_.end(); }

bool SceneState::hasPrefabInstances() const
{
    return !stash_.registry().view<kast::PrefabLink>().empty() || !stash_.registry().view<kast::PrefabInstance>().empty();
}

std::vector<kast::SceneSlot> SceneState::scenes() const
{
    std::vector<kast::SceneSlot> slots;
    for (const Record& record : records_)
        if (std::ranges::find(slots, kast::slotOfId(record.id)) == slots.end())
            slots.push_back(kast::slotOfId(record.id));
    if (settings_ && std::ranges::find(slots, settings_->slot) == slots.end())
        slots.push_back(settings_->slot);
    return slots;
}

std::vector<kast::EntityId> SceneState::ids() const
{
    std::vector<kast::EntityId> ids;
    for (const Record& record : records_)
        ids.push_back(record.id);
    return ids;
}

void SceneState::add(const kast::Scene& scene, kast::EntityId id, bool deep)
{
    Record* existing = find(id);
    if (existing && (existing->deep || !deep))
        return;
    const entt::entity entity = kast::findById(scene, id);
    Record record{.id = id, .deep = deep, .present = entity != entt::null};
    if (record.present) {
        record.copy = copyEntity(scene, entity, stash_, entt::null, deep);
        if (deep) {
            const entt::entity parent = scene.parent(entity);
            record.parent = parent == entt::null ? 0 : idOf(scene, parent);
            record.index = scene.siblingIndex(entity);
        }
    }
    if (!existing) {
        records_.push_back(record);
        return;
    }
    // Shallow -> deep: the subtree as it is now, with the root's earlier name and components.
    if (existing->present && record.present)
        syncEntity(stash_.registry(), existing->copy, stash_.registry(), record.copy);
    stash_.destroy(existing->copy);
    *existing = record;
}

void SceneState::capture(const kast::Scene& scene, const SceneScope& scope)
{
    for (kast::EntityId id : scope.entities)
        add(scene, id, scope.deep);
    if (scope.settings && !settings_)
        settings_ = Settings{scene.activeSlot(), scene.systems, scene.lighting, scene.sky, scene.postProcessing, scene.physics};
}

void SceneState::captureLike(const kast::Scene& scene, const SceneState& shape)
{
    for (const Record& record : shape.records_)
        add(scene, record.id, record.deep);
    capture(scene, {.settings = shape.coversSettings()});
}

void SceneState::merge(const SceneState& other)
{
    for (const Record& record : other.records_) {
        if (covers(record.id))
            continue;
        Record copy = record;
        if (record.present)
            copy.copy = copyEntity(other.stash_, record.copy, stash_, entt::null, record.deep);
        records_.push_back(copy);
    }
    if (!settings_)
        settings_ = other.settings_;
}

void SceneState::addAbsent(kast::EntityId id)
{
    if (!covers(id))
        records_.push_back({.id = id, .deep = true});
}

void SceneState::restore(kast::Scene& scene) const
{
    for (const Record& record : records_)
        if (record.deep)
            scene.destroy(kast::findById(scene, record.id));
    for (const Record& record : records_) {
        if (!record.deep || !record.present || !scene.findScene(kast::slotOfId(record.id)))
            continue;
        const entt::entity parent = record.parent == 0 ? entt::null : kast::findById(scene, record.parent);
        const entt::entity entity = copyEntity(stash_, record.copy, scene, parent, true);
        scene.setSiblingIndex(entity, record.index);
    }
    for (const Record& record : records_) {
        const entt::entity entity = kast::findById(scene, record.id);
        if (!record.deep && record.present && entity != entt::null)
            syncEntity(stash_.registry(), record.copy, scene.registry(), entity);
    }
    if (settings_ && scene.findScene(settings_->slot)) {
        kast::SceneSettings& target = scene.settingsOf(settings_->slot);
        // A lightmap bake isn't an undo step: undoing a settings edit made before it keeps the bake it wrote.
        const std::string baked = target.lighting.lightmaps.bakedPath;
        target.systems = settings_->systems;
        target.lighting = settings_->lighting;
        target.lighting.lightmaps.bakedPath = baked;
        target.sky = settings_->sky;
        target.postProcessing = settings_->postProcessing;
        target.physics = settings_->physics;
    }
}

Json SceneState::describe() const
{
    Json json = Json::array();
    for (const Record& record : records_) {
        Json entry = {record.id, record.deep, record.present, record.parent, record.index};
        if (record.present)
            entry.push_back(describeEntity(stash_, record.copy, record.deep));
        json.push_back(std::move(entry));
    }
    if (settings_) {
        kast::Scene scene; // only for its settings' scene-file form
        scene.systems = settings_->systems;
        scene.lighting = settings_->lighting;
        scene.sky = settings_->sky;
        scene.postProcessing = settings_->postProcessing;
        scene.physics = settings_->physics;
        json.push_back(kast::sceneToJson(scene));
    }
    return json;
}

SceneState captureScene(const kast::Scene& scene, const SceneScope& scope)
{
    SceneState state;
    state.capture(scene, scope);
    return state;
}

namespace {

// Puts state back into doc.scene and selects the first of its entities that exists (none: no selection; a settings-only
// state keeps the selection). prefabSaves: doc.prefabSaves when the step was recorded; if a prefab was saved since,
// the copies' instances are as their prefab was then, so instances reload from the files.
void applySceneState(Document& doc, const SceneState& state, uint32_t prefabSaves)
{
    state.restore(doc.scene);
    if (prefabSaves != doc.prefabSaves && state.hasPrefabInstances())
        reloadInstances(doc); // ponytail: reloads the whole scene, only after a prefab save
    for (kast::EntityId id : state.ids())
        if (selectById(doc, id))
            break;
    ++doc.sceneGeneration;
}

void commitSceneEdit(Document& doc)
{
    SceneRecording recording = std::move(*doc.recording);
    doc.recording.reset();
    std::shared_ptr<SceneState> before = std::move(recording.before);
    for (kast::EntityId id : recording.added)
        before->addAbsent(id);
    auto after = std::make_shared<SceneState>();
    after->captureLike(doc.scene, *before);
    if (after->describe() == before->describe()) {
        updateDirty(doc); // a no-op edit may have called markDirty
        return;
    }
    // The step becomes the head of every scene it touched; undo puts back the heads it replaced.
    const uint64_t serial = ++doc.undoSerial;
    std::map<kast::SceneSlot, uint64_t> previous;
    for (kast::SceneSlot slot : after->scenes())
        previous[slot] = std::exchange(doc.sceneSaves[slot].head, serial);
    const uint32_t saves = doc.prefabSaves;
    doc.history.push({std::move(recording.name),
                      [&doc, before, saves, previous] {
                          applySceneState(doc, *before, saves);
                          for (const auto& [slot, head] : previous)
                              doc.sceneSaves[slot].head = head;
                      },
                      [&doc, after, saves, previous, serial] {
                          applySceneState(doc, *after, saves);
                          for (const auto& [slot, head] : previous)
                              doc.sceneSaves[slot].head = serial;
                      }});
    updateDirty(doc);
}

} // namespace

void beginSceneEdit(Document& doc, std::string name, const SceneScope& scope)
{
    if (doc.play != PlayState::Stopped)
        return;
    if (!doc.recording)
        doc.recording = SceneRecording{std::move(name), std::make_shared<SceneState>()};
    doc.recording->before->capture(doc.scene, scope);
    ++doc.recording->depth;
}

void endSceneEdit(Document& doc, const std::vector<kast::EntityId>& added)
{
    if (!doc.recording || doc.recording->depth == 0)
        return; // Play mode: nothing began
    doc.recording->added.insert(doc.recording->added.end(), added.begin(), added.end());
    if (--doc.recording->depth == 0 && !doc.recording->gesture)
        commitSceneEdit(doc);
}

void sceneGesture(Document& doc, std::string name, const SceneScope& scope, const SceneState& before)
{
    if (doc.play != PlayState::Stopped)
        return;
    if (!doc.recording)
        doc.recording = SceneRecording{std::move(name), std::make_shared<SceneState>()};
    doc.recording->gesture = true;
    doc.recording->before->merge(before);
    doc.recording->before->capture(doc.scene, scope); // parts the caller didn't capture: as they are now
}

void endSceneGesture(Document& doc)
{
    if (!doc.recording || !doc.recording->gesture)
        return;
    doc.recording->gesture = false;
    if (doc.recording->depth == 0)
        commitSceneEdit(doc);
}

void undoScene(Document& doc)
{
    if (doc.play != PlayState::Stopped)
        return;
    endSceneGesture(doc);
    if (doc.history.canUndo())
        kast::log::info(std::format("Undo {}", doc.history.undoName()));
    doc.history.undo();
    updateDirty(doc);
}

void redoScene(Document& doc)
{
    if (doc.play != PlayState::Stopped)
        return;
    endSceneGesture(doc);
    if (doc.history.canRedo())
        kast::log::info(std::format("Redo {}", doc.history.redoName()));
    doc.history.redo();
    updateDirty(doc);
}

void clearSceneHistory(Document& doc)
{
    doc.recording.reset();
    doc.history.clear(!doc.dirty);
}
