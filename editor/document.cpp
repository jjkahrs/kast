#include "document.h"

#include "assets.h"
#include "build.h"

#include <animation.h>
#include <app.h>
#include <log.h>
#include <lua_components.h>
#include <scene_io.h>
#include <play.h>
#include <prefab.h>
#include <state_machines.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace {

constexpr std::string_view kInvalidFolderChars = R"(<>:"/\|?*)"; // Windows; control characters are checked separately

bool isAsciiSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && isAsciiSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isAsciiSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

bool isInvalidFolderChar(char c)
{
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7f || kInvalidFolderChars.find(c) != std::string_view::npos;
}

constexpr SDL_DialogFileFilter kSceneFilter{"Kast scene (*.scene)", "scene"};
constexpr SDL_DialogFileFilter kPemFilter{"PEM certificate (*.pem, *.crt)", "pem;crt"};
constexpr bx::Vec3 kNewLightEuler{-50.0f, -30.0f, 0.0f}; // ~50 deg down, slightly from the side so faces shade differently
constexpr bx::Vec3 kMainCameraPosition{0.0f, 3.0f, 7.0f};
constexpr bx::Vec3 kMainCameraEuler{-20.0f, 0.0f, 0.0f}; // looks down at the origin from kMainCameraPosition

constexpr const char* kSystemTemplate = R"(-- A Lua system: update(dt) runs once per frame in Play mode, after the C++ systems.
-- See docs/engine/lua-systems.html for the full kast.* API.
local elapsed = 0

return {
    -- init = function() kast.log("started") end,
    update = function(dt)
        elapsed = elapsed + dt
        -- for e in kast.query("Transform", "MeshRenderer") do
        --     local t = kast.get(e, "Transform")       -- {position = {x=, y=, z=}, rotation = {x=, y=, z=, w=}, scale = ...}
        --     t.position.y = 0.5 + math.sin(elapsed)
        --     kast.set(e, "Transform", {position = t.position}) -- partial: other fields keep their values
        -- end
        -- if kast.input.keyDown("Space") then kast.log("jump") end
        -- local dx, dy = kast.input.mouseDelta()
        -- local id = kast.create("Spawned"); kast.add(id, "MeshRenderer"); kast.destroy(id)
    end,
    -- shutdown = function() kast.log("stopped") end,
}
)";

constexpr const char* kComponentTemplate = R"(-- A Lua component: a data-only component type. The file returns one table; see docs/engine/lua-components.html.
return {
    name = "%NAME%",              -- type name and scene-file key; unique, letters/digits/_
    -- displayName = "%NAME%",    -- Inspector / Add Component label (defaults to name)
    fields = {                     -- in save and Inspector order
        { name = "speed", type = "float", default = 1, min = 0 },
        -- { name = "count",   type = "int",    default = 3, min = 0, max = 10 },
        -- { name = "active",  type = "bool",   default = true },
        -- { name = "label",   type = "string", default = "hello" },
        -- { name = "offset",  type = "vec3",   default = {x = 0, y = 1, z = 0} },
        -- { name = "tint",    type = "vec3",   color = true, default = {x = 1, y = 1, z = 1} },
        -- { name = "facing",  type = "quat" },                        -- defaults to identity
        -- { name = "mode",    type = "enum",   values = {"idle", "busy"}, default = "idle" },
        -- { name = "tags",    type = "array",  of = "string", default = {"a", "b"} },
        -- { name = "weights", type = "array",  of = "float", min = 0, max = 1 },
    },
}
)";

constexpr const char* kStateMachineTemplate = R"(-- A state machine: named states, entered one at a time. The file returns one table; see docs/engine/state-machines.html.
-- Use it as a game machine (State Machines panel > Global) or on entities (StateMachine component).
return {
    name = "%NAME%",       -- unique machine name: kast.fsm.* targets and StateMachine "machine"
    initial = "idle",      -- the state entered at Play start
    states = {
        idle = {
            -- ctx = {machine = "%NAME%", entity = id or nil (game machine), from = "..." or nil (first state), to = "..."}
            enter = function(ctx) kast.log("%NAME%: " .. tostring(ctx.from) .. " -> " .. ctx.to) end,
            -- exit = function(ctx) end,
        },
        active = { allowedFrom = {"idle"} },   -- may only be entered from idle
        -- paused = { allowedFrom = {"*"} },  -- from any state (also when allowedFrom is omitted)
    },
}
)";

std::optional<std::filesystem::path> rootOf(const std::optional<kast::Project>& project)
{
    return project ? std::optional(project->root) : std::nullopt;
}

std::optional<kast::Project> projectOf(const std::string& sceneFile)
{
    return kast::discoverProject(std::filesystem::absolute(sceneFile).parent_path());
}

// Writes doc's scene with its editor camera in editorData; no project or document state changes.
bool writeScene(Document& doc, const std::string& path, std::string& error)
{
    doc.scene.editorData["camera"] = doc.camera.toJson();
    return kast::saveScene(doc.scene, path, error);
}

} // namespace

const char* objectKindLabel(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Empty: return "Create Empty";
    case ObjectKind::Cube: return "Cube";
    case ObjectKind::Plane: return "Plane";
    case ObjectKind::Sphere: return "Sphere";
    case ObjectKind::Model: return "Model";
    case ObjectKind::DirectionalLight: return "Directional Light";
    case ObjectKind::PointLight: return "Point Light";
    case ObjectKind::Camera: return "Camera";
    }
    return "";
}

entt::entity createObject(Document& doc, ObjectKind kind, entt::entity parent)
{
    kast::Scene& scene = doc.scene;
    const char* name = kind == ObjectKind::Empty ? "Empty" : objectKindLabel(kind);
    beginSceneEdit(doc, std::format("Create {}", name));
    const entt::entity entity = scene.create(name, parent);
    if (entity == entt::null) {
        endSceneEdit(doc);
        return entity; // parent is at the hierarchy depth limit
    }
    switch (kind) {
    case ObjectKind::Cube: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Cube); break;
    case ObjectKind::Plane: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Plane); break;
    case ObjectKind::Sphere: scene.registry().emplace<kast::MeshRenderer>(entity, kast::Primitive::Sphere); break;
    case ObjectKind::Model: scene.registry().emplace<kast::ModelRenderer>(entity); break; // pick one in the Inspector
    case ObjectKind::DirectionalLight:
        scene.registry().emplace<kast::DirectionalLight>(entity);
        scene.registry().get<kast::Transform>(entity).rotation = kast::quatFromEulerDeg(kNewLightEuler);
        break;
    case ObjectKind::PointLight: scene.registry().emplace<kast::PointLight>(entity); break;
    case ObjectKind::Camera: scene.registry().emplace<kast::Camera>(entity); break;
    case ObjectKind::Empty: break;
    }
    doc.selected = entity;
    markDirty(doc);
    endSceneEdit(doc, {scene.registry().get<kast::PersistentId>(entity).value});
    return entity;
}

entt::entity createModelEntity(Document& doc, const std::string& path, entt::entity parent)
{
    const std::string name = std::filesystem::path(path).stem().string();
    beginSceneEdit(doc, std::format("Create {}", name));
    const entt::entity entity = doc.scene.create(name, parent);
    if (entity == entt::null) {
        endSceneEdit(doc);
        return entity; // parent is at the hierarchy depth limit
    }
    doc.scene.registry().emplace<kast::ModelRenderer>(entity, path);
    if (doc.project)
        kast::models().setRoot(doc.project->root);
    if (const auto model = kast::models().get(path); model && !model->clips.empty())
        doc.scene.registry().emplace<kast::Animator>(entity, kast::defaultClip(*model));
    doc.selected = entity;
    markDirty(doc);
    endSceneEdit(doc, {doc.scene.registry().get<kast::PersistentId>(entity).value});
    return entity;
}

void deleteSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    const entt::registry& registry = doc.scene.registry();
    beginSceneEdit(doc, std::format("Delete {}", registry.get<kast::Name>(doc.selected).value),
                   {.entities = {registry.get<kast::PersistentId>(doc.selected).value}, .deep = true});
    doc.scene.destroy(doc.selected);
    doc.selected = entt::null;
    markDirty(doc);
    endSceneEdit(doc);
}

void duplicateSelected(Document& doc)
{
    if (!doc.scene.valid(doc.selected))
        return;
    beginSceneEdit(doc, std::format("Duplicate {}", doc.scene.registry().get<kast::Name>(doc.selected).value));
    doc.selected = kast::duplicateLinked(doc.scene, doc.selected);
    markDirty(doc);
    endSceneEdit(doc, {doc.scene.registry().get<kast::PersistentId>(doc.selected).value});
}

void markDirty(Document& doc)
{
    if (doc.play != PlayState::Stopped)
        return;
    doc.dirty = true;
    if (!doc.recording)
        doc.history.forgetSavePoint();
}

bool selectById(Document& doc, kast::EntityId id)
{
    doc.selected = entt::null;
    for (auto [entity, persistent] : doc.scene.registry().view<kast::PersistentId>().each())
        if (persistent.value == id)
            doc.selected = entity;
    return doc.selected != entt::null;
}

void startPlay(Document& doc)
{
    if (doc.prefabEdit) {
        kast::log::warn("Play is disabled while a prefab is open: go Back to the scene first");
        return;
    }
    if (doc.play == PlayState::Stopped) {
        endSceneGesture(doc); // a gesture still open is an edit-mode step
        doc.playSnapshot = kast::sceneToJson(doc.scene);
        doc.playSnapshotDirty = doc.dirty;
        doc.focusRequest = PanelFocus::Game;
        kast::log::info("Play");
        if (kast::activeCamera(doc.scene) == entt::null)
            kast::log::warn("No Camera in scene: the Game view has nothing to render");
        doc.playSceneName.clear();
        ++doc.playGeneration;
        doc.scene.registry().clear<kast::AnimatorState>(); // Play starts clips from 0, not from the Edit preview
        std::optional<kast::AuthConfig> auth;
        if (std::string error; doc.project && !(auth = kast::authConfig(*doc.project, error)) && !error.empty())
            kast::log::error(std::format("Online Services: {}; kast.auth is off for this Play session", error));
        doc.lua.start(doc.scene, doc.project ? std::optional(doc.project->root) : std::nullopt, projectScenePath(doc),
                      &doc.input, doc.project ? doc.project->stateMachines.value_or(std::vector<std::string>{})
                                              : std::vector<std::string>{},
                      std::move(auth));
        if (doc.project)
            doc.lua.audio().setMix(doc.project->audio);
        if (doc.onPlayStarted)
            doc.onPlayStarted(doc.lua);
    }
    doc.play = PlayState::Playing;
    doc.lua.audio().setPaused(false);
}

void togglePause(Document& doc)
{
    if (doc.play == PlayState::Stopped)
        return;
    doc.play = doc.play == PlayState::Paused ? PlayState::Playing : PlayState::Paused;
    doc.lua.audio().setPaused(doc.play == PlayState::Paused); // holds every voice where it is
}

void stopPlay(Document& doc)
{
    if (doc.play == PlayState::Stopped)
        return;
    if (doc.onPlayStopping)
        doc.onPlayStopping(doc.lua);
    doc.lua.stop(); // shutdowns see the play-mode scene
    // Ids are only unique per scene file: after a kast.loadScene switch the selection's id means nothing in the snapshot.
    const kast::EntityId selected = doc.playSceneName.empty() && doc.scene.valid(doc.selected)
                                        ? doc.scene.registry().get<kast::PersistentId>(doc.selected).value
                                        : 0;
    std::string error;
    if (!kast::sceneFromJson(doc.playSnapshot, doc.scene, error, nullptr, rootOf(doc.project).value_or(std::filesystem::path())))
        kast::log::error(std::format("Cannot restore the scene from before Play: {}", error)); // keeps the play-mode scene
    doc.play = PlayState::Stopped;
    doc.playSnapshot = nullptr;
    doc.playSceneName.clear();
    doc.dirty = doc.playSnapshotDirty;
    selectById(doc, selected);
    ++doc.sceneGeneration;
    doc.focusRequest = PanelFocus::Scene;
    kast::log::info("Stop");
}

void updatePlay(Document& doc, float dt)
{
    if (doc.play == PlayState::Stopped) {
        // Edit mode previews animation (playOnStart clips, the Inspector's Play/Pause and scrub). AnimatorState is
        // never saved and doesn't dirty the scene.
        kast::animatorSystem(doc.scene, dt);
        return;
    }
    if (doc.play != PlayState::Playing)
        return;
    if (kast::tickPlay(doc.scene, doc.lua, dt)) { // kast.loadScene replaced doc.scene
        doc.selected = entt::null;
        ++doc.playGeneration;
        ++doc.sceneGeneration;
        doc.playSceneName = std::filesystem::path(doc.lua.scenePath()).stem().string();
    }
}

std::string projectScenePath(const Document& doc)
{
    if (!doc.project || doc.path.empty())
        return {};
    return kast::toProjectPath(doc.project->root, doc.path).value_or("");
}

bool canEditSystems(const Document& doc) { return doc.play == PlayState::Stopped; }

bool addSystem(Document& doc, const std::filesystem::path& script, std::string& error)
{
    if (!doc.project) {
        error = "Open a scene inside a project to add Lua systems";
        return false;
    }
    const std::optional<std::string> path = kast::toProjectPath(doc.project->root, script);
    if (!path) {
        error = std::format("'{}' is outside the project '{}'", script.string(), doc.project->root.string());
        return false;
    }
    beginSceneEdit(doc, "Add Lua System", {.settings = true});
    doc.scene.systems.push_back(*path);
    markDirty(doc);
    endSceneEdit(doc);
    return true;
}

void removeSystem(Document& doc, size_t index)
{
    beginSceneEdit(doc, "Remove Lua System", {.settings = true});
    doc.scene.systems.erase(doc.scene.systems.begin() + static_cast<ptrdiff_t>(index));
    markDirty(doc);
    endSceneEdit(doc);
}

void moveSystem(Document& doc, size_t index, int delta)
{
    const ptrdiff_t target = static_cast<ptrdiff_t>(index) + delta;
    if (target < 0 || target >= std::ssize(doc.scene.systems))
        return;
    beginSceneEdit(doc, "Move Lua System", {.settings = true});
    std::swap(doc.scene.systems[index], doc.scene.systems[static_cast<size_t>(target)]);
    markDirty(doc);
    endSceneEdit(doc);
}

void remapScenePaths(Document& doc, const std::string& from, const std::string& to)
{
    bool changed = remapProjectPaths(doc.scene.systems, from, to);
    for (auto [entity, mesh] : doc.scene.registry().view<kast::MeshRenderer>().each()) {
        std::vector<std::string> path = {mesh.material};
        if (remapProjectPaths(path, from, to)) {
            mesh.material = path[0];
            changed = true;
        }
    }
    for (auto [entity, model] : doc.scene.registry().view<kast::ModelRenderer>().each()) {
        std::vector<std::string> path = {model.model};
        const bool moved = remapProjectPaths(path, from, to);
        model.model = path[0];
        const bool overrides = remapProjectPaths(model.materials, from, to);
        changed = changed || moved || overrides;
    }
    // Instances (also in the scene waiting behind prefab edit mode, which is dirtied with it); refreshPrefabs then
    // reloads them from the moved files.
    const auto remapInstances = [&](kast::Scene& scene) {
        bool changed = false;
        for (auto [entity, instance] : scene.registry().view<kast::PrefabInstance>().each()) {
            std::vector<std::string> path = {instance.path};
            if (remapProjectPaths(path, from, to)) {
                instance.path = path[0];
                if (instance.placeholder.is_object())
                    instance.placeholder["prefab"] = path[0];
                changed = true;
            }
        }
        return changed;
    };
    // The files moved, so the histories' copies would put the old paths back: they are dropped.
    if (remapInstances(doc.scene) || changed) {
        markDirty(doc);
        clearSceneHistory(doc);
    }
    if (doc.prefabEdit) {
        if (remapInstances(doc.prefabEdit->scene)) {
            doc.prefabEdit->dirty = true;
            doc.prefabEdit->history.clear(false);
        }
        std::vector<std::string> paths = {doc.prefabEdit->prefab};
        if (remapProjectPaths(paths, from, to))
            doc.prefabEdit->prefab = paths[0];
        if (std::optional<std::string> scene = doc.project ? kast::toProjectPath(doc.project->root, doc.prefabEdit->path)
                                                           : std::nullopt;
            scene && remapProjectPaths(paths = {*scene}, from, to))
            doc.prefabEdit->path = (doc.project->root / fromUtf8(paths[0])).string();
    }
}

const char* prefabEditBlocker(const Document& doc)
{
    return !doc.project                      ? "Open a project to use prefabs"
           : doc.play != PlayState::Stopped ? "Stop Play mode to change prefabs"
                                            : nullptr;
}

bool createPrefabFrom(Document& doc, entt::entity entity, const std::string& folder, std::string& path, std::string& error)
{
    if (const char* blocker = prefabEditBlocker(doc)) {
        error = blocker;
        return false;
    }
    if (!doc.scene.valid(entity)) {
        error = "Select an entity to make a prefab of";
        return false;
    }
    if (doc.prefabEdit && doc.scene.parent(entity) == entt::null) {
        error = "The open prefab's root can't itself become a prefab instance";
        return false;
    }
    const std::filesystem::path dir = folder.empty() ? doc.project->root : doc.project->root / fromUtf8(folder);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = uniquePrefabName(dir, doc.scene.registry().get<kast::Name>(entity).value);
    path = folder.empty() ? name : folder + '/' + name;
    if (!kast::createPrefab(doc.scene, entity, path, doc.project->root, error))
        return false;
    doc.selected = entity;
    markDirty(doc);
    clearSceneHistory(doc); // it wrote a file
    return true;
}

entt::entity dropPrefab(Document& doc, const std::string& path, entt::entity parent, std::string& error)
{
    if (const char* blocker = prefabEditBlocker(doc)) {
        error = blocker;
        return entt::null;
    }
    std::shared_ptr<const kast::PrefabAsset> asset;
    if (!kast::loadPrefab(doc.project->root, path, asset, error))
        return entt::null;
    if (doc.prefabEdit && asset->contains(doc.prefabEdit->prefab)) {
        error = std::format("'{}' {} '{}', the prefab being edited: a prefab can't contain itself", path,
                            path == doc.prefabEdit->prefab ? "is" : "contains", doc.prefabEdit->prefab);
        return entt::null;
    }
    beginSceneEdit(doc, std::format("Add {}", utf8(fromUtf8(path).stem())));
    const entt::entity entity = kast::instantiatePrefab(doc.scene, asset, parent, error);
    if (entity == entt::null) {
        endSceneEdit(doc);
        return entity;
    }
    doc.selected = entity;
    markDirty(doc);
    endSceneEdit(doc, {doc.scene.registry().get<kast::PersistentId>(entity).value});
    kast::log::info(std::format("Added an instance of '{}'", path));
    return entity;
}

namespace {

// refreshPrefabs without touching the histories.
void reloadInstances(Document& doc)
{
    if (!doc.project)
        return;
    const kast::EntityId selected = doc.scene.valid(doc.selected) ? doc.scene.registry().get<kast::PersistentId>(doc.selected).value : 0;
    std::string error;
    if (!kast::reloadPrefabs(doc.scene, doc.project->root, error))
        kast::log::error(std::format("Cannot refresh the prefab instances: {}", error));
    if (doc.prefabEdit && !kast::reloadPrefabs(doc.prefabEdit->scene, doc.project->root, error))
        kast::log::error(std::format("Cannot refresh the prefab instances of the waiting scene: {}", error));
    selectById(doc, selected);
    ++doc.sceneGeneration;
}

bool hasInstances(const kast::Scene& scene) { return !scene.registry().view<kast::PrefabInstance>().empty(); }

} // namespace

void refreshPrefabs(Document& doc)
{
    if (!doc.project)
        return;
    reloadInstances(doc);
    // The histories' copies hold the instances as their prefabs were.
    if (hasInstances(doc.scene))
        clearSceneHistory(doc);
    if (doc.prefabEdit && hasInstances(doc.prefabEdit->scene))
        doc.prefabEdit->history.clear(!doc.prefabEdit->dirty);
}

bool openPrefab(Document& doc, const std::string& path, std::string& error)
{
    if (const char* blocker = prefabEditBlocker(doc)) {
        error = blocker;
        return false;
    }
    kast::Scene prefab;
    const std::optional<std::filesystem::path> file = kast::resolveProjectPath(doc.project->root, path, error);
    if (!file || !kast::loadPrefabScene(doc.project->root, path, prefab, error))
        return false;
    endSceneGesture(doc);
    if (!doc.prefabEdit) {
        const kast::EntityId selected =
            doc.scene.valid(doc.selected) ? doc.scene.registry().get<kast::PersistentId>(doc.selected).value : 0;
        doc.prefabEdit = Document::PrefabEdit{std::move(doc.scene), doc.camera, doc.path, doc.dirty, selected, path,
                                              std::move(doc.history)};
    }
    doc.prefabEdit->prefab = path;
    doc.scene = std::move(prefab);
    doc.path = file->string();
    doc.dirty = false;
    clearSceneHistory(doc); // the prefab's own; a prefab open before is dropped
    doc.selected = entt::null;
    doc.camera = EditorCamera{.pivot = {0.0f, 0.5f, 0.0f}, .yaw = 30.0f, .pitch = -25.0f, .distance = 7.0f};
    ++doc.sceneGeneration;
    kast::log::info(std::format("Editing prefab '{}'", path));
    return true;
}

void closePrefab(Document& doc)
{
    if (!doc.prefabEdit)
        return;
    Document::PrefabEdit waiting = std::move(*doc.prefabEdit);
    doc.prefabEdit.reset();
    doc.scene = std::move(waiting.scene);
    doc.camera = waiting.camera;
    doc.path = waiting.path;
    doc.dirty = waiting.dirty;
    doc.recording.reset();
    doc.history = std::move(waiting.history); // the prefab's is dropped
    selectById(doc, waiting.selected);
    reloadInstances(doc); // bumps sceneGeneration
    kast::log::info(std::format("Back to scene '{}' from prefab '{}'", displayName(doc), waiting.prefab));
}

bool runPrefabOp(Document& doc, const PrefabOp& op, std::string& error)
{
    if (const char* blocker = prefabEditBlocker(doc)) {
        error = blocker;
        return false;
    }
    const entt::entity entity = kast::findById(doc.scene, op.entity);
    if (entity == entt::null || !doc.scene.registry().all_of<kast::PrefabLink>(entity)) {
        error = std::format("no prefab instance entity with id {}", op.entity);
        return false;
    }
    using Kind = PrefabOp::Kind;
    const bool root = kast::isPrefabRoot(doc.scene, entity);
    if (!root && (op.kind == Kind::RevertDeleted || op.kind == Kind::RevertAll || op.kind == Kind::ApplyDeleted ||
                  op.kind == Kind::ApplyAll || op.kind == Kind::Unpack)) {
        error = std::format("entity {} is not a prefab instance root", op.entity);
        return false;
    }
    const std::filesystem::path& projectRoot = doc.project->root;
    // Revert* and Unpack are scene steps; Apply* writes prefab files, so it drops the history instead.
    const bool apply = op.kind >= Kind::ApplyField && op.kind <= Kind::ApplyAll;
    const std::string& name = doc.scene.registry().get<kast::Name>(entity).value;
    const bool field = op.kind == Kind::RevertField || op.kind == Kind::RevertComponent;
    if (!apply)
        beginSceneEdit(doc,
                       op.kind == Kind::Unpack ? std::format("Unpack {}", name)
                       : field                 ? std::format("Revert {}{}{}", op.component, op.field.empty() ? "" : ".", op.field)
                                               : std::format("Revert {}", name),
                       {.entities = {op.entity}, .deep = !field});
    bool ok = true;
    switch (op.kind) {
    case Kind::RevertField:
        ok = kast::revertPrefabField(doc.scene, entity, op.component, op.field);
        break;
    case Kind::RevertComponent:
        ok = kast::revertPrefabComponent(doc.scene, entity, op.component);
        break;
    case Kind::RevertDeleted:
        ok = kast::revertDeletedChild(doc.scene, entity, op.local, projectRoot, error);
        break;
    case Kind::RevertAll:
        ok = kast::revertAllOverrides(doc.scene, entity, projectRoot, error);
        break;
    case Kind::ApplyField:
    case Kind::ApplyComponent:
        ok = kast::applyPrefabOverride(doc.scene, entity, op.component, op.kind == Kind::ApplyField ? op.field : "",
                                       op.hop, projectRoot, error);
        break;
    case Kind::ApplyDeleted:
        ok = kast::applyDeletedChild(doc.scene, entity, op.local, projectRoot, error);
        break;
    case Kind::ApplyAll:
        ok = kast::applyAllOverrides(doc.scene, entity, projectRoot, error);
        break;
    case Kind::Unpack:
        kast::unpackPrefab(doc.scene, entity);
        kast::log::info(std::format("Unpacked '{}'", doc.scene.registry().get<kast::Name>(entity).value));
        break;
    }
    if (!ok) {
        if (error.empty())
            error = "nothing to revert";
        if (!apply)
            endSceneEdit(doc);
        return false;
    }
    markDirty(doc);
    if (apply) {
        refreshPrefabs(doc); // prefab files changed
        clearSceneHistory(doc);
    } else {
        if (op.kind == Kind::RevertDeleted || op.kind == Kind::RevertAll) // the scene was reloaded
            reloadInstances(doc);
        endSceneEdit(doc);
    }
    selectById(doc, op.entity);
    return true;
}

std::string breadcrumb(const Document& doc)
{
    if (!doc.prefabEdit)
        return displayName(doc);
    const std::string scene =
        doc.prefabEdit->path.empty() ? "Untitled" : std::filesystem::path(doc.prefabEdit->path).stem().string();
    return std::format("{} > {}", scene, utf8(fromUtf8(doc.prefabEdit->prefab).filename()));
}

bool newSystemFromTemplate(Document& doc, std::string& error)
{
    if (!doc.project) {
        error = "Open a scene inside a project to add Lua systems";
        return false;
    }
    if (doc.path.empty()) {
        error = "Save the scene first";
        return false;
    }
    std::filesystem::path file;
    if (!writeNewSystem(doc.project->root / "scripts", file, error))
        return false;
    if (!addSystem(doc, file, error))
        return false;
    kast::log::info(std::format("Created Lua system '{}'", file.string()));
    return true;
}

void resetToDefault(Document& doc)
{
    doc.scene = kast::Scene{};
    doc.prefabEdit.reset();
    entt::registry& registry = doc.scene.registry();

    const entt::entity light = createObject(doc, ObjectKind::DirectionalLight, entt::null);
    registry.get<kast::Transform>(light).position = {0.0f, 3.0f, 0.0f};
    registry.get<kast::DirectionalLight>(light).color = {1.0f, 0.96f, 0.9f};

    const entt::entity ground = createObject(doc, ObjectKind::Plane, entt::null);
    registry.get<kast::Name>(ground).value = "Ground";
    registry.get<kast::Transform>(ground).scale = {10.0f, 1.0f, 10.0f};

    const entt::entity cube = createObject(doc, ObjectKind::Cube, entt::null);
    registry.get<kast::Transform>(cube).position = {0.0f, 0.5f, 0.0f}; // unit cube resting on y=0

    const entt::entity camera = createObject(doc, ObjectKind::Camera, entt::null);
    registry.get<kast::Name>(camera).value = "Main Camera";
    registry.get<kast::Transform>(camera).position = kMainCameraPosition;
    registry.get<kast::Transform>(camera).rotation = kast::quatFromEulerDeg(kMainCameraEuler);

    doc.camera = EditorCamera{.pivot = {0.0f, 0.5f, 0.0f}, .yaw = 30.0f, .pitch = -25.0f, .distance = 7.0f};
    doc.path.clear();
    doc.selected = entt::null;
    doc.dirty = false;
    clearSceneHistory(doc);
    ++doc.sceneGeneration;
}

bool openFile(Document& doc, const std::string& path, std::string& error)
{
    // The scene's project's Lua components must be active before it loads; the current scene is replaced below.
    std::optional<kast::Project> project = projectOf(path);
    kast::activateComponentDefinitions(rootOf(project), nullptr);
    kast::Scene scene;
    kast::SceneLoadReport report;
    if (!kast::loadScene(path, scene, error, &report, rootOf(project).value_or(std::filesystem::path()))) {
        if (kast::activateComponentDefinitions(rootOf(doc.project), &doc.scene)) { // back to the kept scene's project
            markDirty(doc);
            clearSceneHistory(doc); // migrated: the copies hold the old component data
        }
        return false;
    }
    EditorCamera camera;
    if (scene.editorData.contains("camera") && !camera.fromJson(scene.editorData["camera"]))
        kast::log::warn("Scene has an invalid editor camera; using the default view");
    doc.scene = std::move(scene);
    doc.camera = camera;
    doc.path = path;
    doc.project = std::move(project);
    doc.selected = entt::null;
    doc.dirty = report.migrated; // Lua component data fixed by the drift rules
    doc.prefabEdit.reset();
    clearSceneHistory(doc);
    ++doc.sceneGeneration;
    kast::log::info(std::format("Opened scene '{}'", path));
    return true;
}

bool saveFile(Document& doc, const std::string& path, std::string& error)
{
    endSceneGesture(doc);
    if (doc.prefabEdit) { // the open prefab, as a prefab: the project and the waiting scene stay
        const std::optional<std::string> prefab = kast::toProjectPath(doc.project->root, path);
        if (!prefab) {
            error = std::format("Cannot save prefab '{}': it is outside the project", path);
            return false;
        }
        if (!kast::savePrefab(doc.scene, doc.project->root, *prefab, error))
            return false;
        doc.dirty = false;
        doc.history.markSaved();
        kast::log::info(std::format("Saved prefab '{}'", *prefab));
        return true;
    }
    if (!writeScene(doc, path, error))
        return false;
    std::optional<kast::Project> project = projectOf(path);
    const auto root = [](const std::optional<kast::Project>& p) { return p ? p->root : std::filesystem::path(); };
    if (!doc.scene.systems.empty() && root(project) != root(doc.project))
        kast::log::warn(std::format("Saved into {}: the scene's Lua system paths are project-relative and may no longer "
                                    "resolve",
                                    project ? std::format("project '{}'", project->name) : "no project"));
    const bool projectChanged = root(project) != root(doc.project);
    doc.path = path;
    doc.project = std::move(project);
    doc.dirty = false;
    doc.history.markSaved();
    kast::log::info(std::format("Saved scene '{}'", path));
    if (projectChanged && kast::activateComponentDefinitions(rootOf(doc.project), &doc.scene)) {
        markDirty(doc); // the new project's Lua component definitions changed the scene
        clearSceneHistory(doc);
    }
    return true;
}

bool openProject(Document& doc, const std::filesystem::path& file, std::string& error)
{
    if (file.filename() != kast::kProjectFileName) {
        error = std::format("'{}' is not a {} file", file.string(), kast::kProjectFileName);
        return false;
    }
    kast::Project project;
    if (!kast::loadProject(file, project, error))
        return false;
    kast::log::info(std::format("Opened project '{}' ({})", project.name, project.root.string()));
    if (project.startScene) {
        std::string reason;
        const std::optional<std::filesystem::path> scene = kast::resolveProjectPath(project.root, *project.startScene, reason);
        if (scene && openFile(doc, scene->string(), reason))
            return true;
        kast::log::warn(std::format("Cannot open the project's startScene '{}': {}", *project.startScene, reason));
    }
    resetToDefault(doc);
    doc.project = std::move(project);
    kast::activateComponentDefinitions(rootOf(doc.project), &doc.scene); // the default scene has no Lua components
    return true;
}

bool canEditComponents(const Document& doc) { return doc.play == PlayState::Stopped && doc.project; }

void refreshComponents(Document& doc)
{
    if (doc.play == PlayState::Stopped && kast::refreshComponentDefinitions(doc.scene)) {
        markDirty(doc);
        clearSceneHistory(doc); // migrated: the copies hold the old component data
    }
}

std::filesystem::path newComponentFile(const std::filesystem::path& projectRoot, std::string& typeName)
{
    const std::filesystem::path dir = projectRoot / kast::kComponentsDir;
    std::filesystem::path file = dir / "new_component.lua";
    typeName = "NewComponent";
    for (int n = 2; std::filesystem::exists(file); ++n) {
        file = dir / std::format("new_component_{}.lua", n);
        typeName = std::format("NewComponent{}", n);
    }
    return file;
}

namespace {

bool writeTemplate(const std::filesystem::path& file, std::string text, const std::string& name, std::string& error);

// Writes kComponentTemplate for typeName to file (creating its folder), replacing any file there, and logs it.
bool writeComponentTemplate(const std::filesystem::path& file, const std::string& typeName, std::string& error)
{
    if (!writeTemplate(file, kComponentTemplate, typeName, error))
        return false;
    kast::log::info(std::format("Created Lua component '{}'", file.string()));
    return true;
}

} // namespace

bool newComponentFromTemplate(Document& doc, std::string& error)
{
    if (!canEditComponents(doc)) {
        error = doc.project ? "Stop Play to add components" : "Open a scene inside a project to add Lua components";
        return false;
    }
    std::string typeName;
    const std::filesystem::path file = newComponentFile(doc.project->root, typeName);
    if (!writeComponentTemplate(file, typeName, error))
        return false;
    refreshComponents(doc); // loads it now rather than at the next poll
    return true;
}

const char* componentEditBlocker(const Document& doc)
{
    return !doc.project                     ? "Open a scene inside a project"
           : doc.play != PlayState::Stopped ? "Stop Play to add components"
                                            : nullptr;
}

std::string componentFileStem(std::string_view typeName)
{
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto lowerOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    std::string stem;
    for (size_t i = 0; i < typeName.size(); ++i) {
        const char c = typeName[i];
        if (upper(c) && i > 0 &&
            (lowerOrDigit(typeName[i - 1]) ||
             (upper(typeName[i - 1]) && i + 1 < typeName.size() && typeName[i + 1] >= 'a' && typeName[i + 1] <= 'z')))
            stem += '_';
        stem += upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return stem;
}

namespace {

// The name of a file in file's folder whose name matches file's (lower-case) one case-insensitively, as on Windows,
// even in a case-sensitive folder; empty if none.
std::string existingFileName(const std::filesystem::path& file)
{
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        return text;
    };
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), ec))
        if (lower(entry.path().filename().string()) == file.filename().string())
            return entry.path().filename().string();
    return {};
}

// Writes text with %NAME% replaced by name to file (creating its folder), replacing any file there.
bool writeTemplate(const std::filesystem::path& file, std::string text, const std::string& name, std::string& error)
{
    for (size_t at; (at = text.find("%NAME%")) != std::string::npos;)
        text.replace(at, 6, name);
    return writeTextFile(file, text, error);
}

} // namespace

std::filesystem::path luaComponentFile(const std::filesystem::path& projectRoot, std::string_view typeName)
{
    return projectRoot / kast::kComponentsDir / (componentFileStem(trimmed(typeName)) + ".lua");
}

std::string newComponentNameError(const Document& doc, std::string_view name)
{
    if (const char* blocked = componentEditBlocker(doc))
        return blocked;
    const std::string typeName(trimmed(name));
    if (typeName.empty())
        return "Enter a name";
    if (!kast::isIdentifier(typeName))
        return "Use letters, digits and _ only, not starting with a digit";
    if (kast::isBuiltInComponentName(typeName))
        return std::format("\"{}\" is a built-in component", typeName);
    // Every definition file of the active project, loaded or in error (loaded types are a subset).
    if (std::ranges::any_of(kast::componentDefinitionStatus(),
                            [&](const kast::ComponentDefinitionStatus& s) { return s.name == typeName; }))
        return std::format("A Lua component named \"{}\" already exists", typeName);
    if (const std::string existing = existingFileName(luaComponentFile(doc.project->root, typeName)); !existing.empty())
        return std::format("{}/{} already exists", kast::kComponentsDir, existing);
    return {};
}

bool newLuaComponent(Document& doc, entt::entity entity, std::string_view name, std::string& error)
{
    if (!doc.scene.valid(entity)) {
        error = "Select an entity";
        return false;
    }
    if (error = newComponentNameError(doc, name); !error.empty())
        return false;
    const std::string typeName(trimmed(name));
    const std::filesystem::path file = luaComponentFile(doc.project->root, typeName);
    if (!writeComponentTemplate(file, typeName, error))
        return false;
    refreshComponents(doc); // loads it now rather than at the next poll
    const kast::ComponentType type = kast::findAnyComponentType(typeName);
    if (!type.lua()) {
        error = std::format("Created '{}' but it did not load (see Console)", file.string());
        return false;
    }
    kast::addComponent(doc.scene.registry(), entity, type);
    markDirty(doc);
    clearSceneHistory(doc); // it wrote a file
    return true;
}

void refreshStateMachines(Document& doc) { kast::refreshStateMachineDefinitions(rootOf(doc.project)); }

const char* stateMachineEditBlocker(const Document& doc)
{
    return !doc.project                     ? "Open a scene inside a project"
           : doc.play != PlayState::Stopped ? "Stop Play to change state machines"
                                            : nullptr;
}

std::filesystem::path stateMachineFile(const std::filesystem::path& projectRoot, std::string_view name)
{
    return projectRoot / kast::kStateMachinesDir / (componentFileStem(trimmed(name)) + ".lua");
}

std::string newStateMachineNameError(const Document& doc, std::string_view name)
{
    if (const char* blocked = stateMachineEditBlocker(doc))
        return blocked;
    const std::string machine(trimmed(name));
    if (machine.empty())
        return "Enter a name";
    if (!kast::isIdentifier(machine))
        return "Use letters, digits and _ only, not starting with a digit";
    // Every definition file of the project, valid or in error.
    if (std::ranges::any_of(kast::stateMachineDefinitions().status,
                            [&](const kast::ComponentDefinitionStatus& s) { return s.name == machine; }))
        return std::format("A state machine named \"{}\" already exists", machine);
    if (const std::string existing = existingFileName(stateMachineFile(doc.project->root, machine)); !existing.empty())
        return std::format("{}/{} already exists", kast::kStateMachinesDir, existing);
    return {};
}

bool newStateMachine(Document& doc, std::string_view name, std::string& error)
{
    if (error = newStateMachineNameError(doc, name); !error.empty())
        return false;
    const std::string machine(trimmed(name));
    const std::filesystem::path file = stateMachineFile(doc.project->root, machine);
    if (!writeTemplate(file, kStateMachineTemplate, machine, error))
        return false;
    kast::log::info(std::format("Created state machine '{}'", file.string()));
    refreshStateMachines(doc); // loads it now rather than at the next poll
    return true;
}

namespace {

// dir/<stem>[_N].lua: the first N (1 = no suffix) whose file name is free in dir (ignoring case) and whose stem is not in
// names.
std::filesystem::path uniqueLuaFile(const std::filesystem::path& dir, std::string_view stem,
                                    const std::vector<std::string>& names = {})
{
    for (int n = 1;; ++n) {
        const std::string name = n == 1 ? std::string(stem) : std::format("{}_{}", stem, n);
        if (!assetNameTaken(dir, name + ".lua") && std::ranges::find(names, name) == names.end())
            return dir / (name + ".lua");
    }
}

// The template's quoted occurrences of machine oldName in text, renamed to newName; nullopt if text has no
// name = "<oldName>".
std::optional<std::string> renameMachineText(std::string text, const std::string& oldName, const std::string& newName)
{
    if (text.find(std::format("name = \"{}\"", oldName)) == std::string::npos)
        return std::nullopt;
    for (const std::string_view pattern : {"name = \"{}\"", "machine = \"{}\"", "\"{}: "}) {
        const std::string from = std::vformat(pattern, std::make_format_args(oldName));
        const std::string to = std::vformat(pattern, std::make_format_args(newName));
        for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
            text.replace(at, from.size(), to);
    }
    return text;
}

} // namespace

bool writeNewSystem(const std::filesystem::path& dir, std::filesystem::path& file, std::string& error)
{
    file = uniqueLuaFile(dir, "new_system"); // ponytail: free when checked, not locked until written
    return writeTemplate(file, kSystemTemplate, "", error);
}

bool isInStateMachinesDir(const std::filesystem::path& projectRoot, const std::filesystem::path& dir)
{
    return isSameOrInside(dir, projectRoot / kast::kStateMachinesDir);
}

bool writeNewStateMachine(const std::filesystem::path& dir, const std::vector<std::string>& machineNames,
                          std::filesystem::path& file, std::string& error)
{
    file = uniqueLuaFile(dir, "new_state_machine", machineNames);
    return writeTemplate(file, kStateMachineTemplate, utf8(file.stem()), error);
}

bool renameNewStateMachine(const std::filesystem::path& file, const std::string& newName,
                           const std::vector<std::string>& machineNames, std::string& error)
{
    const std::filesystem::path target = fromUtf8(newName);
    const std::string oldStem = utf8(file.stem());
    const std::string newStem = utf8(target.stem());
    const auto refuse = [&](const std::string& reason) {
        error = std::format("Cannot rename '{}' to '{}': {}", utf8(file.filename()), newName, reason);
        return false;
    };
    if (target.extension() != ".lua")
        return refuse("state machine files must end in .lua");
    if (newStem == oldStem)
        return renameAsset(file, newName, error);
    if (!kast::isIdentifier(newStem))
        return refuse(std::format("\"{}\" is not a valid machine name (letters, digits and _ only, not starting with a "
                                  "digit)",
                                  newStem));
    if (std::ranges::any_of(machineNames, [&](const std::string& name) { return name != oldStem && name == newStem; }))
        return refuse(std::format("a state machine named \"{}\" already exists", newStem));
    std::ifstream in(file, std::ios::binary);
    const std::optional<std::string> text =
        renameMachineText(std::string(std::istreambuf_iterator<char>(in), {}), oldStem, newStem);
    in.close();
    if (!text)
        return refuse(std::format("it no longer holds name = \"{}\"", oldStem));
    if (!renameAsset(file, newName, error))
        return false;
    const std::filesystem::path renamed = file.parent_path() / target;
    if (writeTemplate(renamed, *text, newStem, error))
        return true;
    std::string ignored;
    renameAsset(renamed, utf8(file.filename()), ignored); // back to the old name; its text is unchanged
    return false;
}

bool isGlobalStateMachine(const Document& doc, const std::string& name)
{
    return doc.project && doc.project->stateMachines &&
           std::ranges::find(*doc.project->stateMachines, name) != doc.project->stateMachines->end();
}

bool setGlobalStateMachine(Document& doc, const std::string& name, bool global, std::string& error)
{
    if (const char* blocked = stateMachineEditBlocker(doc)) {
        error = blocked;
        return false;
    }
    if (isGlobalStateMachine(doc, name) == global)
        return true;
    kast::Project project = *doc.project;
    std::vector<std::string> machines = project.stateMachines.value_or(std::vector<std::string>{});
    if (global)
        machines.push_back(name);
    else
        std::erase(machines, name);
    project.stateMachines = std::move(machines);
    if (!kast::saveProject(project, error))
        return false;
    doc.project = std::move(project);
    kast::log::info(std::format("State machine \"{}\" is {} global ({})", name, global ? "now" : "no longer",
                                kast::kProjectFileName));
    return true;
}

bool setBusDefault(Document& doc, kast::AudioBus bus, kast::BusMix level, std::string& error)
{
    if (!doc.project) {
        error = "Open a project to set its bus levels";
        return false;
    }
    if (!(level.volume >= 0.0f && level.volume <= 1.0f)) {
        error = std::format("Bus volume must be between 0 and 1, got {}", level.volume);
        return false;
    }
    kast::Project project = *doc.project;
    project.audio[static_cast<int>(bus)] = level;
    if (!kast::saveProject(project, error))
        return false;
    doc.project = std::move(project);
    return true;
}

std::string projectFolderName(std::string_view name)
{
    std::string folder;
    bool afterSpace = false;
    for (const char c : trimmed(name)) {
        if (isAsciiSpace(c)) {
            afterSpace = true;
            continue;
        }
        if (std::exchange(afterSpace, false))
            folder += '-';
        folder += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return folder;
}

std::filesystem::path newProjectTarget(std::string_view name, const std::string& dir)
{
    const std::filesystem::path target = std::filesystem::path(dir) / projectFolderName(name);
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(target, ec);
    return (ec ? target : absolute).lexically_normal();
}

std::string newProjectError(std::string_view name, const std::string& dir)
{
    const std::string folder = projectFolderName(name);
    if (folder.empty())
        return "Enter a project name";
    if (std::ranges::any_of(folder, isInvalidFolderChar))
        return R"(The name must not contain < > : " / \ | ? * or control characters)";
    if (folder == "." || folder == "..")
        return std::format("'{}' is not a valid folder name", folder);
    if (folder.back() == '.') // Windows silently drops it, so the folder would not match the preview
        return "The name must not end with '.'";
    if (dir.empty())
        return "Choose a directory";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return std::format("The directory '{}' does not exist", dir);
    if (const std::filesystem::path target = newProjectTarget(name, dir); std::filesystem::exists(target, ec))
        return std::format("'{}' already exists", target.string());
    return "";
}

bool createProject(std::string_view name, const std::string& dir, std::filesystem::path& created, std::string& error)
{
    error = newProjectError(name, dir);
    if (!error.empty())
        return false;
    const std::filesystem::path target = newProjectTarget(name, dir);
    std::error_code ec;
    if (!std::filesystem::create_directory(target, ec)) {
        error = std::format("Cannot create the folder '{}': {}", target.string(), ec ? ec.message() : "it already exists");
        return false;
    }
    const char* startScene = "scenes/main.scene";
    Document defaults;
    resetToDefault(defaults);
    bool written = std::filesystem::create_directory(target / "scenes", ec);
    if (!written)
        error = std::format("Cannot create the folder '{}': {}", (target / "scenes").string(),
                            ec ? ec.message() : "it already exists");
    written = written && writeScene(defaults, (target / startScene).string(), error) &&
              kast::saveProject({.root = target, .name = std::string(trimmed(name)), .startScene = startScene}, error);
    if (!written) {
        std::filesystem::remove_all(target, ec); // made by this call, so nothing of the user's is lost
        return false;
    }
    created = target;
    kast::log::info(std::format("Created project '{}'", (target / kast::kProjectFileName).string()));
    return true;
}

std::string displayName(const Document& doc)
{
    if (doc.prefabEdit)
        return utf8(fromUtf8(doc.prefabEdit->prefab).filename());
    return doc.path.empty() ? "Untitled" : std::filesystem::path(doc.path).stem().string();
}

std::string windowTitle(const Document& doc, bool extraDirty)
{
    std::string play;
    if (doc.play != PlayState::Stopped)
        play = std::format(" ({}{}{})", doc.play == PlayState::Playing ? "Playing" : "Paused",
                           doc.playSceneName.empty() ? "" : ": ", doc.playSceneName);
    const std::string project = doc.project ? doc.project->name + " - " : "";
    return std::format("Kast Editor - {}{}{}{}", project, displayName(doc), doc.dirty || extraDirty ? "*" : "",
                       play);
}

void FileActions::newScene() { request(Pending::New); }
void FileActions::open() { request(Pending::Open); }
void FileActions::openScene(std::string path)
{
    if (dialogOpen_)
        return; // request would drop it; keep the previous openPath_ for a pending prompt
    openPath_ = std::move(path);
    request(Pending::OpenPath);
}
void FileActions::saveAs()
{
    if (doc_.prefabEdit)
        save(); // a prefab keeps its file
    else
        showSaveDialog(Pending::None);
}
void FileActions::openPrefab(std::string path)
{
    if (dialogOpen_)
        return;
    openPrefab_ = std::move(path);
    if (doc_.prefabEdit)
        request(Pending::OpenPrefab); // the open prefab's unsaved changes
    else
        perform(Pending::OpenPrefab); // the scene waits, unsaved changes and all
}
void FileActions::backFromPrefab() { request(Pending::Back); }
void FileActions::exit() { kast::requestQuit(); } // same path as the window X, so onQuitRequested runs
void FileActions::build() { request(Pending::Build); }
void FileActions::buildAndRun() { request(Pending::BuildAndRun); }

void FileActions::save()
{
    if (doc_.path.empty())
        showSaveDialog(Pending::None);
    else
        saveTo(doc_.path, Pending::None);
}

bool FileActions::onQuitRequested()
{
    stopPlay(doc_); // restore the edit-mode scene first, so unsaved-changes checks see it
    if (!quitConfirmed_ && doc_.prefabEdit && !doc_.dirty)
        closePrefab(doc_); // the waiting scene's unsaved changes still prompt
    if (quitConfirmed_ || (!unsaved() && !dialogOpen_))
        return true;
    request(Pending::Quit);
    return false;
}

std::string FileActions::extraUnsaved() const
{
    return extra.unsaved && !doc_.prefabEdit ? extra.unsaved() : std::string();
}

void FileActions::request(Pending action)
{
    if (dialogOpen_)
        return;
    if (!unsaved()) {
        perform(action);
        return;
    }
    pending_ = action;
    confirmRequested_ = true;
}

void FileActions::perform(Pending action)
{
    if (doc_.prefabEdit && action != Pending::None && action != Pending::OpenPrefab && action != Pending::Back) {
        closePrefab(doc_); // its changes were saved or discarded by now
        if (unsaved()) {
            request(action); // now the scene's
            return;
        }
    }
    switch (action) {
    case Pending::New:
        resetToDefault(doc_);
        kast::log::info("New scene");
        break;
    case Pending::Open:
        dialogOpen_ = true;
        dialogKind_ = DialogKind::Open;
        SDL_ShowOpenFileDialog(onDialogDone, this, window_, &kSceneFilter, 1, nullptr, false);
        break;
    case Pending::OpenPath:
        if (std::string error; !openFile(doc_, openPath_, error)) {
            error_ = error;
            kast::log::error(error_);
            errorRequested_ = true;
        }
        break;
    case Pending::Quit:
        quitConfirmed_ = true;
        kast::requestQuit();
        break;
    case Pending::Build:
        if (startSceneReady())
            browseBuildFolder(false);
        break;
    case Pending::BuildAndRun:
        if (!startSceneReady())
            break;
        if (const std::optional<std::filesystem::path> folder = doc_.project ? loadBuildFolder(doc_.project->root) : std::nullopt)
            buildTo(*folder, true);
        else
            browseBuildFolder(true);
        break;
    case Pending::OpenPrefab:
        if (std::string error; !::openPrefab(doc_, openPrefab_, error)) {
            error_ = error;
            kast::log::error(error_);
            errorRequested_ = true;
        }
        break;
    case Pending::Back:
        closePrefab(doc_);
        break;
    case Pending::None: break;
    }
}

void FileActions::showSaveDialog(Pending after)
{
    if (dialogOpen_)
        return;
    afterSave_ = after;
    dialogOpen_ = true;
    dialogKind_ = DialogKind::SaveAs;
    SDL_ShowSaveFileDialog(onDialogDone, this, window_, &kSceneFilter, 1, doc_.path.empty() ? nullptr : doc_.path.c_str());
}

void FileActions::onDialogDone(void* userdata, const char* const* files, int /*filter*/)
{
    auto* self = static_cast<FileActions*>(userdata);
    std::lock_guard lock(self->dialogMutex_);
    self->dialogDone_ = true;
    self->dialogPath_ = files && files[0] ? files[0] : "";
    self->dialogError_ = files ? "" : SDL_GetError();
}

void FileActions::handleDialogResult()
{
    std::string path;
    std::string dialogError;
    {
        std::lock_guard lock(dialogMutex_);
        if (!dialogDone_)
            return;
        dialogDone_ = false;
        path = std::move(dialogPath_);
        dialogError = std::move(dialogError_);
    }
    dialogOpen_ = false;
    const Pending after = std::exchange(afterSave_, Pending::None);
    if (!path.empty())
        stopPlay(doc_); // the dialog opened before Play started: open/save the edit-mode scene
    if (!dialogError.empty()) {
        error_ = std::format("File dialog failed: {}", dialogError);
    } else if (path.empty()) {
        return; // cancelled
    } else if (dialogKind_ == DialogKind::Open) {
        std::string error;
        if (openFile(doc_, path, error))
            return;
        error_ = error;
    } else if (dialogKind_ == DialogKind::CaFile) {
        if (doc_.project) // a file outside the project stays absolute, so the modal's check explains it
            online_.caFile = kast::toProjectPath(doc_.project->root, fromUtf8(path)).value_or(path);
        return;
    } else if (dialogKind_ == DialogKind::BuildFolder) {
        buildTo(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), runAfterBuild_); // SDL gives UTF-8
        return;
    } else {
        if (std::filesystem::path(path).extension().empty())
            path += ".scene";
        saveTo(path, after);
        return;
    }
    kast::log::error(error_);
    errorRequested_ = true;
}

void FileActions::saveTo(const std::string& path, Pending after)
{
    std::string error;
    if (saveFile(doc_, path, error)) {
        // Then the extra (a dirty zone). Blocked (it logs why): the action waits, so its changes aren't lost.
        if (const std::string what = extraUnsaved(); !what.empty() && extra.save && !extra.save()) {
            if (after == Pending::None)
                return;
            error = std::format("{} was not saved (see the Console), so nothing else was done", what);
        } else {
            perform(after);
            return;
        }
    }
    error_ = error;
    kast::log::error(error_);
    errorRequested_ = true;
}

void FileActions::update()
{
    if (idle()) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteGlobal))
            newScene();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
            open();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
            save();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
            saveAs();
    }
    handleDialogResult();
    drawModals();
    updateTitle();
}

void FileActions::drawModals()
{
    if (std::exchange(confirmRequested_, false))
        ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string what = extraUnsaved();
        ImGui::Text("Save changes to %s%s%s before continuing?", displayName(doc_).c_str(), what.empty() ? "" : " and ",
                    what.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save")) {
            ImGui::CloseCurrentPopup();
            stopPlay(doc_); // save the edit-mode scene, never a play-mode one
            const Pending action = std::exchange(pending_, Pending::None);
            if (doc_.path.empty())
                showSaveDialog(action);
            else
                saveTo(doc_.path, action);
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            ImGui::CloseCurrentPopup();
            stopPlay(doc_);
            if (extra.discard && !extraUnsaved().empty())
                extra.discard();
            perform(std::exchange(pending_, Pending::None));
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            pending_ = Pending::None;
        }
        ImGui::EndPopup();
    }

    drawBuildSettingsModal();
    drawOnlineSettingsModal();

    if (std::exchange(errorRequested_, false))
        ImGui::OpenPopup("Error");
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(error_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void FileActions::openBuildSettings(std::string reason)
{
    if (!doc_.project)
        return;
    buildScenes_ = projectScenes(doc_.project->root);
    buildStartScene_ = doc_.project->startScene.value_or("");
    buildSettingsError_ = std::move(reason);
    buildSettingsRequested_ = true;
    buildSettingsOpen_ = true;
}

void FileActions::drawBuildSettingsModal()
{
    if (std::exchange(buildSettingsRequested_, false))
        ImGui::OpenPopup("Build Settings");
    // Tracks the popup itself, so idle() also sees it closed when another popup (an Error) replaced it.
    buildSettingsOpen_ = ImGui::BeginPopupModal("Build Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!buildSettingsOpen_)
        return;
    const bool listed = std::ranges::find(buildScenes_, buildStartScene_) != buildScenes_.end();
    const std::string preview = listed || buildStartScene_.empty() ? buildStartScene_ : buildStartScene_ + " (missing)";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
    ImGui::BeginDisabled(buildScenes_.empty());
    if (ImGui::BeginCombo("Start Scene", preview.c_str())) {
        for (const std::string& scene : buildScenes_) {
            if (ImGui::Selectable(scene.c_str(), scene == buildStartScene_)) {
                buildStartScene_ = scene;
                buildSettingsError_.clear();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (buildScenes_.empty())
        ImGui::TextUnformatted("No scenes in project. Save a scene first.");
    if (!buildSettingsError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", buildSettingsError_.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!listed);
    const bool save = ImGui::Button("Save");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (save) {
        if (setStartScene(*doc_.project, buildStartScene_, buildSettingsError_))
            ImGui::CloseCurrentPopup();
        else
            kast::log::error(buildSettingsError_);
    } else if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void FileActions::openOnlineSettings()
{
    if (!doc_.project)
        return;
    online_ = onlineChecked_ = doc_.project->online;
    onlineError_.clear();
    checkOnlineConfig(*doc_.project, online_, onlineError_); // e.g. a CA file deleted since it was saved
    onlineSettingsRequested_ = true;
    onlineSettingsOpen_ = true;
}

void FileActions::drawOnlineSettingsModal()
{
    if (std::exchange(onlineSettingsRequested_, false))
        ImGui::OpenPopup("Online Services");
    // Tracks the popup itself, so idle() also sees it closed when another popup (an Error) replaced it.
    onlineSettingsOpen_ = ImGui::BeginPopupModal("Online Services", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (!onlineSettingsOpen_)
        return;
    const float fieldWidth = ImGui::GetFontSize() * 24.0f;
    ImGui::Checkbox("Enabled", &online_.enabled);
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputTextWithHint("Auth URL", "https://auth.example.com", &online_.authUrl);
    std::string caFile = online_.caFile.value_or("");
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::InputTextWithHint("CA File", "optional: project-relative .pem", &caFile))
        online_.caFile = caFile.empty() ? std::nullopt : std::optional(caFile);
    ImGui::SameLine();
    ImGui::BeginDisabled(dialogOpen_);
    if (ImGui::Button("Browse...")) {
        dialogOpen_ = true;
        dialogKind_ = DialogKind::CaFile;
        const std::string root = utf8(doc_.project->root);
        SDL_ShowOpenFileDialog(onDialogDone, this, window_, &kPemFilter, 1, root.c_str(), false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        online_.caFile.reset();
    ImGui::TextDisabled("The CA file is trusted besides the Windows root certificates (auth and world TLS).");
    if (online_ != onlineChecked_) {
        onlineChecked_ = online_;
        onlineError_.clear();
        checkOnlineConfig(*doc_.project, online_, onlineError_);
    }
    if (!onlineError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", onlineError_.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!onlineError_.empty() || dialogOpen_);
    const bool save = ImGui::Button("Save");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (save) {
        if (setOnlineConfig(*doc_.project, online_, onlineError_))
            ImGui::CloseCurrentPopup();
        else
            kast::log::error(onlineError_);
    } else if (ImGui::Button("Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !dialogOpen_)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool FileActions::startSceneReady()
{
    std::string error;
    if (!doc_.project || checkStartScene(*doc_.project, error))
        return true; // no project: buildTo reports it
    kast::log::warn(error);
    openBuildSettings(error);
    return false;
}

void FileActions::browseBuildFolder(bool run)
{
    if (dialogOpen_ || !doc_.project)
        return;
    runAfterBuild_ = run;
    dialogOpen_ = true;
    dialogKind_ = DialogKind::BuildFolder;
    const std::optional<std::filesystem::path> last = loadBuildFolder(doc_.project->root);
    const std::string start = last ? reinterpret_cast<const char*>(last->u8string().c_str()) : "";
    SDL_ShowOpenFolderDialog(onDialogDone, this, window_, start.empty() ? nullptr : start.c_str(), false);
}

void FileActions::buildTo(const std::filesystem::path& folder, bool run)
{
    std::string error;
    std::filesystem::path exe;
    const char* base = SDL_GetBasePath(); // kast_player.exe is built next to the editor
    if (!doc_.project) {
        error = "Build failed: no project is open";
    } else if (!base) {
        error = std::format("Build failed: cannot find the editor's folder: {}", SDL_GetError());
    } else if (buildGame(*doc_.project, folder, std::filesystem::path(reinterpret_cast<const char8_t*>(base)), exe, error)) {
        if (std::string saveError; !saveBuildFolder(doc_.project->root, folder, saveError))
            kast::log::warn(saveError);
        if (!run || launchProcess(exe, {}, error))
            return;
    }
    error_ = error;
    kast::log::error(error_);
    errorRequested_ = true;
}

void FileActions::updateTitle()
{
    std::string title = windowTitle(doc_, !extraUnsaved().empty());
    if (title == title_ || !window_)
        return;
    title_ = std::move(title);
    SDL_SetWindowTitle(window_, title_.c_str());
}
