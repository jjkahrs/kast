#pragma once

#include <scene.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Document;

// Scene undo (docs/editor/undo-redo.html): each edit is a step on doc.history holding copies of what it touched, taken
// before and after it. Entities are named by kast::EntityId, never by entt::entity (handles go stale).

// What a step covers. entities: deep = each one's whole subtree and its place in the hierarchy (create, delete,
// reparent, prefab Revert All / Unpack); shallow = just its name and components (Inspector fields, gizmo, rename).
// settings: the scene's Lua systems, lighting, sky, post processing and physics.
struct SceneScope {
    std::vector<kast::EntityId> entities;
    bool deep = false;
    bool settings = false;
};

// Copies of what a scope covered at one moment, kept aside to put back. Move-only.
class SceneState {
public:
    // Adds scope's entities and settings not captured yet (an entity missing from scene is recorded as absent). A
    // shallow capture of an entity becomes deep, keeping its captured components.
    void capture(const kast::Scene& scene, const SceneScope& scope);
    // Adds the same entities and settings as shape, as they are in scene now.
    void captureLike(const kast::Scene& scene, const SceneState& shape);
    // Adds the copies other has and this doesn't.
    void merge(const SceneState& other);
    // Records id as absent (deep): an entity created after this state.
    void addAbsent(kast::EntityId id);
    bool covers(kast::EntityId id) const;
    bool coversSettings() const { return settings_.has_value(); }
    std::vector<kast::EntityId> ids() const; // in capture order
    // Puts the copies back: deep entities are rebuilt with their ids where they were (or removed if absent), shallow
    // ones get their name and components back.
    void restore(kast::Scene& scene) const;
    // Everything captured as JSON, to compare two states.
    nlohmann::ordered_json describe() const;

private:
    struct Record {
        kast::EntityId id = 0;
        bool deep = false;
        bool present = false;
        kast::EntityId parent = 0; // deep: 0 = a root
        size_t index = 0;          // deep: among its siblings
        entt::entity copy = entt::null; // in stash_
    };
    struct Settings {
        std::vector<std::string> systems;
        kast::SceneLighting lighting;
        kast::SceneSky sky;
        kast::ScenePostProcessing postProcessing;
        kast::ScenePhysics physics;
    };
    Record* find(kast::EntityId id);
    void add(const kast::Scene& scene, kast::EntityId id, bool deep);

    std::vector<Record> records_;
    kast::Scene stash_; // the copies, roots only
    std::optional<Settings> settings_;
};

// The step being recorded (Document::recording).
struct SceneRecording {
    std::string name;
    std::shared_ptr<SceneState> before;
    std::vector<kast::EntityId> added;
    int depth = 0;        // open beginSceneEdit calls
    bool gesture = false; // open until endSceneGesture
};

// Every in-memory scene edit is wrapped in beginSceneEdit / endSceneEdit; nested pairs make one step named by the
// outermost. Nothing is recorded during Play. markDirty outside a recording means an edit that is no step: the save
// point is lost.
// Starts (or joins) the step: captures the parts of scope not captured yet, as they are now.
void beginSceneEdit(Document& doc, std::string name, const SceneScope& scope = {});
// added: entities the edit created. The outermost call captures everything again and pushes the step if anything
// changed; if nothing did, dirty goes back to whether the history is at its save point.
void endSceneEdit(Document& doc, const std::vector<kast::EntityId>& added = {});
// A drag, slider or text field changed the scene this frame: before is scope as the caller captured it before the
// change (captureScene each frame). The first call opens the step; endSceneGesture closes it.
void sceneGesture(Document& doc, std::string name, const SceneScope& scope, const SceneState& before);
// Closes an open gesture step. main calls it each frame once no ImGui item or gizmo is active; Play, undo and save call
// it first.
void endSceneGesture(Document& doc);
SceneState captureScene(const kast::Scene& scene, const SceneScope& scope);

// Undo / redo on doc.history: reselects the step's entity by id (clears the selection if it is gone), bumps
// sceneGeneration and sets dirty from the save point. Nothing during Play.
void undoScene(Document& doc);
void redoScene(Document& doc);
// Drops doc.history and any open recording (New, Open, prefab open / leave, ops that write files and change the scene).
// The save point is the scene as it is now if it isn't dirty.
void clearSceneHistory(Document& doc);
