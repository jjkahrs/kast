// Assert-style checks for the non-trivial engine logic: rotation conventions, matrix decomposition, picking rays,
// hierarchy edits, component registry, scene-file validation, cameras, input, systems, Lua systems, projects, scene
// switching, editor Play mode, Lua components, game.pak, Build, state machines, Assets panel file operations and the
// game GUI (builder document model, GUI files, UIDocument, button click events), the external editor setting and models
// (Assimp loading, animation sampling, crossfades, skinning limits) and physics (bodies, events, kast.physics,
// characters). --bench-physics instead times 500 dynamic boxes and exits.
// Exit 0 if every check passes. Run by CTest as engine_self_check.
#include "assets.h"   // Assets panel file logic; links editor/assets.cpp
#include "build.h"    // editor Build pipeline; links editor/build.cpp
#include "document.h" // editor Play mode; links editor/document.cpp, no window needed
#include "external_editor.h" // external editor command; links editor/external_editor.cpp
#include "gui_doc.h"  // GUI Builder document model; links editor/gui_doc.cpp
#include "gui_undo.h" // GUI Builder undo steps; links editor/gui_undo.cpp
#include "model_importer.h" // Assimp; links kast_model_import
#include "recent_projects.h" // Project Manager list; links editor/recent_projects.cpp
#include "shader_compiler.h" // shaderc for .kshader files; links editor/shader_compiler.cpp
#include "shader_graph_doc.h" // the Shader Graph panels' edit model; links editor/shader_graph_doc.cpp
#include "texture_cooker.h" // texturec for PNG / JPG compression; links editor/texture_cooker.cpp
#include "stats_toggle.h" // the Game panel's F3 rule; header only

#include <components.h>
#include <crash_dialogs.h>
#include <event_bus.h>
#include <gui_events.h>
#include <gui_render.h>
#include <files.h>
#include <gui.h>
#include <input.h>
#include <log.h>
#include <lua_components.h>
#include <lua_systems.h>
#include <material.h>
#include <network.h>
#include <math3d.h>
#include <model.h>
#include <physics.h>
#include <play.h>
#include <animation.h>
#include <app.h>
#include <audio.h>
#include <pak.h>
#include <post_process.h>
#include <prefab.h>
#include <project.h>
#include <particles.h>
#include <renderer.h>
#include <scene.h>
#include <scene_gui.h>
#include <scene_io.h>
#include <shader.h>
#include <shader_graph.h>
#include <shadow.h>
#include <sky.h>
#include <frustum.h>
#include <state_machines.h>
#include <stats_overlay.h>
#include <systems.h>
#include <terrain.h>
#include <texture_cook.h>

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h> // _O_RDONLY
#include <filesystem>
#include <format>
#include <fstream>
#include <io.h> // _wsopen_s, _close
#include <iterator>
#include <map>
#include <optional>
#include <process.h> // _getpid
#include <random>
#include <regex>
#include <share.h>   // _SH_DENYRW
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>

int netSelfCheck(); // tests/net_self_check.cpp: returns its failure count
int liveAuthCheck(const std::string& authUrl, const std::string& mailpitUrl); // the same file: 0, 1 or 77 (skipped)
int liveWorldCheck(const std::string& authUrl, const std::string& mailpitUrl, const std::string& caFile,
                   const std::string& realmName); // the same file: 0, 1 or 77 (skipped)
int liveReplicationCheck(const std::string& authUrl, const std::string& mailpitUrl, const std::string& caFile,
                         const std::string& realmName); // the same file: 0, 1 or 77 (skipped)
int liveOnlineCheck(const std::string& authUrl, const std::string& mailpitUrl,
                    const std::string& composeFolder); // the same file: 0, 1 or 77 (skipped)

namespace {

int g_failures = 0;

void check(bool ok, const char* what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED line {}: {}", where.line(), what));
}

// Never throws: a file briefly locked by Defender or the indexer must not abort the run; main() checks the root is gone.
void removeTree(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// One temp folder per run holds every test folder, so parallel runs never collide and main() cleans up once.
// ponytail: a crashed run leaves its kast_self_check_<pid> behind; sweep stale roots if that ever piles up.
const std::filesystem::path& testRoot()
{
    static const std::filesystem::path root = [] {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("kast_self_check_{}", _getpid());
        removeTree(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }();
    return root;
}

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
bool near(const bx::Vec3& a, const bx::Vec3& b, float eps = 1e-4f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bx::Vec3 rotate(const bx::Quaternion& q, const bx::Vec3& v)
{
    float m[16];
    kast::mtxFromQuat(m, q);
    return kast::transformDir(v, m);
}
bool sameRotation(const bx::Quaternion& a, const bx::Quaternion& b)
{
    return std::abs(bx::dot(a, b)) > 1.0f - 1e-5f; // q and -q are the same rotation
}
bx::Vec3 worldPosition(const kast::Scene& scene, entt::entity e)
{
    float m[16];
    scene.worldMatrix(e, m);
    return {m[12], m[13], m[14]};
}

void checkRotations()
{
    // Right-handed: +90 deg about Y takes +X to -Z; +90 deg about X takes +Y to +Z.
    check(near(rotate(kast::quatFromEulerDeg({0, 90, 0}), {1, 0, 0}), {0, 0, -1}), "yaw +90 maps +X to -Z");
    check(near(rotate(kast::quatFromEulerDeg({90, 0, 0}), {0, 1, 0}), {0, 0, 1}), "pitch +90 maps +Y to +Z");
    check(near(rotate(kast::quatFromEulerDeg({0, 0, 90}), {1, 0, 0}), {0, 1, 0}), "roll +90 maps +X to +Y");

    const bx::Vec3 angles[] = {{0, 0, 0}, {30, 45, 60}, {-50, 120, -10}, {89, 10, 20}, {90, 30, 0}, {-90, 0, 0}, {10, 179, -170}};
    for (const bx::Vec3& euler : angles) {
        const bx::Quaternion q = kast::quatFromEulerDeg(euler);
        check(sameRotation(kast::quatFromEulerDeg(kast::eulerDegFromQuat(q)), q), "euler -> quat -> euler -> quat");

        const bx::Vec3 t{1.5f, -2.0f, 3.0f};
        const bx::Vec3 s{2.0f, 0.5f, 3.0f};
        float m[16];
        kast::mtxFromTrs(m, t, q, s);
        bx::Vec3 t2(bx::InitNone), s2(bx::InitNone);
        bx::Quaternion q2(bx::InitNone);
        kast::decomposeTrs(m, t2, q2, s2);
        check(near(t, t2) && near(s, s2) && sameRotation(q, q2), "decomposeTrs inverts mtxFromTrs");
    }
}

void checkPicking()
{
    const kast::Aabb box{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    check(near(kast::intersect({{0, 0, 5}, {0, 0, -1}}, box), 4.5f), "ray hits box front face");
    check(kast::intersect({{2, 0, 5}, {0, 0, -1}}, box) < 0.0f, "ray beside box misses");
    check(kast::intersect({{0, 0, 5}, {0, 0, 1}}, box) < 0.0f, "box behind ray misses");
    check(kast::intersect({{0, 0, 0}, {1, 0, 0}}, box) == 0.0f, "ray from inside hits at 0");
    const kast::Aabb plane{{-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, 0.5f}};
    check(near(kast::intersect({{0.2f, 3, 0}, {0, -1, 0}}, plane), 3.0f), "ray hits flat plane bounds");

    float view[16], proj[16], viewProj[16];
    bx::mtxLookAt(view, {0, 0, 5}, {0, 0, 0}, {0, 1, 0}, bx::Handedness::Right);
    bx::mtxProj(proj, 60.0f, 2.0f, 0.1f, 100.0f, false, bx::Handedness::Right);
    bx::mtxMul(viewProj, view, proj);
    const kast::Ray center = kast::screenRay(viewProj, 400, 200, 800, 400, false);
    check(near(center.direction, {0, 0, -1}) && near(center.origin.x, 0) && near(center.origin.y, 0), "center pixel looks down -Z");
    const kast::Ray right = kast::screenRay(viewProj, 790, 200, 800, 400, false);
    check(right.direction.x > 0.5f, "right edge pixel points right");
    const kast::Ray top = kast::screenRay(viewProj, 400, 5, 800, 400, false);
    check(top.direction.y > 0.3f, "top edge pixel points up");
}

void checkHierarchy()
{
    kast::Scene scene;
    const entt::entity parent = scene.create("Parent");
    auto& pt = scene.registry().get<kast::Transform>(parent);
    pt.position = {1, 0, 0};
    pt.rotation = kast::quatFromEulerDeg({0, 90, 0});
    pt.scale = {2, 2, 2};
    const entt::entity child = scene.create("Child", parent);
    scene.registry().get<kast::Transform>(child).position = {1, 0, 0};
    const entt::entity grandchild = scene.create("Grandchild", child);
    const entt::entity other = scene.create("Other");

    check(near(worldPosition(scene, child), {1, 0, -2}), "child world = local * parent world");

    check(!scene.reparent(parent, child), "reparent under own child rejected");
    check(!scene.reparent(parent, grandchild), "reparent under grandchild rejected");
    check(!scene.reparent(parent, parent), "reparent under self rejected");
    check(scene.parent(parent) == entt::null && scene.children(entt::null).size() == 2, "rejected reparent changes nothing");

    const bx::Vec3 before = worldPosition(scene, child);
    check(scene.reparent(child, entt::null), "unparent to root");
    check(near(worldPosition(scene, child), before), "unparent keeps world position");
    check(near(scene.registry().get<kast::Transform>(child).scale, {2, 2, 2}), "unparent bakes parent scale");
    check(scene.reparent(child, other) && near(worldPosition(scene, child), before), "reparent keeps world position");
    check(scene.reparent(child, parent) && near(scene.registry().get<kast::Transform>(child).position, {1, 0, 0}),
          "reparent back restores local position");

    const entt::entity copy = scene.duplicate(parent);
    const auto& roots = scene.children(entt::null);
    check(roots.size() == 3 && roots[0] == parent && roots[1] == copy && roots[2] == other, "duplicate goes right after original");
    check(scene.children(copy).size() == 1 && scene.children(scene.children(copy)[0]).size() == 1, "duplicate copies subtree");
    check(scene.registry().get<kast::PersistentId>(copy).value != scene.registry().get<kast::PersistentId>(parent).value,
          "duplicate gets a new id");
    check(near(worldPosition(scene, scene.children(copy)[0]), worldPosition(scene, child)), "duplicate keeps transforms");

    scene.destroy(parent);
    check(!scene.valid(parent) && !scene.valid(child) && !scene.valid(grandchild), "destroy removes subtree");
    check(scene.valid(copy) && scene.children(entt::null).size() == 2, "destroy leaves the rest");
    scene.destroy(parent);
    check(!scene.reparent(parent, entt::null) && !scene.reparent(copy, parent) && scene.parent(copy) == entt::null &&
              scene.children(entt::null).size() == 2,
          "destroy / reparent of a destroyed entity change nothing");
}

// Reparenting keeps the whole world matrix of a mirrored (negative-scale) entity, not just its position.
void checkMirroredReparent()
{
    kast::Scene scene;
    const entt::entity parent = scene.create("Parent");
    auto& pt = scene.registry().get<kast::Transform>(parent);
    pt.position = {1, 2, 3};
    pt.rotation = kast::quatFromEulerDeg({10, 40, 0});
    pt.scale = {2, 2, 2};
    const entt::entity mirrored = scene.create("Mirrored");
    auto& t = scene.registry().get<kast::Transform>(mirrored);
    t.position = {4, 5, 6};
    t.rotation = kast::quatFromEulerDeg({20, 30, 50});
    t.scale = {2, -2, 2}; // uniform magnitude: the child below gets no shear
    const entt::entity child = scene.create("Child", mirrored);
    scene.registry().get<kast::Transform>(child).rotation = kast::quatFromEulerDeg({0, 70, 0});
    const auto keepsWorld = [&](entt::entity e, entt::entity newParent) {
        float before[16], after[16];
        scene.worldMatrix(e, before);
        const bool moved = scene.reparent(e, newParent);
        scene.worldMatrix(e, after);
        return moved && std::ranges::equal(before, after, [](float a, float b) { return std::abs(a - b) < 1e-4f; });
    };
    check(keepsWorld(mirrored, parent), "reparent keeps a mirrored entity's world matrix");
    check(keepsWorld(mirrored, entt::null), "unparent keeps a mirrored entity's world matrix");
    check(keepsWorld(child, entt::null), "unparent from a mirrored parent keeps the world matrix");
}

// A scene nested deeper than kMaxHierarchyDepth fails to load instead of overflowing the stack in the recursive walks.
void checkDeepHierarchy()
{
    using Json = nlohmann::ordered_json;
    const auto chain = [](int depth) {
        Json entities = Json::array();
        for (kast::EntityId id = 1; id <= kast::EntityId(depth); ++id)
            entities.push_back({{"id", id}, {"parent", id == 1 ? Json(nullptr) : Json(id - 1)}, {"name", "E"},
                                {"components", {{"Transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}},
                                                               {"scale", {1, 1, 1}}}}}}});
        return Json{{"version", kast::kSceneFileVersion}, {"entities", std::move(entities)}};
    };
    kast::Scene scene;
    std::string error;
    const bool loaded = kast::sceneFromJson(chain(kast::kMaxHierarchyDepth), scene, error);
    check(loaded, "a hierarchy at the depth cap loads");
    for (const int depth : {kast::kMaxHierarchyDepth + 1, 100000}) {
        kast::Scene deep;
        check(!kast::sceneFromJson(chain(depth), deep, error) &&
                  error.find(std::format("deeper than {} levels", kast::kMaxHierarchyDepth)) != std::string::npos,
              "a hierarchy deeper than the cap is rejected with an error");
    }
    if (!loaded)
        return;

    // At runtime, create and reparent refuse to go past the cap too.
    entt::entity leaf = scene.children(entt::null)[0];
    entt::entity aboveLeaf = entt::null;
    while (!scene.children(leaf).empty()) {
        aboveLeaf = leaf;
        leaf = scene.children(leaf)[0];
    }
    check(scene.depth(leaf) == kast::kMaxHierarchyDepth && scene.create("Deeper", leaf) == entt::null &&
              scene.children(leaf).empty(),
          "create under an entity at the depth cap is refused");
    const entt::entity pair = scene.create("Pair");
    scene.create("PairChild", pair);
    check(!scene.reparent(pair, aboveLeaf) && scene.parent(pair) == entt::null,
          "reparent that would push a subtree past the depth cap is refused");
    check(scene.reparent(pair, scene.parent(aboveLeaf)) &&
              scene.depth(scene.children(pair)[0]) == kast::kMaxHierarchyDepth,
          "reparent up to the depth cap is allowed");
}

// A registered type that only the self-check knows: proves new components need no serialization/copy code and
// covers the int, bool and string field types.
struct TestComponent {
    int count = 3;
    bool enabled = true;
    std::string label = "label";
};

std::vector<std::string> typeNames()
{
    std::vector<std::string> names;
    for (const entt::meta_type& type : kast::componentTypes())
        names.emplace_back(type.name());
    return names;
}

void checkComponentRegistry()
{
    check(typeNames() == std::vector<std::string>{"Transform", "MeshRenderer", "ModelRenderer", "Animator",
                                                 "DirectionalLight", "PointLight", "Camera", "Rotator", "Collider",
                                                 "Rigidbody", "CharacterController", "StateMachine", "UIDocument",
                                                 "AudioSource", "NetworkLocalPlayer", "NetworkReplication",
                                                 "NetworkEntity", "Terrain", "TerrainTile", "ParticleEmitter"},
          "built-in component types in registration order");
    check(kast::componentInfo(kast::findComponentType("TerrainTile")).runtime &&
              !kast::componentInfo(kast::findComponentType("Terrain")).runtime,
          "TerrainTile is runtime only (not in Add Component); Terrain is authored");
    const entt::meta_type transform = kast::findComponentType("Transform");
    const entt::meta_type rotator = kast::findComponentType("Rotator");
    check(transform && rotator && !kast::findComponentType("Name") && !kast::findComponentType("Bogus"),
          "find by type name; structural components not registered");
    check(!kast::componentInfo(transform).removable && kast::componentInfo(rotator).removable,
          "Transform not removable, Rotator removable");

    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity e = scene.create("E");
    check(kast::hasComponent(registry, e, transform) && !kast::hasComponent(registry, e, rotator), "has");
    kast::addComponent(registry, e, rotator);
    check(kast::hasComponent(registry, e, rotator) && registry.get<kast::Rotator>(e).degreesPerSecond == 45.0f,
          "add default-constructs");
    registry.get<kast::Rotator>(e).degreesPerSecond = 10.0f;
    check(kast::getComponent(registry, e, rotator).cast<kast::Rotator&>().degreesPerSecond == 10.0f,
          "get returns a reference");
    kast::removeComponent(registry, e, rotator);
    check(!kast::hasComponent(registry, e, rotator) && !registry.all_of<kast::Rotator>(e), "remove");

    kast::registerComponent<TestComponent>("TestComponent", {"Test Component"})
        .data<&TestComponent::count>("count").custom<kast::FieldInfo>("Count")
        .data<&TestComponent::enabled>("enabled").custom<kast::FieldInfo>("Enabled")
        .data<&TestComponent::label>("label").custom<kast::FieldInfo>("Label");
    check(typeNames().back() == "TestComponent", "newly registered type is listed last");
}

// Component -> JSON (checked against expectedJson) -> component -> JSON gives the same JSON.
template <typename T>
T roundTrip(const T& value, const char* expectedJson)
{
    const entt::meta_any any{value};
    const nlohmann::ordered_json json = kast::componentToJson(any);
    check(json.dump() == expectedJson, expectedJson);
    std::string error;
    entt::meta_any loaded = kast::componentFromJson(any.type(), json, error);
    check(loaded && error.empty(), "component loads from its own JSON");
    check(loaded && kast::componentToJson(loaded) == json, "component JSON round trip");
    return loaded ? loaded.cast<T>() : T{};
}

void checkComponentJson()
{
    const kast::Transform t = roundTrip(kast::Transform{{1.5f, -2, 0.1f}, {0, 0.6f, 0, 0.8f}, {2, 2, 2}},
                                        R"({"position":[1.5,-2.0,0.1],"rotation":[0.0,0.6,0.0,0.8],"scale":[2.0,2.0,2.0]})");
    check(sameRotation(t.rotation, {0, 0.6f, 0, 0.8f}) && near(t.position, {1.5f, -2, 0.1f}),
          "Transform values (Quaternion field)");
    const kast::MeshRenderer m = roundTrip(kast::MeshRenderer{kast::Primitive::Sphere, "materials/red.mat"},
                                           R"({"primitive":"sphere","material":"materials/red.mat","castShadows":true,"receiveShadows":true})");
    check(m.primitive == kast::Primitive::Sphere && m.material == "materials/red.mat", "MeshRenderer values (enum field)");
    const kast::DirectionalLight l = roundTrip(kast::DirectionalLight{{1, 0.9f, 0.8f}, 2.5f},
                                               R"({"color":[1.0,0.9,0.8],"intensity":2.5,"castShadows":true,"bias":0.05,"normalBias":1.5,"maxDistance":100.0,"cascadeCount":4})");
    check(l.intensity == 2.5f, "DirectionalLight values");
    const kast::Rotator r = roundTrip(kast::Rotator{{1, 0, 0}, -30}, R"({"axis":[1.0,0.0,0.0],"degreesPerSecond":-30.0})");
    check(r.degreesPerSecond == -30.0f && near(r.axis, {1, 0, 0}), "Rotator values");
    const kast::Camera cam = roundTrip(kast::Camera{45, 0.5f, 200},
                                       R"({"verticalFovDegrees":45.0,"nearPlane":0.5,"farPlane":200.0})");
    check(cam.verticalFovDegrees == 45.0f && cam.nearPlane == 0.5f && cam.farPlane == 200.0f, "Camera values");
    const TestComponent c = roundTrip(TestComponent{-7, false, "hello"}, R"({"count":-7,"enabled":false,"label":"hello"})");
    check(c.count == -7 && !c.enabled && c.label == "hello", "int, bool and string field values");

    const entt::meta_type rotator = kast::findComponentType("Rotator");
    const std::pair<const char*, const char*> bad[] = {
        {"not an object", "[]"},
        {"missing field", R"({"axis":[0,1,0]})"},
        {"unknown field", R"({"axis":[0,1,0],"degreesPerSecond":1,"speed":2})"},
        {"mistyped float", R"({"axis":[0,1,0],"degreesPerSecond":"fast"})"},
        {"short vector", R"({"axis":[0,1],"degreesPerSecond":1})"},
    };
    for (const auto& [what, text] : bad) {
        std::string error;
        check(!kast::componentFromJson(rotator, nlohmann::ordered_json::parse(text), error) && !error.empty(), what);
    }
}

void checkDuplicateComponents()
{
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity e = scene.create("E");
    registry.emplace<kast::MeshRenderer>(e, kast::Primitive::Plane, "a.mat");
    registry.emplace<kast::Rotator>(e, bx::Vec3{1, 0, 0}, 12.0f);
    registry.emplace<TestComponent>(e, 5, false, "copy me");
    registry.emplace<kast::Camera>(e, 40.0f, 0.3f, 50.0f);
    registry.get<kast::Transform>(e).position = {4, 5, 6};
    const entt::entity copy = scene.duplicate(e);
    const auto* mesh = registry.try_get<kast::MeshRenderer>(copy);
    const auto* rotator = registry.try_get<kast::Rotator>(copy);
    const auto* test = registry.try_get<TestComponent>(copy);
    check(mesh && mesh->primitive == kast::Primitive::Plane && mesh->material == "a.mat", "duplicate copies MeshRenderer");
    check(rotator && rotator->degreesPerSecond == 12.0f && near(rotator->axis, {1, 0, 0}), "duplicate copies Rotator");
    check(test && test->label == "copy me", "duplicate copies a newly registered component");
    const auto* camera = registry.try_get<kast::Camera>(copy);
    check(camera && camera->verticalFovDegrees == 40.0f && camera->nearPlane == 0.3f && camera->farPlane == 50.0f,
          "duplicate copies Camera");
    check(near(registry.get<kast::Transform>(copy).position, {4, 5, 6}), "duplicate copies Transform");
    check(!registry.all_of<kast::DirectionalLight>(copy), "duplicate adds nothing extra");
}

void checkLoadValidation()
{
    const std::filesystem::path dir = testRoot() / "kast_self_check";
    std::filesystem::create_directories(dir);
    const std::string transform = R"("Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]})";
    const auto entity = [&](int id, const char* parent, const std::string& more = "") {
        return std::format(R"({{"id": {}, "parent": {}, "name": "E", "components": {{{}{}}}}})", id, parent, transform, more);
    };
    const auto file = [](const std::string& entities) { return R"({"version": 5, "entities": [)" + entities + "]}"; };
    const std::tuple<const char*, std::string, const char*> bad[] = {
        // what, file contents, text the error must mention
        {"invalid JSON", "{ \"version\": 5, ", ""},
        {"wrong version", R"({"version": 99, "entities": []})", "version"},
        {"version 1 rejected",
         R"({"version": 1, "entities": [{"id": 1, "parent": null, "name": "E", "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}]})",
         "version"},
        {"missing Transform", file(R"({"id": 1, "parent": null, "name": "E", "components": {}})"), "Transform"},
        {"missing components", file(R"({"id": 1, "parent": null, "name": "E"})"), "components"},
        {"bad enum", file(entity(1, "null", R"(, "MeshRenderer": {"primitive": "torus", "material": ""})")), "torus"},
        {"mistyped field", file(entity(1, "null", R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": "fast"})")),
         "degreesPerSecond"},
        {"unknown field", file(entity(1, "null", R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": 1, "rpm": 2})")), "rpm"},
        {"missing field", file(entity(1, "null", R"(, "DirectionalLight": {"color": [1,1,1]})")), "intensity"},
        {"zero quaternion",
         file(R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,0], "scale": [1,1,1]}}})"),
         "rotation"},
        {"position beyond float range",
         file(R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [1e39,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}})"),
         "position"},
        {"float field beyond float range",
         file(entity(1, "null", R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": -1e39})")), "degreesPerSecond"},
        {"camera fov 0", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 0, "nearPlane": 0.1, "farPlane": 10})")),
         "verticalFovDegrees"},
        {"camera fov 180", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 180, "nearPlane": 0.1, "farPlane": 10})")),
         "verticalFovDegrees"},
        {"camera near 0", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 60, "nearPlane": 0, "farPlane": 10})")),
         "nearPlane"},
        {"camera near >= far", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 60, "nearPlane": 10, "farPlane": 10})")),
         "farPlane"},
        {"point light range 0", file(entity(1, "null", R"(, "PointLight": {"color": [1,1,1], "intensity": 1, "range": 0})")),
         "range"},
        {"point light negative intensity",
         file(entity(1, "null", R"(, "PointLight": {"color": [1,1,1], "intensity": -0.5, "range": 5})")), "intensity"},
        {"point light color > 1", file(entity(1, "null", R"(, "PointLight": {"color": [1,1.5,1], "intensity": 1, "range": 5})")),
         "color"},
        {"lighting not an object", R"({"version": 5, "entities": [], "lighting": 3})", "lighting"},
        {"lighting color out of range",
         R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,-0.1,1], "ambientIntensity": 0.2}})", "ambientColor"},
        {"lighting color wrong size", R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,1], "ambientIntensity": 0.2}})",
         "ambientColor"},
        {"lighting negative intensity",
         R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,1,1], "ambientIntensity": -1}})", "ambientIntensity"},
        {"lighting mistyped intensity",
         R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,1,1], "ambientIntensity": "dim"}})", "ambientIntensity"},
        {"lighting missing intensity", R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,1,1]}})",
         "ambientIntensity"},
        {"lighting unknown field",
         R"({"version": 5, "entities": [], "lighting": {"ambientColor": [1,1,1], "ambientIntensity": 1, "fog": 1}})", "fog"},
        {"sky not an object", R"({"version": 5, "entities": [], "sky": 3})", "\"sky\" must be an object"},
        {"sky unknown mode", R"({"version": 5, "entities": [], "sky": {"mode": "night"}})", "\"mode\""},
        {"sky unknown field", R"({"version": 5, "entities": [], "sky": {"mode": "off", "sun": true}})", "\"sun\""},
        {"sky color mistyped", R"({"version": 5, "entities": [], "sky": {"top": "blue"}})", "\"top\""},
        {"sky color channel mistyped", R"({"version": 5, "entities": [], "sky": {"horizon": [1, "1", 1]}})", "\"horizon\""},
        {"sky color out of range", R"({"version": 5, "entities": [], "sky": {"bottom": [0, 1.5, 0]}})", "\"bottom\""},
        {"sky color wrong size", R"({"version": 5, "entities": [], "sky": {"top": [0, 0]}})", "\"top\""},
        {"sky faces not an object", R"({"version": 5, "entities": [], "sky": {"faces": ["a.png"]}})", "\"faces\""},
        {"sky unknown face", R"({"version": 5, "entities": [], "sky": {"faces": {"up": "a.png"}}})", "\"up\""},
        {"sky face not a string", R"({"version": 5, "entities": [], "sky": {"faces": {"px": 1}}})", "\"px\""},
        {"dangling parent", file(entity(1, "7")), "parent"},
        {"cycle", file(entity(1, "2") + ", " + entity(2, "1")), "cycle"},
        {"self parent", file(entity(1, "1")), "cycle"},
        {"duplicate id", file(entity(1, "null") + ", " + entity(1, "null")), "duplicate"},
    };
    kast::Scene scene;
    scene.create("Keep me");
    scene.lighting.ambientIntensity = 0.7f;
    for (const auto& [what, text, mention] : bad) {
        const std::string path = (dir / "bad.scene").string();
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        std::string error;
        const bool loaded = kast::loadScene(path, scene, error);
        check(!loaded && !error.empty() && error.find(mention) != std::string::npos, what);
        if (!loaded)
            kast::log::info(std::format("expected error ({}): {}", what, error));
    }
    check(scene.children(entt::null).size() == 1 && scene.lighting.ambientIntensity == 0.7f,
          "failed loads leave the scene untouched");

    // Child listed before its parent still loads, keeping file order among siblings.
    const std::string path = (dir / "good.scene").string();
    std::ofstream(path, std::ios::binary | std::ios::trunc) << file(entity(5, "9") + ", " + entity(9, "null"));
    std::string error;
    check(kast::loadScene(path, scene, error), "child-before-parent file loads");
    check(scene.children(entt::null).size() == 1 && scene.children(scene.children(entt::null)[0]).size() == 1,
          "child-before-parent hierarchy rebuilt");
    check(scene.registry().get<kast::PersistentId>(scene.create("New")).value == 10, "new ids continue after loaded ids");

    std::ofstream(path, std::ios::binary | std::ios::trunc)
        << file(R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [2,0,0,0], "scale": [1,1,1]}}})");
    check(kast::loadScene(path, scene, error), "non-unit quaternion file loads");
    const bx::Quaternion loaded = scene.registry().get<kast::Transform>(scene.children(entt::null)[0]).rotation;
    check(loaded.x == 1.0f && loaded.y == 0.0f && loaded.z == 0.0f && loaded.w == 0.0f, "non-unit quaternion loads normalized");
}

bx::Vec3 transformPoint(const float* m, const bx::Vec3& p) { return bx::mul(p, m); }

void checkCameras()
{
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    check(kast::activeCamera(scene) == entt::null, "no Camera: no active camera");

    const entt::entity first = scene.create("First");
    const entt::entity firstChild = scene.create("First child", first);
    const entt::entity second = scene.create("Second");
    registry.emplace<kast::Camera>(second); // added first, so pool order differs from hierarchy order
    registry.emplace<kast::Camera>(firstChild);
    check(kast::activeCamera(scene) == firstChild, "active camera is the first Camera depth-first");
    registry.remove<kast::Camera>(firstChild);
    check(kast::activeCamera(scene) == second, "active camera falls back to the next one");

    // At (0, 0, 5), unrotated: looks down -Z at the origin, 5 m ahead.
    float view[16], proj[16];
    registry.get<kast::Transform>(second).position = {0, 0, 5};
    kast::cameraMatrices(scene, second, 2.0f, false, view, proj);
    check(near(transformPoint(view, {0, 0, 0}), {0, 0, -5}), "view: origin 5 m ahead of a camera at z=5");
    check(near(transformPoint(view, {1, 2, 5}), {1, 2, 0}), "view: unrotated camera keeps axes");
    float expected[16];
    bx::mtxProj(expected, 60.0f, 2.0f, 0.1f, 1000.0f, false, bx::Handedness::Right);
    check(std::equal(proj, proj + 16, expected), "proj: default fov/near/far and the given aspect");
    check(near(proj[5], 1.0f / std::tan(bx::toRad(30.0f))) && near(proj[0], proj[5] / 2.0f), "proj: 60 deg vertical fov");

    // Under a parent at x=2 yawed +90 deg (local -Z = world -X), scaled 2x: scale must not change the view.
    const entt::entity parent = scene.create("Rig");
    registry.get<kast::Transform>(parent) = {{2, 0, 0}, kast::quatFromEulerDeg({0, 90, 0}), {2, 2, 2}};
    const entt::entity child = scene.create("Child camera", parent);
    registry.emplace<kast::Camera>(child, 90.0f, 1.0f, 100.0f);
    kast::cameraMatrices(scene, child, 1.0f, false, view, proj);
    check(near(transformPoint(view, {-1, 0, 0}), {0, 0, -3}), "view: world camera transform, scale ignored");
    check(near(transformPoint(view, {2, 1, 0}), {0, 1, 0}), "view: camera up is world up");
    check(near(proj[5], 1.0f), "proj: 90 deg fov from the component");
}

// The F3 overlay's FPS (a 0.5 s rolling average of injected frame times) and its text.
void checkStatsOverlay()
{
    kast::FpsAverage fps;
    check(fps.fps() == 0, "stats: FPS 0 before any frame");
    for (int i = 0; i < 100; ++i)
        fps.addFrame(1.0 / 144.0);
    check(fps.fps() == 144, "stats: steady 144 Hz frames -> FPS 144");
    fps.addFrame(0.1); // one hitch only dents a 0.5 s average
    check(fps.fps() >= 100 && fps.fps() < 144, "stats: one 100 ms hitch averages in, no drop to 10");
    for (int i = 0; i < 40; ++i) // 0.67 s of 60 Hz pushes every older frame out of the window
        fps.addFrame(1.0 / 60.0);
    check(fps.fps() == 60, "stats: after 0.67 s at 60 Hz only those count");
    fps.addFrame(1.5); // a 1.5 s stall alone covers the window
    check(fps.fps() == 1, "stats: a 1.5 s frame alone -> FPS 0.67, rounded to 1");
    check(kast::statsOverlayText(144, 42) == "FPS 144  Ping 42 ms", "stats: text 'FPS 144  Ping 42 ms'");
    check(kast::statsOverlayText(60, std::nullopt) == "FPS 60  Ping -- ms", "stats: no RTT -> 'Ping -- ms'");
    check(kast::statsOverlayText(0, 0) == "FPS 0  Ping 0 ms", "stats: zeros print as numbers");
    kast::StatsOverlay overlay;
    check(!overlay.visible(), "stats: the overlay starts hidden");

    StatsToggle game; // the editor's Game panel
    check(!game.visible, "stats: the editor overlay starts hidden");
    check(!game.update(true, true, PlayState::Stopped) && !game.visible, "stats: F3 while Stopped does nothing");
    check(!game.update(true, false, PlayState::Playing) && !game.visible, "stats: F3 with the Game panel unfocused does nothing");
    check(!game.update(false, true, PlayState::Playing) && !game.visible, "stats: no F3, no change");
    check(game.update(true, true, PlayState::Playing) && game.visible, "stats: F3 on the focused Game panel while Playing shows it");
    check(!game.update(false, false, PlayState::Paused) && game.visible, "stats: Pause and focus loss keep it shown");
    check(game.update(true, true, PlayState::Paused) && !game.visible, "stats: F3 while Paused hides it");
    check(game.update(true, true, PlayState::Paused) && game.visible, "stats: F3 while Paused shows it");
    check(game.update(false, true, PlayState::Stopped) && !game.visible, "stats: Stop hides it");
    check(!game.update(false, true, PlayState::Playing) && !game.visible, "stats: the next Play starts with it hidden");
}

void checkInput()
{
    kast::Input input;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_W;
    input.onEvent(event);
    check(input.keysDown[SDL_SCANCODE_W] && !input.keysDown[SDL_SCANCODE_S], "key down recorded");
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = 3.0f;
    event.motion.yrel = -2.0f;
    input.onEvent(event);
    input.onEvent(event);
    check(input.mouseDeltaX == 6.0f && input.mouseDeltaY == -4.0f, "mouse delta accumulates within a frame");
    input.endFrame();
    check(input.mouseDeltaX == 0.0f && input.mouseDeltaY == 0.0f && input.keysDown[SDL_SCANCODE_W],
          "endFrame clears the delta, keeps held keys");
    event = {};
    event.type = SDL_EVENT_KEY_UP;
    event.key.scancode = SDL_SCANCODE_W;
    input.onEvent(event);
    check(!input.keysDown[SDL_SCANCODE_W], "key up recorded");
    input.keysDown[SDL_SCANCODE_A] = true;
    input.clear();
    check(!input.keysDown[SDL_SCANCODE_A], "clear releases every key");

    event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    input.onEvent(event);
    check(input.buttonsDown[2] && !input.buttonsDown[0], "right button down recorded");
    event.button.button = 42; // not a button kast::Input tracks
    input.onEvent(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_RIGHT;
    input.onEvent(event);
    check(!input.buttonsDown[2], "right button up recorded");
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    input.onEvent(event);
    check(input.buttonsDown[0], "left button down recorded");

    event = {};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = 1.0f;
    input.onEvent(event);
    input.onEvent(event);
    event.wheel.y = 0.5f;
    event.wheel.direction = SDL_MOUSEWHEEL_FLIPPED; // natural scrolling: SDL reports it reversed
    input.onEvent(event);
    check(input.wheelDelta == 1.5f, "wheel delta accumulates within a frame, flipped wheels reversed");
    input.endFrame();
    check(input.wheelDelta == 0.0f && input.buttonsDown[0], "endFrame clears the wheel, keeps held buttons");

    input.keysDown[SDL_SCANCODE_W] = true;
    input.cursorLocked = true;
    input.wheelDelta = 2.0f;
    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.onEvent(event);
    check(!input.buttonsDown[0] && !input.keysDown[SDL_SCANCODE_W] && !input.cursorLocked && input.wheelDelta == 0.0f,
          "window focus loss releases keys and buttons, unlocks the cursor and clears the wheel");
    input.buttonsDown[1] = true;
    input.cursorLocked = true;
    input.clear();
    check(!input.buttonsDown[1] && !input.cursorLocked, "clear releases buttons and unlocks the cursor");
}

void checkRotatorSystem()
{
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity spinning = scene.create("Spinning");
    registry.emplace<kast::Rotator>(spinning, bx::Vec3{0, 2, 0}, 90.0f); // axis gets normalized
    const entt::entity still = scene.create("Still");
    registry.emplace<kast::Rotator>(still, bx::Vec3{0, 0, 0}, 90.0f);
    const bx::Quaternion stillStart = kast::quatFromEulerDeg({10, 20, 30});
    registry.get<kast::Transform>(still).rotation = stillStart;

    kast::rotatorSystem(scene, 1.0f);
    check(sameRotation(registry.get<kast::Transform>(spinning).rotation, kast::quatFromEulerDeg({0, 90, 0})),
          "Rotator: 90 deg/s for 1 s = 90 deg about Y");
    const bx::Quaternion q = registry.get<kast::Transform>(still).rotation;
    check(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
              std::memcmp(&q, &stillStart, sizeof(q)) == 0,
          "Rotator: zero axis leaves rotation unchanged and finite");

    // Local axis: after 90 deg about Z, local Y points along world -X; spinning about it keeps it there.
    registry.get<kast::Transform>(spinning).rotation = kast::quatFromEulerDeg({0, 0, 90});
    kast::tickSystems(scene, 1.0f);
    const bx::Quaternion r = registry.get<kast::Transform>(spinning).rotation;
    check(near(rotate(r, {0, 1, 0}), {-1, 0, 0}), "Rotator: spins about the local axis");
    check(near(rotate(r, {1, 0, 0}), {0, 0, -1}), "Rotator: local X turned 90 deg about world -X");
}

void checkSnapshot()
{
    kast::Scene scene;
    const entt::entity sphere = scene.create("Sphere");
    scene.registry().emplace<kast::Rotator>(sphere);
    scene.registry().emplace<kast::MeshRenderer>(sphere, kast::Primitive::Sphere);
    scene.create("Satellite", sphere);
    scene.editorData["camera"] = {{"yaw", 20}};
    const nlohmann::ordered_json snapshot = kast::sceneToJson(scene);
    const std::string before = snapshot.dump(2);

    kast::tickSystems(scene, 0.5f);
    scene.registry().remove<kast::MeshRenderer>(sphere);
    scene.duplicate(sphere);
    scene.create("Extra");
    check(kast::sceneToJson(scene).dump(2) != before, "systems and edits change the scene");

    std::string error;
    check(kast::sceneFromJson(snapshot, scene, error), "snapshot restores");
    check(kast::sceneToJson(scene).dump(2) == before, "restored snapshot is byte-identical");
}

// Editor Play mode on a Document (editor/document.cpp), no window.
void checkPlayMode()
{
    Document doc;
    resetToDefault(doc); // light, ground, cube, camera
    const entt::entity mainCamera = kast::activeCamera(doc.scene);
    check(mainCamera == doc.scene.children(entt::null)[3] &&
              doc.scene.registry().get<kast::Name>(mainCamera).value == "Main Camera",
          "default scene has a Main Camera");
    const entt::entity cube = doc.scene.children(entt::null)[2];
    doc.scene.registry().emplace<kast::Rotator>(cube);
    const kast::EntityId cubeId = doc.scene.registry().get<kast::PersistentId>(cube).value;
    const auto cubeRotation = [&] { // by id: Stop rebuilds the registry
        for (auto [entity, id, transform] : doc.scene.registry().view<kast::PersistentId, kast::Transform>().each())
            if (id.value == cubeId)
                return transform.rotation;
        return bx::Quaternion{0, 0, 0, 0};
    };

    const bx::Quaternion start = cubeRotation();
    updatePlay(doc, 1.0f);
    check(sameRotation(cubeRotation(), start), "systems do not tick in edit mode");

    doc.selected = doc.scene.children(entt::null)[1]; // Ground
    const kast::EntityId groundId = doc.scene.registry().get<kast::PersistentId>(doc.selected).value;
    doc.dirty = false;
    const std::string before = kast::sceneToJson(doc.scene).dump(2);
    check(doc.focusRequest == PanelFocus::None, "no focus request before Play");
    startPlay(doc);
    check(doc.focusRequest == PanelFocus::Game, "Play from Stopped requests Game focus");
    doc.focusRequest = PanelFocus::None; // the Game panel took it
    updatePlay(doc, 1.0f);
    const bx::Quaternion played = cubeRotation();
    check(!sameRotation(played, start), "systems tick while playing");
    togglePause(doc);
    updatePlay(doc, 1.0f);
    check(doc.play == PlayState::Paused && sameRotation(cubeRotation(), played), "systems do not tick while paused");
    startPlay(doc);
    updatePlay(doc, 1.0f);
    check(doc.play == PlayState::Playing && !sameRotation(cubeRotation(), played), "Play resumes from Pause");
    check(doc.focusRequest == PanelFocus::None, "Pause and resume leave focus alone");

    createObject(doc, ObjectKind::Sphere, entt::null); // play-mode edits
    duplicateSelected(doc);
    doc.selected = cube;
    deleteSelected(doc);
    check(!doc.dirty, "play-mode edits do not set dirty");
    selectById(doc, groundId);

    stopPlay(doc);
    check(doc.play == PlayState::Stopped, "Stop");
    check(doc.focusRequest == PanelFocus::Scene, "Stop requests Scene focus");
    check(kast::sceneToJson(doc.scene).dump(2) == before, "Stop restores the byte-identical pre-Play scene");
    check(!doc.dirty, "Stop restores dirty = false");
    check(doc.scene.valid(doc.selected) && doc.scene.registry().get<kast::PersistentId>(doc.selected).value == groundId,
          "Stop restores the selection by id");

    doc.dirty = true;
    startPlay(doc);
    createObject(doc, ObjectKind::Cube, entt::null); // selects a play-only entity
    stopPlay(doc);
    check(doc.dirty, "Stop restores dirty = true");
    check(doc.selected == entt::null, "selection cleared when its entity no longer exists");

    // No camera: Play warns once and still runs.
    const auto warnings = [] {
        return std::ranges::count_if(kast::log::lines(), [](const std::string& line) { return line.find("No Camera") != std::string::npos; });
    };
    doc.scene.destroy(kast::activeCamera(doc.scene));
    const auto warned = warnings();
    startPlay(doc);
    startPlay(doc); // already playing: no second warning
    check(doc.play == PlayState::Playing && warnings() == warned + 1, "Play without a camera warns once and runs");
    stopPlay(doc);

    check(doc.scene.registry().all_of<kast::Camera>(createObject(doc, ObjectKind::Camera, entt::null)),
          "Create Camera adds a Camera");
}

// --- Lua systems ---

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// Rewrites a script with a later mtime, so hot reload sees the change even within the file clock's resolution.
void rewrite(const std::filesystem::path& path, const std::string& text)
{
    const auto before = std::filesystem::last_write_time(path);
    writeFile(path, text);
    std::filesystem::last_write_time(path, before + std::chrono::seconds(2));
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool logged(const std::string& part)
{
    return std::ranges::any_of(kast::log::lines(), [&](const std::string& line) { return contains(line, part); });
}

entt::entity named(const kast::Scene& scene, const std::string& name)
{
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity e) {
        if (found == entt::null && scene.registry().get<kast::Name>(e).value == name)
            found = e;
    });
    return found;
}

kast::LuaSystemStatus statusOf(const kast::LuaSystems& lua, size_t index)
{
    const auto status = lua.status();
    return index < status.size() ? status[index] : kast::LuaSystemStatus{"<missing>", false, "<no status>"};
}

// A system that ran its own Lua asserts reports ok; if not, say why.
void checkOk(const kast::LuaSystems& lua, size_t index, const char* what,
             std::source_location where = std::source_location::current())
{
    const kast::LuaSystemStatus status = statusOf(lua, index);
    check(status.ok, what, where);
    if (!status.ok)
        kast::log::error(std::format("  {}: {}", status.path, status.error));
}

std::filesystem::path luaTestDir()
{
    const std::filesystem::path dir = testRoot() / "kast_lua_check";
    removeTree(dir);
    std::filesystem::create_directories(dir / "scripts");
    return dir;
}

void checkSystemsFormat()
{
    const std::filesystem::path dir = luaTestDir();
    kast::Scene scene;
    scene.create("E");
    scene.systems = {"scripts/b.lua", "scripts/a.lua", "../shared/c.lua"};
    const std::string first = (dir / "first.scene").string(), second = (dir / "second.scene").string();
    std::string error;
    check(kast::saveScene(scene, first, error), "save a scene with systems");
    kast::Scene loaded;
    check(kast::loadScene(first, loaded, error) && loaded.systems == scene.systems, "systems load in listed order");
    check(kast::saveScene(loaded, second, error), "re-save");
    const auto read = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    check(read(first) == read(second) && contains(read(first), "\"version\": 5") && contains(read(first), "\"systems\""),
          "version 5 systems round trip is byte-identical");

    const std::string entity =
        R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}})";
    const auto file = [&](int version, const std::string& systems) {
        return std::format(R"({{"version": {}, "entities": [{}]{}}})", version, entity, systems);
    };
    writeFile(dir / "nosystems.scene", file(5, ""));
    loaded.systems = {"stale.lua"};
    check(kast::loadScene((dir / "nosystems.scene").string(), loaded, error) && loaded.systems.empty(),
          "a version 5 file without \"systems\" loads with none");
    const std::tuple<const char*, std::string, const char*> bad[] = {
        {"systems not an array", file(5, R"(, "systems": "a.lua")"), "\"systems\" must be an array"},
        {"systems entry not a string", file(5, R"(, "systems": ["a.lua", 7])"), "systems[1]: must be a non-empty string"},
        {"systems entry empty", file(5, R"(, "systems": [""])"), "systems[0]: must be a non-empty string"},
        {"version 1 rejected", file(1, ""), "unsupported \"version\" 1"},
        {"version 2 rejected", file(2, ""), "unsupported \"version\" 2"},
        {"version 3 rejected", file(3, R"(, "systems": [])"), "unsupported \"version\" 3"},
        {"version 4 rejected", file(4, ""), "unsupported \"version\" 4"},
        {"version 6 rejected", file(6, ""), "unsupported \"version\" 6"},
    };
    for (const auto& [what, text, mention] : bad) {
        writeFile(dir / "bad.scene", text);
        const bool ok = kast::loadScene((dir / "bad.scene").string(), loaded, error);
        check(!ok && contains(error, mention), what);
        if (!ok)
            kast::log::info(std::format("expected error ({}): {}", what, error));
    }
}

// Play mode on a Document: init on Play, update after the C++ systems in listed order, nothing while paused or
// stopped, shutdown on Stop.
void checkLuaPlayMode()
{
    const std::filesystem::path dir = luaTestDir();
    // Each system appends its digit to the Probe light's intensity, so the order shows in the number.
    const std::string probe = "local function probe() for e in kast.query(\"DirectionalLight\") do return e end end\n";
    writeFile(dir / "scripts/a.lua", probe + R"(
return {
    init = function() kast.set(probe(), "DirectionalLight", {color = {x = 0.5, y = 0, z = 0}}) end,
    update = function(dt)
        local light = kast.get(probe(), "DirectionalLight")
        kast.set(probe(), "DirectionalLight", {intensity = light.intensity * 10 + 1})
    end,
    shutdown = function() kast.log("a shutdown") end,
})");
    writeFile(dir / "scripts/b.lua", probe + R"(
return {
    update = function(dt)
        local light = kast.get(probe(), "DirectionalLight")
        local spun = 0
        for e in kast.query("Rotator") do spun = kast.get(e, "Transform").rotation.y end
        kast.set(probe(), "DirectionalLight", {intensity = light.intensity * 10 + 2, color = {x = light.color.x, y = spun, z = 0}})
    end,
})");

    Document doc;
    doc.path = (dir / "play.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Lua"};
    const entt::entity spinner = doc.scene.create("Spinner");
    doc.scene.registry().emplace<kast::Rotator>(spinner, bx::Vec3{0, 1, 0}, 90.0f);
    doc.scene.registry().emplace<kast::DirectionalLight>(doc.scene.create("Probe"), bx::Vec3{0, 0, 0}, 0.0f);
    doc.scene.systems = {"scripts/a.lua", "scripts/b.lua"};
    const auto light = [&] { return doc.scene.registry().get<kast::DirectionalLight>(named(doc.scene, "Probe")); };

    updatePlay(doc, 1.0f);
    check(light().intensity == 0.0f && !doc.lua.running(), "Lua systems do not run when stopped");
    startPlay(doc);
    check(doc.lua.running() && light().color.x == 0.5f, "Play starts a Lua session and calls init");
    updatePlay(doc, 1.0f);
    check(light().intensity == 12.0f, "Lua systems run in listed order");
    check(near(light().color.y, std::sin(bx::toRad(45.0f))), "Lua systems run after the C++ systems (see this frame's rotation)");
    togglePause(doc);
    updatePlay(doc, 1.0f);
    check(light().intensity == 12.0f, "Lua systems do not run while paused");
    startPlay(doc);
    updatePlay(doc, 1.0f);
    check(light().intensity == 1212.0f, "Lua systems resume after Pause, without re-init");
    check(!logged("a.lua: a shutdown"), "no shutdown before Stop");
    stopPlay(doc);
    check(logged("[info] scripts/a.lua: a shutdown") && !doc.lua.running(), "Stop calls shutdown and ends the session");
    check(light().intensity == 0.0f, "Stop restores the scene the Lua systems changed");
}

// The kast.* API, exercised from Lua (its own asserts disable the system on failure) and checked from C++.
void checkLuaApi()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "api.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
    assert(string.find(err, "api.lua:%d+:"), "error '" .. tostring(err) .. "' has no file:line")
end
local done = false
return { update = function(dt)
    if done then return end
    done = true
    local meshes = {}
    for e in kast.query("Transform", "MeshRenderer") do meshes[#meshes + 1] = e end
    assert(#meshes == 2, "query by two components")
    local cube, cam = meshes[1], nil
    for e in kast.query("Camera") do cam = e end
    local n = 0
    for e in kast.query() do n = n + 1 end
    assert(n == 3, "query with no names visits every entity")

    local t = kast.get(cube, "Transform")
    assert(t.position.x == 1 and t.position.y == 2 and t.rotation.w == 1 and t.scale.z == 1, "get Vec3/Quaternion as tables")
    assert(kast.get(cube, "MeshRenderer").primitive == "cube", "get enum as its name")
    assert(kast.get(cam, "Camera").verticalFovDegrees == 60, "get float")
    kast.set(cube, "Transform", {position = {x = 2, y = 3, z = 4}})
    kast.set(cube, "MeshRenderer", {primitive = "sphere"})
    t = kast.get(cube, "Transform")
    assert(t.position.z == 4 and t.rotation.w == 1, "partial set keeps the other fields")

    assert(not kast.has(cube, "Rotator"))
    kast.add(cube, "Rotator")
    assert(kast.has(cube, "Rotator") and kast.get(cube, "Rotator").degreesPerSecond == 45, "add default-constructs")
    kast.add(cube, "Rotator") -- already there: no-op
    kast.remove(cube, "Rotator")
    kast.remove(cube, "Rotator") -- already gone: no-op
    assert(not kast.has(cube, "Rotator"), "remove")

    fails('"Transform" cannot be removed', function() kast.remove(cube, "Transform") end)
    fails('unknown component "Teleporter"', function() kast.has(cube, "Teleporter") end)
    fails('unknown component "Teleporter"', function() for e in kast.query("Transform", "Teleporter") do end end)
    fails("invalid entity id 999", function() kast.get(999, "Transform") end)
    fails("invalid entity id 1.5", function() kast.get(1.5, "Transform") end)
    fails("invalid entity id x", function() kast.get("x", "Transform") end)
    fails('has no "Camera"', function() kast.get(cube, "Camera") end)
    fails('"verticalFovDegrees" must be between', function() kast.set(cam, "Camera", {verticalFovDegrees = 0}) end)
    fails('"nearPlane" must be a number', function() kast.set(cam, "Camera", {nearPlane = "far", farPlane = 5}) end)
    fails('unknown field "speed"', function() kast.set(cam, "Camera", {speed = 1}) end)
    fails('"position" must be a table', function() kast.set(cube, "Transform", {scale = {x = 2, y = 2, z = 2}, position = {x = 9, y = 9}}) end)
    fails('"scale" must be a table', function() kast.set(cube, "Transform", {position = {x = 9, y = 9, z = 9}, scale = "big"}) end)
    fails("finite number", function() kast.set(cam, "Camera", {farPlane = math.huge}) end)
    fails('"rotation" must be a nonzero quaternion', function() kast.set(cube, "Transform", {rotation = {x = 0, y = 0, z = 0, w = 0}}) end)
    fails('unknown value "torus"', function() kast.set(cube, "MeshRenderer", {primitive = "torus"}) end)
    local light = kast.get(cam, "PointLight")
    assert(light.intensity == 1 and light.range == 10 and light.color.x == 1, "get PointLight defaults")
    kast.set(cam, "PointLight", {intensity = 2, range = 3.5, color = {x = 0.5, y = 0.25, z = 0}})
    light = kast.get(cam, "PointLight")
    assert(light.intensity == 2 and light.range == 3.5 and light.color.y == 0.25, "set PointLight")
    fails('"range" must be > 0', function() kast.set(cam, "PointLight", {range = 0}) end)
    fails('"intensity" must be >= 0', function() kast.set(cam, "PointLight", {intensity = -1, range = 7}) end)
    fails('"color" channels must be between 0 and 1', function() kast.set(cam, "PointLight", {color = {x = 1.5, y = 0, z = 0}}) end)
    fails('unknown key name "NotAKey"', function() kast.input.keyDown("NotAKey") end)
    fails('kast.input.mouseDown: unknown button thumb (left, middle, right, x1 or x2)', function() kast.input.mouseDown("thumb") end)
    fails("kast.input.mouseDown: unknown button 1", function() kast.input.mouseDown(1) end)
    fails("kast.input.setCursorLocked: expected a boolean, got nil", function() kast.input.setCursorLocked() end)
    fails("kast is read-only", function() kast.get = nil end)
    fails("kast is read-only", function() kast.input.keyDown = nil end)

    assert(kast.input.keyDown("W") and not kast.input.keyDown("S"), "keyDown by SDL name")
    local dx, dy = kast.input.mouseDelta()
    assert(dx == 3 and dy == -2, "mouseDelta")
    assert(kast.input.mouseDown("right") and not kast.input.mouseDown("left") and not kast.input.mouseDown("x2"),
           "mouseDown by name")
    assert(kast.input.wheelDelta() == -2, "wheelDelta")
    local origin, dir = kast.input.mouseRay() -- cursor at the top right of a 2:1 view, camera at 0 looking down -z
    assert(origin.x == 0 and origin.y == 0 and origin.z == 0, "mouseRay starts at the camera")
    assert(dir.x > 0 and dir.y > 0 and dir.z < 0 and math.abs(dir.x / dir.y - 2) < 1e-4 and
           math.abs(dir.y / -dir.z - math.tan(math.rad(30))) < 1e-4 and
           math.abs(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z - 1) < 1e-4,
           "mouseRay through the cursor, normalized")
    kast.input.setCursorLocked(true)

    local parent = kast.create("Parent")
    local child = kast.create("Child", parent)
    kast.add(child, "MeshRenderer")
    fails("invalid entity id", function() kast.create("Orphan", 12345) end)
    local visited = {}
    for e in kast.query("MeshRenderer") do
        visited[#visited + 1] = e
        if e == cube then
            kast.destroy(parent) -- takes child with it: skipped later in this loop
            local late = kast.create("Late")
            kast.add(late, "MeshRenderer") -- created during the loop: not visited
        end
    end
    assert(#visited == 2 and visited[1] == cube, "destroy during query skips destroyed entities, new ones not visited")
    fails("invalid entity id", function() kast.has(child, "Transform") end)
    kast.log("hello")
    kast.warn("careful")
    kast.error("oops")
end })");

    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity cube = scene.create("Cube");
    registry.get<kast::Transform>(cube).position = {1, 2, 3};
    registry.emplace<kast::MeshRenderer>(cube, kast::Primitive::Cube);
    registry.emplace<kast::MeshRenderer>(scene.create("Sphere"), kast::Primitive::Sphere);
    const entt::entity cam = scene.create("Camera");
    registry.emplace<kast::Camera>(cam);
    registry.emplace<kast::PointLight>(cam);
    scene.systems = {"api.lua"};
    kast::Input input;
    input.keysDown[SDL_SCANCODE_W] = true;
    input.mouseDeltaX = 3.0f;
    input.mouseDeltaY = -2.0f;
    input.buttonsDown[2] = true;
    input.wheelDelta = -2.0f;
    input.view = {10.0f, 20.0f, 200.0f, 100.0f};
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 210.0f; // the view's top right corner, in window coordinates
    motion.motion.y = 20.0f;
    input.onEvent(motion); // xrel/yrel 0: the deltas above stay

    kast::LuaSystems lua;
    lua.start(scene, dir, "", &input);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.* API asserts in Lua pass");
    check(input.cursorLocked, "kast.input.setCursorLocked asks the host for the lock");
    check(near(registry.get<kast::Transform>(cube).position, {2, 3, 4}) &&
              near(registry.get<kast::Transform>(cube).scale, {1, 1, 1}),
          "set wrote Transform; rejected sets left it unchanged");
    check(registry.get<kast::MeshRenderer>(cube).primitive == kast::Primitive::Sphere, "set wrote an enum by name");
    check(registry.get<kast::Camera>(cam).verticalFovDegrees == 60.0f && registry.get<kast::Camera>(cam).farPlane == 1000.0f,
          "rejected Camera sets left it unchanged");
    const auto& pointLight = registry.get<kast::PointLight>(cam);
    check(pointLight.intensity == 2.0f && pointLight.range == 3.5f && near(pointLight.color, {0.5f, 0.25f, 0.0f}),
          "set wrote PointLight; rejected sets left it unchanged");
    check(named(scene, "Parent") == entt::null && named(scene, "Child") == entt::null, "destroy removes the subtree");
    const entt::entity late = named(scene, "Late");
    check(scene.valid(late) && registry.all_of<kast::MeshRenderer>(late) && scene.parent(late) == entt::null,
          "create adds a root entity through Scene");
    check(logged("[info] api.lua: hello") && logged("[warn] api.lua: careful") && logged("[error] api.lua: oops"),
          "kast.log/warn/error prefix the file");
    lua.stop();
    check(!lua.running() && lua.status().empty(), "stop ends the session");
}

// Stands in for SceneGui (which needs RmlUi and bgfx): records what kast.gui.* asks for.
class FakeGui final : public kast::GuiAccess {
public:
    std::optional<std::string> value(const std::string& id) override
    {
        const auto it = values.find(id);
        return it == values.end() ? std::nullopt : std::optional(it->second);
    }
    void setText(const std::string& id, std::string_view text) override
    {
        texts[id] = std::string(text);
        history.push_back(std::format("text {}={}", id, text));
    }
    void setVisible(const std::string& id, bool visible) override
    {
        visible_[id] = visible;
        history.push_back(std::format("visible {}={}", id, visible));
    }
    void setAttribute(const std::string& id, const std::string& name, std::string_view value) override
    {
        history.push_back(std::format("attribute {}.{}={}", id, name, value));
    }
    bool focus(const std::string& id) override
    {
        history.push_back("focus " + id);
        return id == "chat_input";
    }
    std::optional<std::string> focusedField() override { return focused; }
    bool mouseOver() override { return over; }
    std::optional<std::string> hovered() override { return hoveredId; }
    void reset() override
    {
        ++resets;
        texts.clear();
        visible_.clear();
    }

    std::map<std::string, std::string> values;
    std::map<std::string, std::string> texts;
    std::map<std::string, bool> visible_;
    std::vector<std::string> history; // every write, kept across reset()
    std::optional<std::string> focused;
    bool over = false;
    std::optional<std::string> hoveredId;
    int resets = 0;
};

// kast.items from <project>/items/ (lua_items.h): every field, read-only; a rejected file is logged and left out.
void checkLuaItems()
{
    const std::filesystem::path dir = luaTestDir();
    std::filesystem::create_directories(dir / "items" / "sub");
    writeFile(dir / "items" / "wing.lua", "return {id = 'wing', name = 'Wing', icon = 'icons/wing.png', quality = 'poor', "
                                          "maxStack = 20, sellPrice = 1, description = 'Leathery.'}");
    writeFile(dir / "items" / "sub" / "bad.lua", "return {id = 'bad', name = 'Bad', icon = 'b.png', quality = 'shiny', "
                                                 "maxStack = 1, sellPrice = 0, description = ''}");
    writeFile(dir / "items.lua", R"(return { init = function()
    local w = kast.items.wing
    assert(w.id == "wing" and w.name == "Wing" and w.icon == "icons/wing.png" and w.quality == "poor" and
           w.maxStack == 20 and w.sellPrice == 1 and w.description == "Leathery.", "kast.items: every field")
    assert(kast.items.bad == nil, "a rejected item is left out")
    assert(not pcall(function() kast.items.wing.maxStack = 5 end), "an item is read-only")
    assert(not pcall(function() kast.items.x = {} end), "kast.items is read-only")
end, update = function() end })");
    kast::Scene scene;
    scene.systems = {"items.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.items asserts in Lua pass");
    check(logged("Item rejected: items/sub/bad.lua: \"quality\" must be one of \"poor\", \"common\", \"uncommon\", \"rare\", \"epic\""),
          "a rejected item file is logged with its path and reason");
    lua.stop();
}

// GuiRenderer's filter and gradient compilers (CPU only, no bgfx): every effect RmlUi asks for compiles; an unknown
// or broken one returns 0 (the element draws without it) and warns once however often it is asked for.
void checkGuiEffectCompile()
{
    kast::GuiRenderer renderer;
    const size_t start = kast::log::lines().size();
    const auto warnings = [&](std::string_view text) {
        return std::count_if(kast::log::lines().begin() + static_cast<std::ptrdiff_t>(start), kast::log::lines().end(),
                             [&](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    for (const char* name : {"opacity", "brightness", "contrast", "grayscale", "sepia", "hue-rotate", "saturate", "invert",
                             "blur", "drop-shadow"}) {
        const Rml::CompiledFilterHandle filter = renderer.CompileFilter(name, {{"value", Rml::Variant(0.5f)}});
        check(filter != 0, std::format("GUI filter '{}' compiles", name).c_str());
        renderer.ReleaseFilter(filter);
    }
    const Rml::ColorStopList stops = {{Rml::ColourbPremultiplied(255, 0, 0, 255), Rml::NumericValue(0.0f, Rml::Unit::NUMBER)},
                                      {Rml::ColourbPremultiplied(0, 0, 255, 255), Rml::NumericValue(1.0f, Rml::Unit::NUMBER)}};
    for (const char* name : {"linear-gradient", "radial-gradient", "conic-gradient"})
        for (bool repeating : {false, true}) {
            const Rml::CompiledShaderHandle shader = renderer.CompileShader(
                name, {{"color_stop_list", Rml::Variant(stops)}, {"repeating", Rml::Variant(repeating)}});
            check(shader != 0, std::format("GUI shader '{}' (repeating {}) compiles", name, repeating).c_str());
            renderer.ReleaseShader(shader);
        }

    check(renderer.CompileFilter("glitter", {}) == 0 && renderer.CompileFilter("glitter", {}) == 0,
          "an unknown GUI filter compiles to 0");
    check(warnings("GUI filter 'glitter' is not supported") == 1, "an unknown GUI filter warns once");
    check(renderer.CompileFilter("blur", {{"sigma", Rml::Variant(-2.0f)}}) == 0, "a negative blur compiles to 0");
    check(renderer.CompileFilter("brightness", {{"value", Rml::Variant(std::nanf(""))}}) == 0,
          "a NaN brightness compiles to 0");
    check(warnings("GUI filter 'blur' has invalid parameters") == 1 && warnings("GUI filter 'brightness' has invalid") == 1,
          "bad filter parameters warn");
    check(renderer.CompileShader("noise", {}) == 0 && warnings("GUI shader 'noise' is not supported") == 1,
          "an unknown GUI shader compiles to 0 and warns");
    check(renderer.CompileShader("linear-gradient", {}) == 0 && warnings("GUI linear-gradient has no color stops") == 1,
          "a gradient without stops compiles to 0 and warns");
}

void checkLuaGui()
{
    check(kast::guiTextRml("a < b & \"c\" > d\nnext") == "a &lt; b &amp; &quot;c&quot; &gt; d<br/>next",
          "guiTextRml escapes &, <, >, \" and turns \\n into <br/>");

    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "nogui.lua", R"(return { init = function() assert(kast.gui == nil, "kast.gui without a GUI") end,
    update = function() end })");
    {
        kast::Scene scene;
        scene.systems = {"nogui.lua"};
        kast::LuaSystems lua;
        lua.start(scene, dir, "", nullptr);
        lua.tick(0.1f);
        checkOk(lua, 0, "kast.gui is nil when no GUI is attached");
        lua.stop();
    }

    writeFile(dir / "gui.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
    assert(string.find(err, "gui.lua:%d+:"), "error '" .. tostring(err) .. "' has no file:line")
end
return {
    init = function()
        kast.gui.setText("status", "written in init") -- before any document loads: kept by the GUI
    end,
    update = function()
        assert(kast.gui.value("email") == "me@example.com", "value of a text field")
        local secret = kast.gui.value("password")
        assert(secret == "gui-check-secret", "value of a password field")
        assert(kast.gui.value("nothing") == nil, "value of a missing field is nil")
        kast.gui.setText("password", "")
        kast.gui.setText("log", "a\nb")
        kast.gui.setVisible("realm_2", false)
        assert(kast.gui.focusedField() == "chat_input", "focusedField")
        assert(kast.gui.mouseOver() == true, "mouseOver")
        assert(kast.gui.hovered() == "slot_3", "hovered")
        kast.gui.setAttribute("icon_3", "src", "../textures/icons/wing.png")
        assert(kast.gui.focus("chat_input") == true and kast.gui.focus("nope") == false, "focus")
        fails("kast.gui.value: id must be a string, got 1", function() kast.gui.value(1) end)
        fails("kast.gui.setAttribute: value must be a string, got nil", function() kast.gui.setAttribute("x", "class") end)
        fails("kast.gui.setText: text must be a string, got nil", function() kast.gui.setText("x") end)
        fails("kast.gui.setVisible: visible must be a boolean, got 1", function() kast.gui.setVisible("x", 1) end)
        fails("kast is read-only", function() kast.gui.value = nil end)
        kast.loadScene("next.scene")
    end,
})");
    writeFile(dir / "next.lua", R"(return { init = function() kast.gui.setVisible("hud", true) end,
    update = function()
        assert(kast.gui.focusedField() == nil, "no focused field")
        assert(kast.gui.mouseOver() == false, "not mouseOver")
        assert(kast.gui.hovered() == nil, "nothing hovered")
    end })");
    writeFile(dir / "next.scene", R"({"version": 5, "entities": [], "systems": ["next.lua"]})");

    kast::Scene scene;
    scene.systems = {"gui.lua"};
    FakeGui gui;
    gui.values = {{"email", "me@example.com"}, {"password", "gui-check-secret"}};
    gui.focused = "chat_input";
    gui.over = true;
    gui.hoveredId = "slot_3";
    kast::Input input;
    input.buttonsDown[0] = true; // held into the switch
    input.cursorLocked = true;
    kast::LuaSystems lua;
    lua.setGui(&gui);
    lua.start(scene, dir, "", &input);
    check(gui.resets == 1 && gui.texts["status"] == "written in init", "reset before the inits, which may write");
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.gui.* asserts in Lua pass");
    check(gui.history == std::vector<std::string>{"text status=written in init", "text password=", "text log=a\nb",
                                                  "visible realm_2=false", "attribute icon_3.src=../textures/icons/wing.png",
                                                  "focus chat_input", "focus nope",
                                                  "visible hud=true"},
          "setText / setVisible / setAttribute reach the GUI with their arguments, in call order");
    check(gui.resets == 2 && gui.texts.empty() && gui.visible_ == std::map<std::string, bool>{{"hud", true}},
          "a kast.loadScene switch resets the GUI before the new scene's inits");
    check(!input.buttonsDown[0] && !input.cursorLocked, "a kast.loadScene switch releases buttons and the cursor lock");
    gui.focused.reset();
    gui.over = false;
    gui.hoveredId.reset();
    lua.tick(0.1f);
    checkOk(lua, 0, "focusedField is nil without a focused text field");
    check(!logged("gui-check-secret"), "no text field value is logged");
    lua.stop();
}

void checkLuaSandbox()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "one.lua", R"(
for _, name in ipairs({"io", "os", "ffi", "debug", "require", "loadfile", "dofile", "package", "jit", "bit",
                       "load", "loadstring", "getfenv", "setfenv", "module"}) do
    assert(_G[name] == nil and rawget(_G, name) == nil, name .. " is reachable")
end
name = "one"
return {
    init = function() counter = 1 end,
    update = function(dt)
        assert(name == "one" and counter == 1, "globals are per system")
        assert(math.pi > 3.14, "library tables are per system")
        assert(kast.input.keyDown("W") == false, "no input: keys are up")
        local dx, dy = kast.input.mouseDelta()
        assert(dx == 0 and dy == 0, "no input: no mouse motion")
        assert(kast.input.mouseDown("left") == false and kast.input.wheelDelta() == 0, "no input: no buttons, no wheel")
        kast.input.setCursorLocked(true) -- no input: nothing to lock
    end,
})");
    writeFile(dir / "two.lua", R"(
name = "two"
math.pi = 3
return {
    init = function() counter = 100 end,
    update = function(dt) assert(name == "two" and counter == 100 and math.pi == 3, "own globals") end,
})");
    kast::Scene scene;
    scene.systems = {"one.lua", "two.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "sandbox: unsafe libraries unreachable, globals isolated (one)");
    checkOk(lua, 1, "sandbox: globals isolated (two)");
    lua.stop();
}

// The string metatable is shared by every system: one can't reach it to change another's s:format().
void checkLuaStringMetatable()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "evil.lua", R"(
pcall(function() getmetatable("").__index.format = function() return "evil" end end)
assert(getmetatable("") == false, "the string metatable is hidden")
assert(("%d"):format(7) == "7" and ("ab"):upper() == "AB", "string methods work")
return { update = function(dt) end })");
    writeFile(dir / "victim.lua", R"(
return { update = function(dt) assert(("%d"):format(7) == "7", "s:format was changed by another system") end })");
    kast::Scene scene;
    scene.systems = {"evil.lua", "victim.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "sandbox: string metatable hidden, string methods work");
    checkOk(lua, 1, "sandbox: string methods untouched by another system");
    lua.stop();
}

// kast.create past the hierarchy depth cap raises a Lua error instead of building a chain that crashes the walks.
void checkLuaDepthCap()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "deep.lua", std::format(R"(
return {{ update = function(dt)
    local id
    for i = 1, {} do id = kast.create("E", id) end
    local ok, err = pcall(kast.create, "E", id)
    assert(not ok and err:find("parent is already {} levels deep", 1, true), "create past the cap: " .. tostring(err))
end }})", kast::kMaxHierarchyDepth, kast::kMaxHierarchyDepth));
    kast::Scene scene;
    scene.systems = {"deep.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.create past the hierarchy depth cap raises a Lua error");
    lua.stop();
}

void checkLuaErrors()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "counter.lua", R"(return { update = function(dt)
    for e in kast.query("DirectionalLight") do
        kast.set(e, "DirectionalLight", {intensity = kast.get(e, "DirectionalLight").intensity + 1})
    end
end })");
    writeFile(dir / "syntax.lua", "return {\n update = function(dt) local x = end\n}");
    writeFile(dir / "runtime.lua",
              "local calls = 0\nreturn { update = function(dt)\n  calls = calls + 1\n  if calls == 2 then error(\"boom\") end\nend }");
    writeFile(dir / "badapi.lua", "return { update = function(dt)\n  kast.get(424242, \"Transform\")\nend }");
    writeFile(dir / "noupdate.lua", "return { init = function() end }");
    writeFile(dir / "notable.lua", "return 42");
    writeFile(dir / "badinit.lua", "return {\n  init = function() local t = nil; return t.x end,\n  update = function(dt) end }");

    kast::Scene scene;
    const entt::entity probe = scene.create("Probe");
    scene.registry().emplace<kast::DirectionalLight>(probe, bx::Vec3{1, 1, 1}, 0.0f);
    scene.systems = {"syntax.lua", "runtime.lua", "counter.lua", "badapi.lua", "noupdate.lua", "notable.lua",
                     "badinit.lua", "missing.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    const auto disabled = [&](size_t index, const std::vector<std::string>& parts, const char* what,
                              std::source_location where = std::source_location::current()) {
        const kast::LuaSystemStatus status = statusOf(lua, index);
        const bool ok = !status.ok && std::ranges::all_of(parts, [&](const std::string& part) { return contains(status.error, part); });
        check(ok, what, where);
        if (!ok)
            kast::log::error(std::format("  {}: ok={} error='{}'", status.path, status.ok, status.error));
    };
    disabled(0, {"syntax.lua:2:"}, "syntax error: disabled, error has file:line");
    disabled(4, {"noupdate.lua", "update"}, "missing update: disabled");
    disabled(5, {"notable.lua", "must return a table"}, "non-table result: disabled");
    disabled(6, {"badinit.lua:2:"}, "init error: disabled with file:line");
    disabled(7, {"missing.lua", "cannot open"}, "missing file: disabled");
    check(logged("Lua system disabled: syntax.lua:2:"), "load errors are logged with file:line");

    lua.tick(0.1f);
    checkOk(lua, 1, "runtime.lua runs fine on its first call");
    disabled(3, {"badapi.lua:2:", "invalid entity id 424242"}, "kast.* error: disabled with file:line");
    lua.tick(0.1f);
    disabled(1, {"runtime.lua:4:", "boom"}, "runtime error: disabled with file:line");
    lua.tick(0.1f);
    checkOk(lua, 2, "other systems keep running");
    check(scene.registry().get<kast::DirectionalLight>(probe).intensity == 3.0f, "the healthy system ran every tick");
    lua.stop();
}

void checkLuaHotReload()
{
    const std::filesystem::path dir = luaTestDir();
    const std::filesystem::path file = dir / "hot.lua";
    const auto version = [](int add, const char* tag) {
        return std::format(R"(return {{
    init = function() kast.log("init {0}") end,
    update = function(dt)
        for e in kast.query("DirectionalLight") do
            kast.set(e, "DirectionalLight", {{intensity = kast.get(e, "DirectionalLight").intensity + {1}}})
        end
    end,
    shutdown = function() kast.log("shutdown {0}") end,
}})",
                           tag, add);
    };
    writeFile(file, version(1, "v1"));
    kast::Scene scene;
    const entt::entity probe = scene.create("Probe");
    scene.registry().emplace<kast::DirectionalLight>(probe, bx::Vec3{1, 1, 1}, 0.0f);
    scene.systems = {"hot.lua"};
    const auto intensity = [&] { return scene.registry().get<kast::DirectionalLight>(probe).intensity; };
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    check(intensity() == 1.0f && logged("hot.lua: init v1"), "v1 runs");

    rewrite(file, version(10, "v2"));
    lua.tick(0.1f);
    check(logged("hot.lua: shutdown v1") && logged("hot.lua: init v2") && logged("Reloaded Lua system hot.lua"),
          "hot reload: shutdown old, init new");
    check(intensity() == 11.0f, "hot reload: new behaviour, scene not reset");

    rewrite(file, "return { update = function(dt) local = end }");
    lua.tick(0.1f);
    check(!statusOf(lua, 0).ok && contains(statusOf(lua, 0).error, "hot.lua:1:") && intensity() == 11.0f,
          "reload with a syntax error disables the system");
    lua.tick(0.1f);
    check(intensity() == 11.0f, "disabled system does not run");

    rewrite(file, version(100, "v3"));
    lua.tick(0.1f);
    check(statusOf(lua, 0).ok && statusOf(lua, 0).error.empty() && intensity() == 111.0f,
          "a good reload clears the error and runs again");
    lua.stop();
    check(logged("hot.lua: shutdown v3"), "Stop shuts down the reloaded instance");
}

// --- Projects ---

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

ptrdiff_t countLogged(const std::string& part)
{
    return std::ranges::count_if(kast::log::lines(), [&](const std::string& line) { return contains(line, part); });
}

// Index of the first log line mentioning part; SIZE_MAX if none.
size_t logIndex(const std::string& part)
{
    const std::vector<std::string>& lines = kast::log::lines();
    const auto it = std::ranges::find_if(lines, [&](const std::string& line) { return contains(line, part); });
    return it == lines.end() ? SIZE_MAX : static_cast<size_t>(it - lines.begin());
}

// A fresh temp directory holding a project.kast (and an empty scripts/).
std::filesystem::path projectTestDir(const char* name, const char* projectName)
{
    const std::filesystem::path dir = testRoot() / name;
    removeTree(dir);
    std::filesystem::create_directories(dir / "scripts");
    writeFile(dir / kast::kProjectFileName, std::format(R"({{"version": 1, "name": "{}", "startScene": null}})", projectName));
    return dir;
}

std::filesystem::path looseTestDir(const char* name) // no project.kast
{
    const std::filesystem::path dir = testRoot() / name;
    removeTree(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void saveTestScene(const std::filesystem::path& file, const char* entity, std::vector<std::string> systems)
{
    kast::Scene scene;
    scene.create(entity);
    scene.systems = std::move(systems);
    std::string error;
    check(kast::saveScene(scene, file.string(), error), "write a test scene");
}

// Narrow strings are UTF-8 (cmake/utf8.manifest + /utf-8): path(std::string) and path::string() round-trip non-ASCII
// names instead of going through a legacy code page (which mangled them, or threw for characters outside it).
void checkUtf8Paths()
{
    const std::string name = "J\xC3\xBCrgen \xE6\x97\xA5\xE6\x9C\xAC"; // "Jürgen 日本" as UTF-8 bytes
    check(std::string("Jürgen 日本") == name, "source literals are compiled as UTF-8");
    const std::wstring wide = L"Jürgen 日本";
    check(std::filesystem::path(name).wstring() == wide, "path(std::string) reads UTF-8");
    std::string narrow;
    try {
        narrow = std::filesystem::path(wide).string();
    } catch (const std::system_error&) { // no mapping in the code page
    }
    check(narrow == name, "path::string() writes UTF-8");

    const std::filesystem::path dir = projectTestDir(("kast_utf8_" + name).c_str(), name.c_str());
    check(std::filesystem::exists(testRoot() / (L"kast_utf8_" + wide) / kast::kProjectFileName),
          "a project made through narrow paths lands in the non-ASCII folder");
    kast::Project project;
    std::string error;
    check(kast::loadProject(dir / kast::kProjectFileName, project, error) && project.name == name,
          "load a project from a non-ASCII folder");
    saveTestScene(dir / "a.scene", "e", {});
    kast::Scene scene;
    check(kast::loadScene((dir / "a.scene").string(), scene, error), "save and load a scene by a non-ASCII std::string path");
}

void checkProjectFiles()
{
    check(!kast::findProjectDir(std::filesystem::temp_directory_path()),
          "precondition: no project.kast above the temp directory");
    const std::filesystem::path dir = looseTestDir("kast_project_check");
    const std::filesystem::path root = (dir / "a").lexically_normal();
    std::filesystem::create_directories(dir / "a/b/c");
    std::string error;
    kast::Project project{.root = root, .name = "Demo"};
    check(kast::saveProject(project, error), "save project.kast");
    check(readText(root / "project.kast") == "{\n  \"version\": 1,\n  \"name\": \"Demo\",\n  \"startScene\": null\n}\n",
          "project.kast is written as deterministic JSON (2-space indent, fixed key order)");
    project.startScene = "scenes/start.scene";
    check(kast::saveProject(project, error), "save project.kast with a startScene");
    kast::Project loaded;
    check(kast::loadProject(root / "project.kast", loaded, error) && loaded.name == "Demo" &&
              loaded.startScene == "scenes/start.scene" && loaded.root == root,
          "project.kast round trip");

    const std::tuple<const char*, std::string, const char*> bad[] = {
        {"invalid JSON", R"({"version": 1,)", "Invalid project"},
        {"not an object", "[]", "must be a JSON object"},
        {"missing version", R"({"name": "X", "startScene": null})", "missing \"version\""},
        {"missing name", R"({"version": 1, "startScene": null})", "missing \"name\""},
        {"missing startScene", R"({"version": 1, "name": "X"})", "missing \"startScene\""},
        {"unknown key", R"({"version": 1, "name": "X", "startScene": null, "extra": 1})", "unknown key \"extra\""},
        {"wrong version", R"({"version": 2, "name": "X", "startScene": null})", "unsupported \"version\" 2"},
        {"version not a number", R"({"version": "1", "name": "X", "startScene": null})", "unsupported \"version\""},
        {"empty name", R"({"version": 1, "name": "", "startScene": null})", "\"name\" must be a non-empty string"},
        {"name not a string", R"({"version": 1, "name": 5, "startScene": null})", "\"name\" must be a non-empty string"},
        {"startScene not a string", R"({"version": 1, "name": "X", "startScene": 5})", "\"startScene\" must be a string or null"},
    };
    std::filesystem::create_directories(dir / "bad");
    for (const auto& [what, text, mention] : bad) {
        writeFile(dir / "bad/project.kast", text);
        const bool ok = kast::loadProject(dir / "bad/project.kast", loaded, error);
        check(!ok && contains(error, mention) && loaded.name == "Demo", what);
        if (!ok)
            kast::log::info(std::format("expected error ({}): {}", what, error));
    }
    check(!kast::loadProject(dir / "none/project.kast", loaded, error) && contains(error, "Cannot open project"),
          "missing project.kast");

    // Walk-up discovery: nearest ancestor-or-self.
    check(kast::findProjectDir(dir / "a/b/c") == root, "walk-up finds an ancestor's project.kast");
    check(kast::findProjectDir(root) == root, "walk-up includes the directory itself");
    check(!kast::findProjectDir(dir), "no project.kast up the tree: none");
    kast::saveProject({.root = root / "b", .name = "Inner"}, error);
    check(kast::findProjectDir(dir / "a/b/c") == root / "b", "walk-up stops at the nearest project");
    const auto warned = countLogged("no project is active");
    check(!kast::discoverProject(dir / "bad") && countLogged("no project is active") == warned + 1,
          "an invalid project.kast is warned about and treated as no project");
    check(kast::discoverProject(dir / "a/b/c")->name == "Inner", "discoverProject loads the project found");

    // Project-relative paths.
    check(kast::toProjectPath(root, root / "scripts/x.lua") == "scripts/x.lua", "toProjectPath: '/' separated, relative");
    check(kast::toProjectPath(root, root / "sub/../x.lua") == "x.lua", "toProjectPath normalizes");
    check(!kast::toProjectPath(root, dir / "other.lua"), "toProjectPath: outside the root is rejected");
    check(!kast::toProjectPath(root, root), "toProjectPath: the root itself is not a file in it");
    check(kast::resolveProjectPath(root, "scripts/x.lua", error) == root / "scripts/x.lua", "resolveProjectPath");
    check(kast::resolveProjectPath(root, "a/../b.scene", error) == root / "b.scene", "resolveProjectPath normalizes");
    check(kast::resolveProjectPath(root, "scripts\\x.lua", error) == root / "scripts/x.lua", "backslashes work on Windows");
    const std::pair<const char*, const char*> escapes[] = {
        {"../x.scene", "outside the project"},
        {"a/../../x.scene", "outside the project"},
        {"..", "outside the project"},
        {"C:/x.scene", "must be relative to the project root"},
        {"/x.scene", "must be relative to the project root"},
        {"", "empty"},
    };
    for (const auto& [path, mention] : escapes) {
        error.clear();
        check(!kast::resolveProjectPath(root, path, error) && contains(error, mention), path);
    }
}

// Opening scenes and projects on a Document (the File menu adds dialogs and modals on top).
void checkProjectDocument()
{
    const std::filesystem::path dir = projectTestDir("kast_projdoc_check", "Alpha");
    const std::filesystem::path outside = looseTestDir("kast_projdoc_outside");
    std::filesystem::create_directories(dir / "scenes");
    saveTestScene(dir / "scenes/start.scene", "Start", {});
    saveTestScene(outside / "loose.scene", "Loose", {});
    std::string error;

    Document doc;
    resetToDefault(doc);
    check(!doc.project && windowTitle(doc) == "Kast Editor - Untitled", "no project: title keeps its format");
    check(openFile(doc, (dir / "scenes/start.scene").string(), error) && doc.project && doc.project->name == "Alpha",
          "Open: the project is found walking up from the scene");
    check(projectScenePath(doc) == "scenes/start.scene", "the scene's project-relative path");
    check(windowTitle(doc) == "Kast Editor - Alpha - start", "title with a project");
    check(windowTitle(doc, true) == "Kast Editor - Alpha - start*", "title with unsaved zone changes only");
    doc.dirty = true;
    check(windowTitle(doc) == "Kast Editor - Alpha - start*", "title with a project and unsaved changes");
    resetToDefault(doc);
    check(doc.project && doc.project->name == "Alpha" && doc.path.empty() && projectScenePath(doc).empty(),
          "New keeps the active project");
    check(openFile(doc, (outside / "loose.scene").string(), error) && !doc.project,
          "Open outside any project: no project");

    // Open Project: invalid files change nothing.
    const std::string loose = kast::sceneToJson(doc.scene).dump();
    std::filesystem::create_directories(outside / "bad");
    writeFile(outside / "bad/project.kast", R"({"version": 1})");
    check(!openProject(doc, outside / "bad/project.kast", error) && contains(error, "Invalid project") &&
              kast::sceneToJson(doc.scene).dump() == loose && !doc.project && contains(doc.path, "loose"),
          "Open Project: an invalid project.kast is rejected and changes nothing");
    writeFile(outside / "other.kast", R"({"version": 1, "name": "X", "startScene": null})");
    check(!openProject(doc, outside / "other.kast", error) && contains(error, "not a project.kast") && !doc.project,
          "Open Project: only files named project.kast");

    check(openProject(doc, dir / "project.kast", error) && doc.project && doc.project->name == "Alpha" &&
              doc.path.empty() && !doc.dirty && named(doc.scene, "Ground") != entt::null,
          "Open Project, startScene null: a new default Untitled scene with the project active");

    kast::saveProject({.root = dir, .name = "Alpha", .startScene = "scenes/start.scene"}, error);
    check(openProject(doc, dir / "project.kast", error) && named(doc.scene, "Start") != entt::null &&
              std::filesystem::path(doc.path) == (dir / "scenes/start.scene").lexically_normal() && doc.project,
          "Open Project opens startScene");

    for (const char* start : {"scenes/missing.scene", "../kast_projdoc_outside/loose.scene"}) {
        kast::saveProject({.root = dir, .name = "Alpha", .startScene = start}, error);
        const auto warned = countLogged(std::format("Cannot open the project's startScene '{}'", start));
        check(openProject(doc, dir / "project.kast", error) && doc.project && doc.path.empty() &&
                  named(doc.scene, "Ground") != entt::null &&
                  countLogged(std::format("Cannot open the project's startScene '{}'", start)) == warned + 1,
              "Open Project: a startScene that can't load logs why and opens a new Untitled scene");
    }

    // New Project: folder name sanitizing, validation, then <dir>/<folder>/project.kast opened like Open Project.
    check(projectFolderName("My Game") == "my-game" && projectFolderName("  My   Game  ") == "my-game" &&
              projectFolderName("My\tCool  Game") == "my-cool-game" && projectFolderName("RPG2") == "rpg2" &&
              projectFolderName("\xC3\x89pic Quest") == "\xC3\x89pic-quest" && projectFolderName(" \t ").empty(),
          "New Project folder name: trimmed, ASCII lower-cased, whitespace runs become '-'");
    const std::filesystem::path parent = looseTestDir("kast_projdoc_new");
    const std::string parentDir = parent.string();
    check(newProjectTarget("My Game", parentDir) == (parent / "my-game").lexically_normal(), "New Project target folder");
    check(newProjectError("My Game", parentDir).empty(), "New Project: a valid name and directory");
    check(contains(newProjectError("   ", parentDir), "Enter a project name"), "New Project rejects a blank name");
    for (const char* bad : {"a<b", "a>b", "a:b", "a\"b", "a/b", "a\\b", "a|b", "a?b", "a*b", "a\x01b"})
        check(contains(newProjectError(bad, parentDir), "must not contain"),
              std::format("New Project rejects an invalid character in '{}'", bad).c_str());
    check(contains(newProjectError(".", parentDir), "not a valid folder name") &&
              contains(newProjectError(" .. ", parentDir), "not a valid folder name"),
          "New Project rejects . and ..");
    check(contains(newProjectError("game.", parentDir), "must not end with '.'"), "New Project rejects a trailing '.'");
    check(contains(newProjectError("My Game", ""), "Choose a directory"), "New Project needs a directory");
    check(contains(newProjectError("My Game", (parent / "missing").string()), "does not exist"),
          "New Project rejects a missing directory");
    writeFile(parent / "file.txt", "x");
    check(contains(newProjectError("My Game", (parent / "file.txt").string()), "does not exist"),
          "New Project rejects a file as the directory");
    std::filesystem::create_directories(parent / "taken");
    check(contains(newProjectError("Taken", parentDir), "already exists") &&
              contains(newProjectError("File.txt", parentDir), "already exists"),
          "New Project rejects an existing target folder or file");

    std::filesystem::path created;
    check(!createProject("My Game", (parent / "missing").string(), created, error) && contains(error, "does not exist") &&
              !std::filesystem::exists(parent / "missing"),
          "New Project never creates missing parent directories");
    check(!createProject("Taken", parentDir, created, error) && contains(error, "already exists") &&
              std::filesystem::is_empty(parent / "taken"),
          "New Project refuses an existing folder and writes nothing into it");
    check(createProject("  My Game ", parentDir, created, error) && created == parent / "my-game" &&
              readText(parent / "my-game/project.kast") ==
                  "{\n  \"version\": 1,\n  \"name\": \"My Game\",\n  \"startScene\": \"scenes/main.scene\"\n}\n",
          "New Project creates <dir>/my-game/project.kast with the trimmed name and startScene scenes/main.scene");
    Document defaults;
    resetToDefault(defaults);
    kast::Scene mainScene;
    check(kast::loadScene((created / "scenes/main.scene").string(), mainScene, error) &&
              mainScene.registry().view<kast::Name>().size() == 4 && named(mainScene, "Main Camera") != entt::null &&
              named(mainScene, "Directional Light") != entt::null && named(mainScene, "Ground") != entt::null &&
              named(mainScene, "Cube") != entt::null &&
              mainScene.editorData["camera"] == defaults.camera.toJson(),
          "New Project writes scenes/main.scene: the default scene and its editor camera");
    check(projectScenes(created) == std::vector<std::string>{"scenes/main.scene"},
          "Build Settings lists the new project's scenes/main.scene");
    if (kast::Project made; kast::loadProject(created / "project.kast", made, error))
        check(checkStartScene(made, error), "a new project's startScene passes the build check");
    else
        check(false, "the new project.kast loads");
    check(!createProject("My Game", parentDir, created, error) && contains(error, "already exists"),
          "New Project refuses to create the same project twice");
    const auto warned = countLogged("Cannot open the project's startScene");
    check(openProject(doc, created / "project.kast", error) && doc.project->name == "My Game" &&
              std::filesystem::path(doc.path) == (created / "scenes/main.scene").lexically_normal() &&
              displayName(doc) == "main" && windowTitle(doc) == "Kast Editor - My Game - main" &&
              countLogged("Cannot open the project's startScene") == warned,
          "the new project opens its startScene scenes/main.scene without a warning");
    startPlay(doc);
    check(windowTitle(doc) == "Kast Editor - My Game - main (Playing)", "title while playing");
    togglePause(doc);
    check(windowTitle(doc) == "Kast Editor - My Game - main (Paused)", "title while paused");
    stopPlay(doc);
}

// Project Manager recent list: JSON round trip, bump to top without duplicates, Remove, unusable entries, corrupt file.
void checkRecentProjects()
{
    const std::filesystem::path root = looseTestDir("kast_recent_check");
    const std::filesystem::path file = root / "lists/projects.json";
    const std::filesystem::path alpha = projectTestDir("kast_recent_check/Alpha", "Alpha");
    const std::filesystem::path beta = projectTestDir("kast_recent_check/beta", "Beta");
    std::string error;

    check(loadRecent(file).empty(), "no list file: an empty list");
    std::vector<RecentProject> list;
    bumpRecent(list, alpha, "Alpha", 100);
    bumpRecent(list, beta / "scripts" / "..", "Beta", 200);
    check(list.size() == 2 && list[0].dir == beta && list[0].lastOpened == 200 && list[1].name == "Alpha",
          "bump: newest first, the path made normal");
    check(saveRecent(file, list, error) && std::filesystem::exists(file), "the list is saved, creating its folder");
    std::vector<RecentProject> loaded = loadRecent(file);
    check(loaded.size() == 2 && loaded[0].dir == beta && loaded[0].name == "Beta" && loaded[0].lastOpened == 200 &&
              loaded[1].dir == alpha && loaded[1].lastOpened == 100,
          "save/load round trip keeps path, name, time and order");
    check(contains(readText(file), "[\n  {\n    \"path\": \"" + beta.generic_string() + "\",\n"),
          "2-space indent, '/'-separated paths");

    std::string upper = alpha.string();
    std::ranges::transform(upper, upper.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
    bumpRecent(loaded, upper + "\\", "Alpha", 300);
    check(loaded.size() == 2 && loaded[0].name == "Alpha" && loaded[0].lastOpened == 300 && loaded[1].name == "Beta",
          "bump of a listed folder (other case, trailing '\\') moves it to the top without a duplicate");
    loaded.erase(loaded.begin() + 1); // Remove
    check(saveRecent(file, loaded, error) && loadRecent(file).size() == 1 && std::filesystem::exists(beta / "project.kast"),
          "Remove drops the entry only; the project stays on disk");

    check(recentProblem({alpha, "Alpha", 1}).empty(), "a valid project has no problem");
    check(recentProblem({root / "gone", "Gone", 1}) == "Folder not found", "a missing folder is reported");
    writeFile(beta / "project.kast", R"({"version": 1})");
    check(contains(recentProblem({beta, "Beta", 1}), "Invalid project"), "an invalid project.kast is reported");
    std::filesystem::remove(beta / "project.kast");
    check(!recentProblem({beta, "Beta", 1}).empty(), "a folder without project.kast is reported");

    kast::Project project;
    check(kast::loadProject(alpha / "project.kast", project, error) && recordRecent(file, project, error) &&
              loadRecent(file).size() == 1 && loadRecent(file)[0].dir == alpha,
          "recordRecent: an open project is listed once, at the top");

    for (const char* bad : {"{not json", R"({"path": "x"})", R"([{"path": "x", "name": 3, "lastOpened": 1}])"}) {
        writeFile(file, bad);
        std::filesystem::remove(root / "lists/projects.json.bad");
        const auto warned = countLogged("is invalid");
        check(loadRecent(file).empty() && readText(root / "lists/projects.json.bad") == bad &&
                  countLogged("is invalid") == warned + 1,
              std::format("a corrupt list ({}) is backed up to projects.json.bad, warned about and read as empty", bad).c_str());
    }
    check(!launchProcess(root / "missing.exe", {"--project", alpha.string()}, error) && contains(error, "Cannot start"),
          "a process that can't start (the Project Manager's editor) reports why");
}

// No active project: C++ systems still run, every Lua system is disabled, and the Systems panel can't add.
void checkNoProjectLua()
{
    const std::filesystem::path dir = looseTestDir("kast_noproject_check");
    std::filesystem::create_directories(dir / "scripts");
    writeFile(dir / "scripts/a.lua", "return { update = function(dt) end }");
    Document doc;
    resetToDefault(doc);
    doc.path = (dir / "loose.scene").string();
    const entt::entity cube = doc.scene.children(entt::null)[2];
    doc.scene.registry().emplace<kast::Rotator>(cube);
    doc.scene.systems = {"scripts/a.lua", "scripts/b.lua"};
    const bx::Quaternion start = doc.scene.registry().get<kast::Transform>(cube).rotation;
    startPlay(doc);
    updatePlay(doc, 1.0f);
    check(!sameRotation(doc.scene.registry().get<kast::Transform>(cube).rotation, start), "no project: C++ systems run");
    for (size_t i = 0; i < 2; ++i)
        check(!statusOf(doc.lua, i).ok && contains(statusOf(doc.lua, i).error, "no project"),
              "no project: each Lua system is disabled with a 'no project' error");
    check(logged("Lua system disabled: scripts/a.lua: no project"), "the 'no project' error reaches the Console");
    check(doc.lua.scenePath().empty(), "no project: no current scene path");
    stopPlay(doc);

    std::string error;
    check(!addSystem(doc, dir / "scripts/a.lua", error) && contains(error, "project") && doc.scene.systems.size() == 2,
          "Add Script needs a project");
    check(!newSystemFromTemplate(doc, error) && contains(error, "project") &&
              !std::filesystem::exists(dir / "scripts/new_system.lua"),
          "New from Template needs a project");
}

// Systems panel operations on a Document (the panel only adds ImGui widgets on top).
void checkSystemEdits()
{
    const std::filesystem::path dir = projectTestDir("kast_edits_check", "Edits");
    const std::filesystem::path other = projectTestDir("kast_edits_other", "Other");
    const std::filesystem::path loose = looseTestDir("kast_edits_loose");
    Document doc;
    resetToDefault(doc);
    doc.project = kast::discoverProject(dir);
    std::string error;
    check(!newSystemFromTemplate(doc, error) && error == "Save the scene first" && doc.scene.systems.empty(),
          "template needs a saved scene");
    writeFile(dir / "other.lua", "return { update = function(dt) end }");
    check(addSystem(doc, dir / "other.lua", error) && doc.scene.systems == std::vector<std::string>{"other.lua"} && doc.dirty,
          "Add Script works on an Untitled scene in a project and stores a project-relative path");
    doc.dirty = false;
    writeFile(loose / "outside.lua", "return { update = function(dt) end }");
    check(!addSystem(doc, loose / "outside.lua", error) && contains(error, "outside the project") &&
              doc.scene.systems.size() == 1 && !doc.dirty,
          "Add Script rejects a file outside the project");

    std::filesystem::create_directories(dir / "scenes");
    doc.path = (dir / "scenes/edit.scene").string();
    check(newSystemFromTemplate(doc, error) && newSystemFromTemplate(doc, error), "template written");
    check((doc.scene.systems == std::vector<std::string>{"other.lua", "scripts/new_system.lua", "scripts/new_system_2.lua"}) &&
              std::filesystem::exists(dir / "scripts/new_system_2.lua") && !std::filesystem::exists(dir / "scenes/scripts") &&
              doc.dirty,
          "template files go to <project>/scripts with unique names, stored project-relative, and set dirty");
    moveSystem(doc, 2, -1);
    moveSystem(doc, 0, -1); // past the top: no-op
    check((doc.scene.systems == std::vector<std::string>{"other.lua", "scripts/new_system_2.lua", "scripts/new_system.lua"}),
          "move up / down");
    removeSystem(doc, 2);
    check((doc.scene.systems == std::vector<std::string>{"other.lua", "scripts/new_system_2.lua"}), "remove");

    check(canEditSystems(doc), "systems editable when stopped");
    startPlay(doc);
    check(!canEditSystems(doc), "systems not editable during Play");
    checkOk(doc.lua, 1, "the template is a working system (resolved from the project root)");
    updatePlay(doc, 0.1f);
    checkOk(doc.lua, 1, "the template's update runs");
    stopPlay(doc);

    // Save As never rebases; moving to another project or none warns.
    const std::string warning = "may no longer resolve";
    const std::vector<std::string> systems = doc.scene.systems;
    const auto warned = countLogged(warning);
    std::filesystem::create_directories(dir / "sub");
    check(saveFile(doc, (dir / "sub/moved.scene").string(), error) && doc.scene.systems == systems &&
              countLogged(warning) == warned && doc.project->name == "Edits",
          "Save As inside the project: paths unchanged, no warning");
    startPlay(doc);
    checkOk(doc.lua, 0, "project-relative paths load from a scene in a subfolder");
    stopPlay(doc);
    check(saveFile(doc, (other / "moved.scene").string(), error) && doc.scene.systems == systems &&
              countLogged(warning) == warned + 1 && doc.project && doc.project->name == "Other" &&
              contains(readText(other / "moved.scene"), "\"scripts/new_system_2.lua\""),
          "Save As into another project: warns, paths not rebased, project switches");
    check(saveFile(doc, (loose / "moved.scene").string(), error) && doc.scene.systems == systems &&
              countLogged(warning) == warned + 2 && !doc.project,
          "Save As into no project: warns, no project");
    doc.scene.systems.clear();
    check(saveFile(doc, (dir / "empty.scene").string(), error) && countLogged(warning) == warned + 2 && doc.project &&
              doc.project->name == "Edits",
          "no warning without systems; Save sets the project by walking up");
}

// kast.loadScene errors: raised at the calling line, nothing queued, the current scene keeps running.
void checkSceneSwitchErrors()
{
    const std::filesystem::path dir = projectTestDir("kast_switch_errors", "Errors");
    saveTestScene(dir / "target.scene", "Target", {});
    writeFile(dir / "broken.scene", R"({ "version": 4, )");
    writeFile(dir / "v3.scene", R"({"version": 3, "entities": [], "systems": []})");
    writeFile(dir / "scripts/errors.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
    assert(string.find(err, "errors.lua:%d+:"), "error '" .. tostring(err) .. "' has no file:line")
end
return { update = function(dt)
    assert(kast.currentScene() == "errors.scene", "currentScene before any switch")
    fails("outside the project", function() kast.loadScene("../outside.scene") end)
    fails("outside the project", function() kast.loadScene("scripts/../../target.scene") end)
    fails("must be relative to the project root", function() kast.loadScene("C:/target.scene") end)
    fails("must be relative to the project root", function() kast.loadScene("/target.scene") end)
    fails("path is empty", function() kast.loadScene("") end)
    fails("Cannot open scene", function() kast.loadScene("missing.scene") end)
    fails("Invalid scene", function() kast.loadScene("broken.scene") end)
    fails('unsupported "version" 3', function() kast.loadScene("v3.scene") end)
    assert(not pcall(kast.loadScene), "a missing argument raises")
end })");
    writeFile(dir / "scripts/raw.lua", "return { update = function(dt)\n    kast.loadScene(\"missing.scene\")\nend }");
    writeFile(dir / "scripts/after.lua", "return { update = function(dt) kast.log(\"swerr after\") end }");
    saveTestScene(dir / "errors.scene", "Errors", {"scripts/errors.lua", "scripts/raw.lua", "scripts/after.lua"});

    kast::Scene scene;
    std::string error;
    check(kast::loadScene((dir / "errors.scene").string(), scene, error), "load errors.scene");
    kast::LuaSystems lua;
    lua.start(scene, dir, "errors.scene", nullptr);
    const auto ran = countLogged("swerr after");
    check(!lua.tick(0.1f), "failed kast.loadScene calls queue no switch");
    checkOk(lua, 0, "kast.loadScene errors raise at the calling line (no project root escape, missing, invalid, v3)");
    check(!statusOf(lua, 1).ok && contains(statusOf(lua, 1).error, "raw.lua:2:") &&
              contains(statusOf(lua, 1).error, "Cannot open scene"),
          "an uncaught kast.loadScene error disables the calling system with file:line");
    checkOk(lua, 2, "other systems keep running");
    check(!lua.tick(0.1f) && countLogged("swerr after") == ran + 2 && named(scene, "Errors") != entt::null &&
              lua.scenePath() == "errors.scene",
          "the current scene keeps running");
    lua.stop();
}

// kast.loadScene on a Document: deferred to the end of the frame, last call wins, shutdown calls ignored, old
// shutdowns before new inits, panels/title follow, Stop restores the pre-Play scene after several switches.
void checkSceneSwitch()
{
    const std::filesystem::path dir = projectTestDir("kast_switch_check", "Switch");
    std::filesystem::create_directories(dir / "levels");
    saveTestScene(dir / "a.scene", "A", {"scripts/a.lua", "scripts/a2.lua"});
    saveTestScene(dir / "b.scene", "B", {"scripts/b.lua"});
    saveTestScene(dir / "levels/c.scene", "C", {"scripts/c.lua"});
    writeFile(dir / "scripts/a.lua", R"(return {
    init = function() kast.log("sw a init " .. tostring(kast.currentScene())) end,
    update = function(dt)
        kast.loadScene("levels/c.scene")
        kast.loadScene("./b.scene") -- last call wins
        kast.log("sw a update " .. kast.currentScene()) -- still a.scene: the switch waits for the end of the frame
    end,
    shutdown = function() kast.log("sw a shutdown"); kast.loadScene("levels/c.scene") end, -- ignored
})");
    writeFile(dir / "scripts/a2.lua", R"(return { update = function(dt)
    local n = 0
    for e in kast.query() do n = n + 1 end
    kast.log("sw a2 update " .. n)
end })");
    writeFile(dir / "scripts/b.lua", R"(return {
    init = function() kast.log("sw b init " .. tostring(kast.currentScene())); kast.loadScene("levels/c.scene") end,
    update = function(dt) kast.log("sw b update") end,
    shutdown = function() kast.log("sw b shutdown") end,
})");
    writeFile(dir / "scripts/c.lua", R"(return {
    init = function() kast.log("sw c init " .. tostring(kast.currentScene())) end,
    update = function(dt) end,
    shutdown = function() kast.log("sw c shutdown"); kast.loadScene("a.scene") end, -- ignored: Stop
})");

    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open a.scene in its project");
    const entt::entity a = named(doc.scene, "A");
    doc.scene.registry().get<kast::Name>(a).value = "A edited"; // unsaved: loadScene reads files, Stop restores this
    markDirty(doc);
    doc.selected = a;
    const kast::EntityId selectedId = doc.scene.registry().get<kast::PersistentId>(a).value;
    doc.camera.yaw = 77.0f;
    const std::string before = kast::sceneToJson(doc.scene).dump(2);
    const std::string path = doc.path;
    const std::filesystem::path root = doc.project->root;

    startPlay(doc);
    check(logged("sw a init a.scene") && windowTitle(doc) == "Kast Editor - Switch - a* (Playing)",
          "Play: currentScene is a.scene, title has no switched name yet");
    updatePlay(doc, 0.1f);
    check(logIndex("sw a update a.scene") < logIndex("sw a2 update 1") &&
              logIndex("sw a2 update 1") < logIndex("sw a shutdown") &&
              logIndex("sw a shutdown") < logIndex("sw b init b.scene") && logIndex("sw b init b.scene") != SIZE_MAX,
          "switch at the end of the frame: every update, then old shutdowns, then new inits");
    check(named(doc.scene, "B") != entt::null && named(doc.scene, "A edited") == entt::null && named(doc.scene, "A") == entt::null,
          "the last call wins and the scene is read from disk");
    check(!logged("sw c init"), "loadScene from a shutdown is ignored; from an init it waits for the next frame");
    check(doc.selected == entt::null, "a switch clears the selection");
    check((doc.scene.systems == std::vector<std::string>{"scripts/b.lua"}) && statusOf(doc.lua, 0).path == "scripts/b.lua" &&
              statusOf(doc.lua, 0).ok,
          "the Systems panel lists and reports the new scene's systems");
    check(windowTitle(doc) == "Kast Editor - Switch - a* (Playing: b)", "title shows the running scene");
    check(doc.path == path && doc.project && doc.project->root == root && doc.camera.yaw == 77.0f && doc.dirty,
          "a switch keeps path, project, dirty flag and editor camera");

    togglePause(doc);
    updatePlay(doc, 0.1f);
    check(windowTitle(doc) == "Kast Editor - Switch - a* (Paused: b)" && !logged("sw b update"), "paused: no tick, no switch");
    startPlay(doc);
    updatePlay(doc, 0.1f);
    check(logIndex("sw b update") < logIndex("sw b shutdown") && logIndex("sw b shutdown") < logIndex("sw c init levels/c.scene") &&
              logIndex("sw c init levels/c.scene") != SIZE_MAX,
          "a call from init is applied at the end of the next ticked frame");
    check(named(doc.scene, "C") != entt::null && doc.lua.scenePath() == "levels/c.scene" &&
              windowTitle(doc) == "Kast Editor - Switch - a* (Playing: c)",
          "second switch, into a subfolder");
    doc.selected = named(doc.scene, "C");
    check(doc.scene.registry().get<kast::PersistentId>(doc.selected).value == selectedId,
          "C shares A's id (ids are only unique per scene file)");
    const uint32_t generation = doc.sceneGeneration;

    stopPlay(doc);
    check(logged("sw c shutdown") && countLogged("sw a init") == 1, "Stop: shutdown's loadScene is ignored");
    check(kast::sceneToJson(doc.scene).dump(2) == before, "Stop after two switches restores the byte-identical pre-Play scene");
    check(doc.path == path && doc.project && doc.project->root == root && doc.dirty &&
              windowTitle(doc) == "Kast Editor - Switch - a*",
          "Stop restores path, project, dirty flag and the title");
    check(doc.selected == entt::null, "Stop after a switch clears the selection rather than select A by C's id");
    check(doc.sceneGeneration != generation, "Stop bumps sceneGeneration (panels drop stale edits)");

    // kast.currentScene: nil for an Untitled scene, the project-relative path otherwise.
    writeFile(dir / "scripts/cur.lua",
              R"(return { init = function() kast.log("sw cur " .. tostring(kast.currentScene())) end, update = function(dt) end })");
    resetToDefault(doc);
    doc.scene.systems = {"scripts/cur.lua"};
    startPlay(doc);
    stopPlay(doc);
    check(logged("sw cur nil"), "kast.currentScene() is nil for an Untitled scene");
    doc.path = (dir / "levels/d.scene").string();
    startPlay(doc);
    stopPlay(doc);
    check(logged("sw cur levels/d.scene"), "kast.currentScene() is the project-relative path");

    // The unsaved-changes prompt blocks file actions (Ctrl+N/O/S, the File menu) until it is answered.
    FileActions files(doc);
    doc.dirty = true;
    const bool idleBefore = files.idle();
    files.newScene();
    check(idleBefore && !files.idle() && doc.dirty, "FileActions is not idle while the unsaved-changes prompt is pending");
}

} // namespace

// --- Lua components ---

constexpr const char* kHealthDef = R"(return {
    name = "Health",
    fields = {
        { name = "current", type = "float",  default = 100, min = 0 },
        { name = "max",     type = "float",  default = 100, min = 1 },
        { name = "regen",   type = "bool",   default = true },
        { name = "lives",   type = "int",    default = 3, min = 0, max = 9 },
        { name = "title",   type = "string", default = "Hero" },
        { name = "tint",    type = "vec3",   color = true, default = {x = 1, y = 0, z = 0} },
        { name = "facing",  type = "quat" },
        { name = "faction", type = "enum",   values = {"neutral", "ally", "enemy"}, default = "neutral" },
        { name = "drops",   type = "array",  of = "string", default = {"gold", "potion"} },
    },
}
)";
constexpr const char* kHealthDefaults =
    R"({"current":100.0,"max":100.0,"regen":true,"lives":3,"title":"Hero","tint":[1.0,0.0,0.0],)"
    R"("facing":[0.0,0.0,0.0,1.0],"faction":"neutral","drops":["gold","potion"]})";

const kast::ComponentDefinitionStatus* statusFor(const std::string& path)
{
    for (const kast::ComponentDefinitionStatus& status : kast::componentDefinitionStatus())
        if (status.path == path)
            return &status;
    return nullptr;
}

// A temp project with components/health.lua.
std::filesystem::path componentProject(const char* name)
{
    const std::filesystem::path dir = projectTestDir(name, "Components");
    std::filesystem::create_directories(dir / kast::kComponentsDir);
    writeFile(dir / "components/health.lua", kHealthDef);
    return dir;
}

void checkLuaComponentDefinitions()
{
    const std::filesystem::path dir = componentProject("kast_components_defs");
    std::filesystem::create_directories(dir / "components/sub/deep");
    writeFile(dir / "components/sub/deep/tag.lua", R"(return { name = "Tag", displayName = "Tag!", fields = {} })");
    writeFile(dir / "components/bounded.lua", R"(return { name = "Bounded", fields = {
        { name = "n", type = "int", min = 2, max = 5 },
        { name = "f", type = "float", max = -1 },
        { name = "w", type = "array", of = "float", min = 0, max = 1, default = {0.5, 1} },
        { name = "e", type = "array", of = "enum", values = {"a", "b"}, default = {"b", "a"} },
    } })");
    // Sandbox: nothing outside base/table/string/math is reachable; globals don't leak between files.
    writeFile(dir / "components/sandboxed.lua", R"(
for _, n in ipairs({"io", "os", "ffi", "debug", "package", "require", "loadfile", "dofile", "load", "loadstring", "jit", "module"}) do
    assert(_G[n] == nil, n .. " is reachable")
end
assert(leaked == nil, "a global leaked from another definition file")
return { name = "Sandboxed", fields = {} })");
    writeFile(dir / "components/leaker.lua", R"(leaked = true
return { name = "Leaker", fields = {} })");
    writeFile(dir / "components/escape.lua", R"(return { name = "Escape", fields = {}, x = io.open("x") })");
    // World-server keys (docs/services/world/lua.html): accepted, so files can be shared.
    writeFile(dir / "components/shared.lua", R"(return { name = "Shared", persist = true, replicate = "aoi", fields = {} })");
    writeFile(dir / "components/shared_owner.lua", R"(return { name = "SharedOwner", persist = false, replicate = "owner", fields = {} })");

    // file, contents, text the error must mention (besides the path)
    const std::tuple<const char*, const char*, const char*> bad[] = {
        {"not_table.lua", "return 5", "must return exactly one table"},
        {"bad_persist.lua", R"(return { name = "BadPersist", persist = "yes", fields = {} })", "\"persist\" must be true or false"},
        {"bad_replicate.lua", R"(return { name = "BadReplicate", replicate = "all", fields = {} })",
         "\"replicate\" must be \"none\", \"owner\" or \"aoi\", got all"},
        {"bad_replicate_type.lua", R"(return { name = "BadReplicateType", replicate = 1, fields = {} })",
         "\"replicate\" must be \"none\", \"owner\" or \"aoi\", got 1"},
        {"two_values.lua", R"(return { name = "Two", fields = {} }, 2)", "must return exactly one table"},
        {"no_name.lua", "return { fields = {} }", "\"name\" must be a name"},
        {"bad_name.lua", R"(return { name = "9lives", fields = {} })", "\"name\" must be a name"},
        {"no_fields.lua", R"(return { name = "NoFields" })", "\"fields\" must be a list"},
        {"top_key.lua", R"(return { name = "TopKey", fields = {}, feilds = {} })", "unknown key feilds"},
        {"dup_field.lua", R"(return { name = "DupField", fields = { {name = "a", type = "int"}, {name = "a", type = "bool"} } })",
         "duplicate field name"},
        {"field_name.lua", R"(return { name = "FieldName", fields = { {name = "has space", type = "int"} } })", "\"name\" must be a name"},
        {"bad_type.lua", R"(return { name = "BadType", fields = { {name = "a", type = "flaot"} } })", "unknown \"type\" flaot"},
        {"field_key.lua", R"(return { name = "FieldKey", fields = { {name = "a", type = "int", deafult = 1} } })", "unknown key deafult"},
        {"no_values.lua", R"(return { name = "NoValues", fields = { {name = "a", type = "enum"} } })", "\"values\" must be a list"},
        {"empty_values.lua", R"(return { name = "EmptyValues", fields = { {name = "a", type = "enum", values = {}} } })",
         "\"values\" must not be empty"},
        {"dup_values.lua", R"(return { name = "DupValues", fields = { {name = "a", type = "enum", values = {"x", "x"}} } })",
         "duplicate value \"x\""},
        {"values_on_int.lua", R"(return { name = "ValuesOnInt", fields = { {name = "a", type = "int", values = {"x"}} } })",
         "only allowed on enum fields"},
        {"no_of.lua", R"(return { name = "NoOf", fields = { {name = "a", type = "array"} } })", "unknown \"of\""},
        {"nested.lua", R"(return { name = "Nested", fields = { {name = "a", type = "array", of = "array"} } })", "arrays can't nest"},
        {"of_on_int.lua", R"(return { name = "OfOnInt", fields = { {name = "a", type = "int", of = "int"} } })", "\"of\" is only allowed"},
        {"min_bool.lua", R"(return { name = "MinBool", fields = { {name = "a", type = "bool", min = 1} } })",
         "only allowed on float and int"},
        {"min_text.lua", R"(return { name = "MinText", fields = { {name = "a", type = "float", min = "low"} } })",
         "\"min\" must be a finite number"},
        {"min_max.lua", R"(return { name = "MinMax", fields = { {name = "a", type = "float", min = 2, max = 1} } })",
         "\"min\" must not be greater"},
        {"color_float.lua", R"(return { name = "ColorFloat", fields = { {name = "a", type = "float", color = true} } })",
         "\"color\" must be true or false, on vec3"},
        {"default_range.lua", R"(return { name = "DefaultRange", fields = { {name = "a", type = "float", min = 0, default = -1} } })",
         "must be >= 0"},
        {"default_type.lua", R"(return { name = "DefaultType", fields = { {name = "a", type = "int", default = "x"} } })",
         "must be an integer"},
        {"default_enum.lua", R"(return { name = "DefaultEnum", fields = { {name = "a", type = "enum", values = {"x"}, default = "y"} } })",
         "unknown value \"y\""},
        {"default_array.lua", R"(return { name = "DefaultArray", fields = { {name = "a", type = "array", of = "int", default = {1, "2"}} } })",
         "element 2 must be an integer"},
        {"runtime.lua", "local t = nil\nreturn t.x", "components/runtime.lua:2:"},
        {"syntax.lua", "return {\n name = ", "components/syntax.lua:2:"},
        {"builtin.lua", R"(return { name = "Transform", fields = {} })", "taken by a built-in component"},
        {"structural.lua", R"(return { name = "Name", fields = {} })", "taken by a built-in component"},
    };
    for (const auto& [file, text, mention] : bad)
        writeFile(dir / "components" / file, text);
    writeFile(dir / "components/dup_a.lua", R"(return { name = "Dup", fields = {} })");
    writeFile(dir / "components/sub/dup_b.lua", R"(return { name = "Dup", fields = {} })");
    writeFile(dir / "components/notes.txt", "not a definition");

    kast::activateComponentDefinitions(dir, nullptr);
    const kast::ComponentDefinitionStatus* health = statusFor("components/health.lua");
    check(health && health->ok && health->name == "Health", "health.lua discovered and OK");
    const kast::ComponentDefinitionStatus* tag = statusFor("components/sub/deep/tag.lua");
    check(tag && tag->ok, "definitions are discovered recursively (components/sub/deep/tag.lua)");
    check(kast::findAnyComponentType("Tag") && std::string(kast::findAnyComponentType("Tag").displayName()) == "Tag!",
          "displayName");
    check(statusFor("components/notes.txt") == nullptr, "only .lua files are definitions");
    check(statusFor("components/shared.lua") && statusFor("components/shared.lua")->ok &&
              statusFor("components/shared_owner.lua") && statusFor("components/shared_owner.lua")->ok,
          "persist / replicate (world-server keys) are accepted");
    const kast::ComponentDefinitionStatus* sandboxed = statusFor("components/sandboxed.lua");
    check(sandboxed && sandboxed->ok, "sandbox: io/os/ffi/debug/package/require/loadfile/dofile/load unreachable");
    if (sandboxed && !sandboxed->ok)
        kast::log::error(sandboxed->error);
    const kast::ComponentDefinitionStatus* escape = statusFor("components/escape.lua");
    check(escape && !escape->ok && contains(escape->error, "components/escape.lua:1:"), "using io fails the definition");
    for (const auto& [file, text, mention] : bad) {
        const std::string path = std::string("components/") + file;
        const kast::ComponentDefinitionStatus* status = statusFor(path);
        const bool ok = status && !status->ok && contains(status->error, path) && contains(status->error, mention);
        check(ok, file);
        if (status && !ok)
            kast::log::error(std::format("  {}: {}", file, status->error));
    }
    const kast::ComponentDefinitionStatus* dupA = statusFor("components/dup_a.lua");
    const kast::ComponentDefinitionStatus* dupB = statusFor("components/sub/dup_b.lua");
    check(dupA && dupB && !dupA->ok && !dupB->ok && contains(dupA->error, "components/sub/dup_b.lua") &&
              contains(dupB->error, "components/dup_a.lua") && !kast::findAnyComponentType("Dup"),
          "duplicate Lua names: both rejected, each naming the other file");
    check(logged("Lua component rejected: components/bad_type.lua"), "rejected definitions are logged");
    check(std::string(kast::findAnyComponentType("Transform").name()) == "Transform" &&
              !kast::findAnyComponentType("Transform").lua(),
          "a clashing Lua definition doesn't replace the built-in");

    // Field descriptions and canonical defaults.
    const kast::ComponentType type = kast::findAnyComponentType("Health");
    check(type && type.lua() && type.removable() && type.fields().size() == 9, "Health type");
    check(kast::defaultLuaComponent(*type.lua()).dump() == kHealthDefaults, "declared and implied (identity quat) defaults");
    const kast::ComponentType bounded = kast::findAnyComponentType("Bounded");
    check(bounded && kast::defaultLuaComponent(*bounded.lua()).dump() ==
                         R"({"n":2,"f":-1.0,"w":[0.5,1.0],"e":["b","a"]})",
          "omitted defaults are the zero value moved into min/max; arrays of float and enum");
    const std::vector<kast::ComponentType> all = kast::allComponentTypes();
    check(all.size() >= 5 && std::string(all[0].name()) == "Transform" &&
              std::string(all[all.size() - 1].name()) == "Tag",
          "allComponentTypes: built-ins first, then Lua types sorted by name");
    check(kast::componentTypes().size() == all.size() - kast::luaComponentTypes().size(),
          "componentTypes() still lists built-ins only");

    kast::activateComponentDefinitions(std::nullopt, nullptr);
    check(!kast::findAnyComponentType("Health") && kast::componentDefinitionStatus().empty(),
          "no project: no Lua components");
}

void checkLuaComponentRegistry()
{
    const std::filesystem::path dir = componentProject("kast_components_registry");
    kast::activateComponentDefinitions(dir, nullptr);
    const kast::ComponentType health = kast::findAnyComponentType("Health");

    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity e = scene.create("Hero");
    check(!kast::hasComponent(registry, e, health), "has (absent)");
    kast::addComponent(registry, e, health);
    check(kast::hasComponent(registry, e, health) && kast::componentToJson(registry, e, health).dump() == kHealthDefaults,
          "add gives the declared defaults");

    std::string error;
    nlohmann::ordered_json json = kast::componentToJson(registry, e, health);
    json["current"] = 42.5;
    json["drops"] = {"sword"};
    check(kast::setComponentJson(registry, e, health, json, error) &&
              kast::componentToJson(registry, e, health)["current"] == 42.5,
          "setComponentJson writes");
    const std::string valid = kast::componentToJson(registry, e, health).dump();
    const std::pair<const char*, const char*> bad[] = {
        {"current", R"(-1)"}, {"lives", R"(10)"}, {"lives", R"(1.5)"}, {"faction", R"("boss")"},
        {"drops", R"("sword")"}, {"drops", R"(["a", 1])"}, {"facing", R"([0,0,0,0])"}, {"tint", R"([1,2])"},
    };
    for (const auto& [field, value] : bad) {
        nlohmann::ordered_json broken = nlohmann::ordered_json::parse(valid);
        broken[field] = nlohmann::ordered_json::parse(value);
        check(!kast::setComponentJson(registry, e, health, broken, error) && contains(error, field) &&
                  kast::componentToJson(registry, e, health).dump() == valid,
              "strict set rejects a bad value and leaves the component unchanged");
    }
    nlohmann::ordered_json scaled = nlohmann::ordered_json::parse(valid);
    scaled["facing"] = {0, 0, 2, 0};
    check(kast::setComponentJson(registry, e, health, scaled, error) &&
              kast::componentToJson(registry, e, health)["facing"].dump() == "[0.0,0.0,1.0,0.0]",
          "Lua quat field set non-unit is stored normalized");
    check(kast::setComponentJson(registry, e, health, nlohmann::ordered_json::parse(valid), error), "restore Health");
    nlohmann::ordered_json extra = nlohmann::ordered_json::parse(valid);
    extra["armor"] = 1;
    check(!kast::setComponentJson(registry, e, health, extra, error) && contains(error, "unknown field \"armor\""),
          "strict set rejects unknown fields");
    nlohmann::ordered_json missing = nlohmann::ordered_json::parse(valid);
    missing.erase("title");
    check(!kast::setComponentJson(registry, e, health, missing, error) && contains(error, "missing field \"title\""),
          "strict set rejects missing fields");

    // Duplicate copies Lua components and preserved data.
    registry.emplace<kast::PreservedComponents>(e).byName["Ghost"] = nlohmann::ordered_json::parse(R"({"b":1,"a":[2]})");
    const entt::entity copy = scene.duplicate(e);
    check(kast::hasComponent(registry, copy, health) && kast::componentToJson(registry, copy, health).dump() == valid,
          "Duplicate copies Lua components");
    check(registry.all_of<kast::PreservedComponents>(copy) &&
              registry.get<kast::PreservedComponents>(copy).byName.at("Ghost").dump() == R"({"b":1,"a":[2]})",
          "Duplicate copies preserved (Missing) data");
    kast::removeComponent(registry, copy, health);
    check(!kast::hasComponent(registry, copy, health) && kast::hasComponent(registry, e, health), "remove");

    // Scene JSON: same shape as built-ins, after them; preserved last; byte-identical round trip.
    const nlohmann::ordered_json saved = kast::sceneToJson(scene);
    const nlohmann::ordered_json& components = saved["entities"][0]["components"];
    std::vector<std::string> keys;
    for (const auto& [key, value] : components.items())
        keys.push_back(key);
    check(keys == std::vector<std::string>{"Transform", "Health", "Ghost"}, "save order: built-ins, Lua, preserved");
    check(components["Health"].dump() == valid, "Lua component saved as its fields");
    const std::string path = (dir / "round.scene").string();
    const std::string path2 = (dir / "round2.scene").string();
    kast::Scene loaded;
    check(kast::saveScene(scene, path, error) && kast::loadScene(path, loaded, error) &&
              kast::saveScene(loaded, path2, error) && readText(path) == readText(path2),
          "save -> load -> save is byte-identical");
    kast::Scene restored;
    check(kast::sceneFromJson(saved, restored, error) && kast::sceneToJson(restored).dump() == saved.dump(),
          "in-memory snapshot restores Lua and preserved components exactly");

    // Play -> Stop on a Document restores them exactly.
    Document doc;
    check(openFile(doc, path, error) && !doc.dirty, "open a scene with Lua components (not dirty)");
    const std::string before = kast::sceneToJson(doc.scene).dump();
    startPlay(doc);
    const entt::entity hero = named(doc.scene, "Hero");
    nlohmann::ordered_json changed = kast::componentToJson(doc.scene.registry(), hero, health);
    changed["current"] = 1.0;
    check(kast::setComponentJson(doc.scene.registry(), hero, health, changed, error), "edit during Play");
    kast::removePreservedComponent(doc.scene.registry(), hero, "Ghost");
    stopPlay(doc);
    check(kast::sceneToJson(doc.scene).dump() == before && !doc.dirty, "Stop restores Lua and preserved data exactly");

    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

void checkLuaComponentApi()
{
    const std::filesystem::path dir = componentProject("kast_components_api");
    kast::activateComponentDefinitions(dir, nullptr);
    writeFile(dir / "scripts/health_api.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
    assert(string.find(err, "health_api.lua:%d+:"), "error '" .. tostring(err) .. "' has no file:line")
end
local function snapshot(e)
    local h = kast.get(e, "Health")
    return table.concat({h.current, h.max, tostring(h.regen), h.lives, h.title, h.tint.x, h.tint.y, h.tint.z,
                         h.facing.x, h.facing.w, h.faction, table.concat(h.drops, ",")}, "|")
end
local done = false
return { update = function(dt)
    if done then return end
    done = true
    local found = {}
    for e in kast.query("Transform", "Health") do found[#found + 1] = e end
    assert(#found == 1, "query by a Lua component")
    local e = found[1]
    assert(kast.has(e, "Health"), "has")
    local h = kast.get(e, "Health")
    assert(h.current == 100 and h.max == 100 and h.regen == true and h.lives == 3 and h.title == "Hero", "get scalars")
    assert(h.tint.x == 1 and h.tint.y == 0 and h.facing.w == 1 and h.faction == "neutral", "get vec3, quat, enum")
    assert(#h.drops == 2 and h.drops[1] == "gold" and h.drops[2] == "potion", "arrays are 1-based sequences")

    kast.set(e, "Health", {current = 50, drops = {"a", "b", "c"}, faction = "enemy", tint = {x = 0, y = 1, z = 0}})
    h = kast.get(e, "Health")
    assert(h.current == 50 and h.max == 100 and h.title == "Hero", "set is partial")
    assert(#h.drops == 3 and h.drops[3] == "c" and h.faction == "enemy" and h.tint.y == 1, "set arrays, enums, vec3")
    kast.set(e, "Health", {drops = {}})
    assert(#kast.get(e, "Health").drops == 0, "set an empty array")

    local before = snapshot(e)
    fails('"current" must be a number', function() kast.set(e, "Health", {lives = 4, current = "full"}) end)
    fails('unknown field "armor"', function() kast.set(e, "Health", {current = 1, armor = 2}) end)
    fails('unknown value "boss"', function() kast.set(e, "Health", {faction = "boss"}) end)
    fails('"current" must be >= 0', function() kast.set(e, "Health", {current = -1}) end)
    fails('"lives" must be <= 9', function() kast.set(e, "Health", {lives = 10}) end)
    fails('"lives" must be an integer', function() kast.set(e, "Health", {lives = 1.5}) end)
    fails('"regen" must be true or false', function() kast.set(e, "Health", {regen = 1}) end)
    fails('"drops" must be a sequence', function() kast.set(e, "Health", {drops = {x = 1}}) end)
    fails('"drops" element 2 must be a string', function() kast.set(e, "Health", {drops = {"ok", 5}}) end)
    fails('"tint" must be a table', function() kast.set(e, "Health", {tint = {x = 1}}) end)
    fails('"facing" must be a nonzero quaternion', function() kast.set(e, "Health", {facing = {x = 0, y = 0, z = 0, w = 0}}) end)
    assert(snapshot(e) == before, "failed sets leave Health unchanged")

    kast.remove(e, "Health")
    assert(not kast.has(e, "Health"), "remove")
    fails('has no "Health"', function() kast.get(e, "Health") end)
    kast.add(e, "Health")
    assert(kast.get(e, "Health").current == 100, "add uses the declared defaults")
    local spawned = kast.create("Spawned")
    kast.add(spawned, "Health")
    local n = 0
    for x in kast.query("Health") do n = n + 1 end
    assert(n == 2, "query sees added Lua components")
    fails('unknown component "Ghost"', function() kast.has(e, "Ghost") end)
    fails('unknown component "Ghost"', function() kast.get(e, "Ghost") end)
end })");

    kast::Scene scene;
    const entt::entity hero = scene.create("Hero");
    kast::addComponent(scene.registry(), hero, kast::findAnyComponentType("Health"));
    scene.registry().emplace<kast::PreservedComponents>(hero).byName["Ghost"] = nlohmann::ordered_json::object();
    scene.systems = {"scripts/health_api.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.* on a Lua component: asserts in Lua pass");
    check(named(scene, "Spawned") != entt::null &&
              kast::hasComponent(scene.registry(), named(scene, "Spawned"), kast::findAnyComponentType("Health")),
          "kast.add on a new entity");
    lua.stop();
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

constexpr const char* kSceneHead = R"({"version": 5, "entities": [{"id": 1, "parent": null, "name": "Drifty", "components": {)"
                                   R"("Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]})";

void checkLuaComponentDrift()
{
    const std::filesystem::path dir = componentProject("kast_components_drift");
    const std::string drifted = std::string(kSceneHead) +
        R"(, "Health": {"max": 100, "regen": "yes", "lives": 12, "title": "Hero", "tint": [1,0,0], "facing": [0,0,0,1],)"
        R"( "faction": "boss", "drops": ["a", 3], "armor": 5}}}]})";
    writeFile(dir / "drift.scene", drifted);
    writeFile(dir / "match.scene", std::string(kSceneHead) + R"(, "Health": )" + kHealthDefaults + "}}]}");

    Document doc;
    std::string error;
    const ptrdiff_t warningsBefore = countLogged("Entity 'Drifty' (id 1): Health:");
    check(openFile(doc, (dir / "drift.scene").string(), error), "a drifted scene still loads");
    const ptrdiff_t warnings = countLogged("Entity 'Drifty' (id 1): Health:") - warningsBefore;
    check(warnings == 6, "one warning per drift fix (missing, unknown, mistyped, out of range, bad enum, bad element)");
    check(logged("unknown field \"armor\" dropped") && logged("missing field \"current\" set to default 100.0") &&
              logged("\"regen\" \"yes\" must be true or false; reset to default true") &&
              logged("\"lives\" 12 must be <= 9; reset to default 3") &&
              logged("\"faction\" \"boss\" unknown value \"boss\"") && logged("\"drops\" [\"a\",3] element 2 must be a string"),
          "warnings name the field and the fix");
    const kast::ComponentType health = kast::findAnyComponentType("Health");
    check(kast::componentToJson(doc.scene.registry(), named(doc.scene, "Drifty"), health).dump() == kHealthDefaults,
          "drift fixes: defaults for missing/invalid values, unknown fields dropped");
    check(doc.dirty, "a drifted scene is dirty after load");
    check(openFile(doc, (dir / "match.scene").string(), error) && !doc.dirty, "a matching scene is not dirty after load");

    // Built-in components stay strict.
    writeFile(dir / "strict.scene", std::string(kSceneHead) + R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": "fast"}}}]})");
    check(!openFile(doc, (dir / "strict.scene").string(), error) && contains(error, "degreesPerSecond"),
          "built-in components keep the strict load rules");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

void checkMissingComponents()
{
    const std::filesystem::path dir = projectTestDir("kast_components_missing", "Missing");
    const char* ghost = R"({"b": 1, "a": [2, {"z": true, "y": null}]})";
    writeFile(dir / "missing.scene", std::string(kSceneHead) + R"(, "Teleporter": )" + ghost +
                                         R"(, "Health": {"current": 5, "bogus": 1}}}]})");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "missing.scene").string(), error), "a scene with unknown components loads");
    check(logged("Entity 'Drifty' (id 1): unknown component \"Teleporter\"") &&
              logged("Entity 'Drifty' (id 1): unknown component \"Health\""),
          "each unknown component is a warning");
    check(!doc.dirty, "preserving data doesn't dirty the scene");
    const entt::entity e = named(doc.scene, "Drifty");
    const auto* preserved = doc.scene.registry().try_get<kast::PreservedComponents>(e);
    check(preserved && preserved->byName.size() == 2 &&
              preserved->byName.at("Teleporter").dump() == nlohmann::ordered_json::parse(ghost).dump(),
          "unknown component JSON kept verbatim (key order too)");
    check(saveFile(doc, (dir / "saved.scene").string(), error), "save");
    const nlohmann::ordered_json saved = nlohmann::ordered_json::parse(readText(dir / "saved.scene"));
    check(saved["entities"][0]["components"]["Teleporter"].dump() == nlohmann::ordered_json::parse(ghost).dump() &&
              saved["entities"][0]["components"]["Health"].dump() == R"({"current":5,"bogus":1})",
          "Save writes preserved JSON back unchanged");

    // kast.* can't reach it.
    writeFile(dir / "scripts/ghost.lua", R"(return { update = function(dt)
    local ok, err = pcall(function() return kast.has(1, "Teleporter") end)
    assert(not ok and string.find(err, 'unknown component "Teleporter"', 1, true), "Teleporter must be unknown")
    ok, err = pcall(function() return kast.get(1, "Health") end)
    assert(not ok and string.find(err, 'unknown component "Health"', 1, true), "Health must be unknown")
end })");
    kast::LuaSystems lua;
    doc.scene.systems = {"scripts/ghost.lua"};
    lua.start(doc.scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.* raises unknown component on preserved data");
    lua.stop();
    doc.scene.systems.clear();

    // The definition appears later: preserved Health is converted with the drift rules; Teleporter stays preserved.
    std::filesystem::create_directories(dir / kast::kComponentsDir);
    writeFile(dir / "components/health.lua", kHealthDef);
    doc.dirty = false;
    refreshComponents(doc);
    const kast::ComponentType health = kast::findAnyComponentType("Health");
    check(health && kast::hasComponent(doc.scene.registry(), e, health) &&
              kast::componentToJson(doc.scene.registry(), e, health)["current"] == 5.0,
          "a new definition converts preserved data");
    preserved = doc.scene.registry().try_get<kast::PreservedComponents>(e);
    check(preserved && preserved->byName.size() == 1 && preserved->byName.contains("Teleporter"),
          "converted data leaves Missing; other unknown components stay");
    check(doc.dirty && logged("Entity 'Drifty' (id 1): Health: unknown field \"bogus\" dropped"),
          "conversion applies the drift rules (warning + dirty)");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

void checkComponentHotReload()
{
    const std::filesystem::path dir = componentProject("kast_components_reload");
    writeFile(dir / "hero.scene", std::string(kSceneHead) + R"(, "Health": )" + kHealthDefaults + "}}]}");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "hero.scene").string(), error) && !doc.dirty, "open");
    const entt::entity e = named(doc.scene, "Drifty");
    const auto healthJson = [&] {
        return kast::componentToJson(doc.scene.registry(), e, kast::findAnyComponentType("Health"));
    };
    refreshComponents(doc);
    check(!doc.dirty, "no file change: nothing reloads");

    // Modify: rename a field, add one. Instances migrate.
    std::string modified = kHealthDef;
    modified.replace(modified.find("\"current\""), 9, "\"hp\"");
    modified.replace(modified.find("    },\n}"), 0, "        { name = \"armor\", type = \"int\", default = 7 },\n");
    rewrite(dir / "components/health.lua", modified);
    refreshComponents(doc);
    check(healthJson().contains("hp") && !healthJson().contains("current") && healthJson()["armor"] == 7 && doc.dirty,
          "modified definition: instances migrate (default for new fields, unknown dropped), dirty");
    check(logged("Reloaded Lua component definitions"), "reload is logged");

    // Add a file.
    writeFile(dir / "components/mana.lua", R"(return { name = "Mana", fields = { {name = "points", type = "int"} } })");
    refreshComponents(doc);
    check(kast::findAnyComponentType("Mana") && statusFor("components/mana.lua") && statusFor("components/mana.lua")->ok,
          "added definition file is picked up");

    // Delete: instances become preserved, data intact. Restore: converted back.
    const std::string data = healthJson().dump();
    doc.dirty = false;
    std::filesystem::remove(dir / "components/health.lua");
    refreshComponents(doc);
    const auto* preserved = doc.scene.registry().try_get<kast::PreservedComponents>(e);
    check(!kast::findAnyComponentType("Health") && preserved && preserved->byName.at("Health").dump() == data && !doc.dirty,
          "deleted definition: data kept as Missing, not dirty");
    writeFile(dir / "components/health.lua", modified);
    refreshComponents(doc);
    check(kast::findAnyComponentType("Health") && healthJson().dump() == data &&
              !doc.scene.registry().all_of<kast::PreservedComponents>(e) && !doc.dirty,
          "restored definition converts it back unchanged");

    // A broken definition also preserves data.
    rewrite(dir / "components/health.lua", "return {");
    refreshComponents(doc);
    check(!kast::findAnyComponentType("Health") && doc.scene.registry().all_of<kast::PreservedComponents>(e) &&
              !statusFor("components/health.lua")->ok,
          "broken definition: data kept as Missing, status shows the error");
    rewrite(dir / "components/health.lua", modified);
    refreshComponents(doc);

    // Play freezes definitions; pending changes apply after Stop, to the restored scene.
    startPlay(doc);
    rewrite(dir / "components/health.lua", kHealthDef); // back to "current"
    refreshComponents(doc);
    check(healthJson().contains("hp") && kast::findAnyComponentType("Health").fields()[0].name == "hp",
          "during Play nothing changes");
    nlohmann::ordered_json playEdit = healthJson();
    playEdit["armor"] = 99;
    check(kast::setComponentJson(doc.scene.registry(), e, kast::findAnyComponentType("Health"), playEdit, error),
          "play-mode edit");
    stopPlay(doc);
    check(healthJson()["armor"] == 7, "Stop restores the pre-Play data under the frozen definition");
    const entt::entity restored = named(doc.scene, "Drifty");
    refreshComponents(doc);
    const nlohmann::ordered_json after =
        kast::componentToJson(doc.scene.registry(), restored, kast::findAnyComponentType("Health"));
    check(after.contains("current") && !after.contains("hp") && !after.contains("armor"),
          "after Stop the pending definition change applies to the restored scene");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

void checkComponentTemplate()
{
    const std::filesystem::path dir = projectTestDir("kast_components_template", "Template");
    std::string typeName;
    check(newComponentFile(dir, typeName) == dir / "components/new_component.lua" && typeName == "NewComponent",
          "first template file name");
    Document doc;
    std::string error;
    resetToDefault(doc);
    check(!newComponentFromTemplate(doc, error) && contains(error, "project"), "no project: New from Template refused");
    check(openProject(doc, dir / kast::kProjectFileName, error), "open project");
    check(newComponentFromTemplate(doc, error) && std::filesystem::exists(dir / "components/new_component.lua"),
          "New from Template writes components/new_component.lua");
    check(kast::findAnyComponentType("NewComponent") && statusFor("components/new_component.lua")->ok,
          "the template is a valid definition and is loaded at once");
    check(newComponentFile(dir, typeName) == dir / "components/new_component_2.lua" && typeName == "NewComponent2",
          "next file is numbered");
    check(newComponentFromTemplate(doc, error) && kast::findAnyComponentType("NewComponent2") &&
              statusFor("components/new_component_2.lua")->ok,
          "numbered template has its own type name, so both stay valid");
    startPlay(doc);
    check(!canEditComponents(doc) && !newComponentFromTemplate(doc, error) &&
              !std::filesystem::exists(dir / "components/new_component_3.lua"),
          "disabled during Play");
    stopPlay(doc);
    check(canEditComponents(doc), "enabled again after Stop");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

std::string readBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void checkNewLuaComponent()
{
    for (const auto& [name, stem] : {std::pair{"PlayerHealth", "player_health"}, {"HTTPServer", "http_server"},
                                     {"HTTPServer2D", "http_server2_d"}, {"Health2", "health2"}, {"health", "health"},
                                     {"My_Thing", "my_thing"}})
        check(componentFileStem(name) == stem, std::format("componentFileStem({}) == {}", name, stem).c_str());

    const std::filesystem::path dir = componentProject("kast_new_lua_component"); // defines Health
    writeFile(dir / "components/broken.lua", R"(return { name = "Broken", fields = { { name = "x", type = "nope" } } })");
    const std::string kept = "-- not a definition; must survive\n";
    writeFile(dir / "components/Odd_Case.lua", kept);
    const auto luaFiles = [&] {
        size_t n = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir / "components"))
            n += entry.path().extension() == ".lua";
        return n;
    };

    Document doc;
    std::string error;
    resetToDefault(doc);
    entt::entity entity = createObject(doc, ObjectKind::Empty, entt::null);
    check(newComponentNameError(doc, "PlayerHealth") == "Open a scene inside a project" &&
              !newLuaComponent(doc, entity, "PlayerHealth", error) && contains(error, "project"),
          "no project: New Lua Component refused");
    check(openProject(doc, dir / kast::kProjectFileName, error), "open project");
    entity = createObject(doc, ObjectKind::Empty, entt::null);
    doc.dirty = false;
    check(statusFor("components/broken.lua") && !statusFor("components/broken.lua")->ok, "broken definition listed");

    const std::pair<const char*, const char*> invalid[] = {
        {"", "Enter a name"},
        {"   ", "Enter a name"},
        {"2Fast", "letters, digits"},
        {"Player Health", "letters, digits"},
        {"Bad-Name", "letters, digits"},
        {"Transform", "built-in"},
        {"Name", "built-in"},
        {"Health", "already exists"},
        {"Broken", "already exists"},
        {"OddCase", "components/Odd_Case.lua already exists"},
        {"odd_case", "components/Odd_Case.lua already exists"},
    };
    for (const auto& [name, part] : invalid) {
        const size_t before = luaFiles();
        const bool refused = contains(newComponentNameError(doc, name), part) &&
                             !newLuaComponent(doc, entity, name, error) && contains(error, part);
        check(refused && luaFiles() == before && !doc.dirty,
              std::format("New Lua Component '{}' refused ({}), nothing written", name, part).c_str());
    }
    check(readBytes(dir / "components/Odd_Case.lua") == kept, "existing different-case file not overwritten");
    check(!newLuaComponent(doc, entt::null, "PlayerHealth", error) && error == "Select an entity" &&
              !std::filesystem::exists(dir / "components/player_health.lua"),
          "no entity: refused, nothing written");

    check(newComponentNameError(doc, " PlayerHealth ").empty(), "valid name (trimmed)");
    check(newLuaComponent(doc, entity, " PlayerHealth ", error), "New Lua Component PlayerHealth created");
    const std::string text = readBytes(dir / "components/player_health.lua");
    const kast::ComponentType type = kast::findAnyComponentType("PlayerHealth");
    check(contains(text, R"(name = "PlayerHealth",)") && contains(text, "speed"), "file is the template with the name");
    check(type.lua() && std::ranges::any_of(kast::luaComponentTypes(), [](const auto& d) { return d->name == "PlayerHealth"; }),
          "type loaded at once");
    check(kast::hasComponent(doc.scene.registry(), entity, type) && doc.dirty, "added to the entity, scene dirty");

    doc.dirty = false;
    const entt::entity other = createObject(doc, ObjectKind::Empty, entt::null);
    doc.dirty = false;
    check(!newLuaComponent(doc, other, "PlayerHealth", error) && contains(error, "already exists") &&
              readBytes(dir / "components/player_health.lua") == text && !doc.dirty,
          "second create of the same name refused, file byte-identical");

    startPlay(doc);
    check(newComponentNameError(doc, "Other") == "Stop Play to add components" &&
              !newLuaComponent(doc, other, "Other", error) && !std::filesystem::exists(dir / "components/other.lua"),
          "Play: refused, nothing written");
    stopPlay(doc);
    check(newLuaComponent(doc, other, "Other", error), "enabled again after Stop");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

// --- game.pak and Build ---

void checkPak()
{
    const std::filesystem::path dir = looseTestDir("kast_pak_check");
    std::filesystem::create_directories(dir / "src/scripts/ai");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"project.kast", "{}"},
        {"scripts/a.lua", "return 1"},
        {"scripts/ai/b.lua", std::string("bin\0ary\xff", 9)},
        {"empty.txt", ""},
        {"scriptsx.lua", "not under scripts/"},
    };
    std::vector<std::string> paths;
    for (const auto& [path, bytes] : files) {
        writeFile(dir / "src" / path, bytes);
        paths.push_back(path);
    }
    std::string error;
    check(kast::writePak(dir / "src", paths, dir / "game.pak", error), "write a pak");
    kast::Pak pak;
    check(pak.open(dir / "game.pak", error), "open the written pak");
    for (const auto& [path, bytes] : files)
        check(pak.find(path) == std::optional<std::string_view>(bytes), "every pak entry reads back byte-identical");
    check(!pak.find("missing.lua") && !pak.find("scripts"), "unknown paths and directories are not entries");
    check(pak.list("scripts") == std::vector<std::string>{"scripts/a.lua", "scripts/ai/b.lua"},
          "list a pak directory: everything under it, sorted, not a sibling sharing its prefix");
    check(pak.list("").size() == files.size(), "list the whole pak");
    check(!kast::writePak(dir / "src", {"nope.lua"}, dir / "bad.pak", error) && contains(error, "nope.lua"),
          "writing a pak with a missing file fails naming it");

    const std::string good = readText(dir / "game.pak");
    const auto rejected = [&](std::string bytes, const char* expected, const char* what) {
        kast::Pak corrupt;
        std::string reason;
        const bool ok = corrupt.parse(std::move(bytes), reason);
        check(!ok && contains(reason, expected), what);
        if (ok || !contains(reason, expected))
            kast::log::error(std::format("  got: {}", reason));
    };
    rejected("", "bad magic", "an empty pak is rejected");
    rejected("ZPAK" + good.substr(4), "bad magic", "a pak with bad magic is rejected");
    std::string version = good;
    version[4] = 2;
    rejected(version, "unsupported pak version 2", "a pak with an unknown version is rejected");
    rejected(good.substr(0, 10), "truncated header", "a pak cut inside its header is rejected");
    rejected(good.substr(0, 30), "truncated index", "a pak cut inside its index is rejected");
    rejected(good.substr(0, good.size() - 1), "past the end", "a pak missing its last data byte is rejected");
    // First entry: u32 path length at 12, the path, then its u64 offset and u64 size.
    const size_t firstOffset = 16 + files[0].first.size();
    std::string offset = good;
    const uint64_t past = good.size() + 100;
    std::memcpy(offset.data() + firstOffset, &past, sizeof(past));
    rejected(offset, "past the end", "an entry offset past the end is rejected");
    std::string huge = good;
    const uint64_t wrap = ~uint64_t{0};
    std::memcpy(huge.data() + firstOffset + 8, &wrap, sizeof(wrap));
    rejected(huge, "past the end", "an entry size that would overflow is rejected");
    kast::Pak missing;
    check(!missing.open(dir / "absent.pak", error) && contains(error, "Cannot open"), "a missing pak is an error");

    // Exact on-disk layout: header, index (path, offset, size per entry), then the data in index order.
    std::filesystem::create_directories(dir / "layout/b");
    writeFile(dir / "layout/a.txt", "hi");
    writeFile(dir / "layout/b/c.lua", "");
    std::string expected = "KPAK";
    const auto put = [&](auto value) { expected.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    put(kast::kPakVersion);
    put(uint32_t{2});
    put(uint32_t{5});
    expected += "a.txt";
    put(uint64_t{64});
    put(uint64_t{2});
    put(uint32_t{7});
    expected += "b/c.lua";
    put(uint64_t{66});
    put(uint64_t{0});
    expected += "hi";
    check(kast::writePak(dir / "layout", {"a.txt", "b/c.lua"}, dir / "layout.pak", error) &&
              readText(dir / "layout.pak") == expected && !std::filesystem::exists(dir / "layout.pak.tmp"),
          "pak bytes match the documented layout");
    check(!kast::writePak(dir / "layout", {"a.txt", "gone.lua"}, dir / "layout.pak", error) && contains(error, "gone.lua") &&
              readText(dir / "layout.pak") == expected && !std::filesystem::exists(dir / "layout.pak.tmp"),
          "a pak write that fails mid-stream keeps the previous pak");
}

void checkFileWrites()
{
    const std::filesystem::path dir = looseTestDir("kast_file_writes");
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "data.txt";
    const std::filesystem::path temp = dir / "data.txt.tmp";
    check(kast::writeFileAtomic(file, "first") && readText(file) == "first", "atomic write creates a file");
    check(kast::writeFileAtomic(file, "second") && readText(file) == "second", "atomic write replaces a file");
    check(!std::filesystem::exists(temp), "atomic write leaves no temp file behind");

    std::filesystem::create_directories(temp); // temp file can't be created
    check(!kast::writeFileAtomic(file, "third") && readText(file) == "second", "failed temp write keeps the original");
    std::filesystem::remove(temp);
    std::filesystem::create_directories(dir / "folder"); // rename over a directory fails
    check(!kast::writeFileAtomic(dir / "folder", "x") && std::filesystem::is_directory(dir / "folder") &&
              !std::filesystem::exists(dir / "folder.tmp"),
          "failed rename cleans up its temp file");

    // saveScene goes through the same path: a failed save keeps the previous scene file.
    kast::Scene scene;
    scene.create("Saved");
    std::string error;
    const std::string scenePath = (dir / "main.scene").string();
    check(kast::saveScene(scene, scenePath, error), "save a scene");
    const std::string saved = readText(scenePath);
    scene.create("Unsaved");
    std::filesystem::create_directories(scenePath + ".tmp");
    check(!kast::saveScene(scene, scenePath, error) && readText(scenePath) == saved, "failed scene save keeps the old file");
    std::filesystem::remove(scenePath + ".tmp");

    // A file that exists but can't be opened (locked) is a read failure, not empty content.
    writeFile(dir / "locked.lua", "return 1");
    int fd = -1;
    check(_wsopen_s(&fd, (dir / "locked.lua").c_str(), _O_RDONLY, _SH_DENYRW, 0) == 0, "lock a file exclusively");
    std::string bytes;
    check(!kast::readFile(dir / "locked.lua", bytes), "readFile fails on a locked file");
    check(!kast::writePak(dir, {"locked.lua"}, dir / "game.pak", error) && contains(error, "locked.lua"),
          "writePak fails on a locked file instead of packing it empty");
    if (fd != -1)
        _close(fd);
    check(kast::readFile(dir / "locked.lua", bytes) && bytes == "return 1", "readFile reads it once unlocked");
}

void checkBuild()
{
    const std::pair<const char*, const char*> names[] = {
        {"Switch Test", "Switch Test.exe"}, {"  My: Game?  ", "My_ Game_.exe"}, {"a/b\\c", "a_b_c.exe"},
        {"Game...", "Game.exe"},            {"con", "con_.exe"},              {"Nul.v2", "Nul.v2_.exe"},
        {"", "Game.exe"},                   {" .. ", "Game.exe"},              {"tab\there", "tab_here.exe"},
    };
    for (const auto& [name, exe] : names) {
        check(gameExeName(name) == exe, "gameExeName sanitizes the project name");
        if (gameExeName(name) != exe)
            kast::log::error(std::format("  '{}' -> '{}', expected '{}'", name, gameExeName(name), exe));
    }

    const std::filesystem::path dir = looseTestDir("kast_build_check");
    const std::filesystem::path root = dir / "game";
    for (const char* file : {"project.kast", "a.scene", "scripts/s.lua", ".hidden", ".kast/editor.json", ".git/HEAD",
                             "scripts/.cache/x", "out/old.exe"}) {
        std::filesystem::create_directories((root / file).parent_path());
        writeFile(root / file, "x");
    }
    check(pakContents(root, root / "out") == std::vector<std::string>{"a.scene", "project.kast", "scripts/s.lua"},
          "pak contents skip dotfiles, dot-directories and an output folder inside the project");
    check(pakContents(root, dir).size() == 4, "an output folder above the project excludes nothing else");

    // Validation writes nothing.
    kast::Project project{.root = root, .name = "Demo"};
    const std::filesystem::path out = dir / "out";
    const std::filesystem::path playerDir = std::filesystem::path(SDL_GetBasePath());
    std::filesystem::path exe;
    std::string error;
    check(!buildGame(project, out, playerDir, exe, error) && contains(error, "no startScene") &&
              contains(error, "Build > Build Settings"),
          "Build needs a startScene and points at Build Settings");
    project.startScene = "missing.scene";
    check(!buildGame(project, out, playerDir, exe, error) && contains(error, "missing.scene"),
          "Build needs a startScene that exists");
    project.startScene = "a.scene"; // "x": not a valid scene
    check(!buildGame(project, out, playerDir, exe, error) && contains(error, "Invalid scene"),
          "Build needs a startScene that loads");
    project.startScene = "../escape.scene";
    check(!buildGame(project, out, playerDir, exe, error) && contains(error, "outside the project"),
          "Build rejects a startScene outside the project");
    saveTestScene(root / "a.scene", "Thing", {});
    project.startScene = "a.scene";
    check(!buildGame(project, root, playerDir, exe, error) && contains(error, "other than the project folder"),
          "Build rejects the project folder as output");
    std::string upperRoot = root.string();
    std::ranges::transform(upperRoot, upperRoot.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::filesystem::path& same : {std::filesystem::path(root.native() + L"\\"), std::filesystem::path(upperRoot)})
        check(!buildGame(project, same, playerDir, exe, error) && contains(error, "other than the project folder"),
              "Build rejects the project folder as output with a trailing separator or different case");
    check(!buildGame(project, root / "scripts", playerDir, exe, error) && contains(error, "previous build") &&
              readText(root / "scripts/s.lua") == "x" && !std::filesystem::exists(root / "scripts" / kGamePakName),
          "Build rejects a content folder inside the project as output");
    for (const char* folder : {".wip", "build"})
        std::filesystem::create_directories(root / folder);
    saveTestScene(root / ".wip/a.scene", "Thing", {});
    project.startScene = ".wip/a.scene";
    check(!buildGame(project, out, playerDir, exe, error) && contains(error, ".wip/a.scene") && contains(error, "game.pak"),
          "Build rejects a startScene in a dot-folder (game.pak leaves it out)");
    writeFile(root / "build" / kGamePakName, "old");
    saveTestScene(root / "build/s.scene", "Thing", {});
    project.startScene = "build/s.scene";
    check(!buildGame(project, root / "build", playerDir, exe, error) && contains(error, "output folder") &&
              readText(root / "build" / kGamePakName) == "old",
          "Build rejects a startScene inside an in-project output folder");
    std::filesystem::remove_all(root / "build");
    project.startScene = "a.scene";
    check(!buildGame(project, out, dir, exe, error) && contains(error, "kast_player.exe"),
          "Build needs kast_player.exe next to the editor");
    check(!std::filesystem::exists(out), "failed builds write nothing");

    check(buildGame(project, out, playerDir, exe, error), "Build a valid project");
    check(exe == out / "Demo.exe", "Build names the exe after the project");
    for (const char* file : {"Demo.exe", "game.pak", "SDL3.dll", "lua51.dll"})
        check(std::filesystem::is_regular_file(out / file), "Build writes the exe, game.pak and the player's DLLs");
    kast::Pak pak;
    check(pak.open(out / kGamePakName, error) && pak.list("") == pakContents(root, out) &&
              pak.find("a.scene") == std::optional<std::string_view>(readText(root / "a.scene")),
          "game.pak holds the project content");
    writeFile(out / "keep.txt", "mine");
    check(buildGame(project, out, playerDir, exe, error) && readText(out / "keep.txt") == "mine",
          "Build overwrites its own files and leaves others alone");
    check(buildGame(project, root / "builds/a", playerDir, exe, error) &&
              buildGame(project, root / "builds/a", playerDir, exe, error),
          "Build accepts a new in-project output folder, then rebuilds into it (it holds game.pak)");
    std::filesystem::remove_all(root / "builds");
    kast::Project cafe = project;
    cafe.name = reinterpret_cast<const char*>(u8"Caf\u00e9");
    check(buildGame(cafe, dir / u8"out_\u65e5\u672c", playerDir, exe, error) && exe.filename().u8string() == u8"Caf\u00e9.exe" &&
              std::filesystem::is_regular_file(exe),
          "Build names the exe after a non-ASCII project name");

    // The last output folder is editor-local: .kast/editor.json, project.kast untouched.
    const std::string projectFile = readText(root / "project.kast");
    check(!loadBuildFolder(root / "scripts"), "no remembered Build folder without .kast/editor.json");
    check(saveBuildFolder(root, out, error) && loadBuildFolder(root) == std::filesystem::path(out.generic_string()),
          "remembered Build folder round trip");
    const std::filesystem::path unicodeOut = dir / u8"J\u00fcrgen \u65e5\u672c";
    check(saveBuildFolder(root, unicodeOut, error) && loadBuildFolder(root) == unicodeOut,
          "remembered non-ASCII Build folder round trip");
    check(readText(root / "project.kast") == projectFile, "remembering the Build folder leaves project.kast unchanged");
    writeFile(root / ".kast/editor.json", "{oops");
    check(!loadBuildFolder(root), "an invalid editor.json is ignored");

    // Build menu items: a project is open and Play is stopped.
    Document doc;
    resetToDefault(doc);
    FileActions files(doc);
    check(!files.canBuild(), "Build is disabled with no project");
    doc.project = project;
    check(files.canBuild(), "Build is enabled with a project, stopped");
    startPlay(doc);
    check(!files.canBuild(), "Build is disabled while playing");
    togglePause(doc);
    check(!files.canBuild(), "Build is disabled while paused");
    stopPlay(doc);
    check(files.canBuild(), "Build is enabled again after Stop");
}

// Build Settings: Start Scene choices, Save validating and writing only project.kast, Build opening the modal.
void checkBuildSettings()
{
    check(projectScenes(KAST_TEST_PROJECT_DIR) ==
              std::vector<std::string>{"audio.scene", "custom_shaders.scene", "events.scene", "fsm.scene", "gpu_textures.scene", "gpu_textures_bad.scene", "health.scene", "material_features.scene",
                                       "materials.scene", "missing_component.scene", "models.scene",
                                       "models_anim.scene", "models_lua.scene", "models_material.scene",
                                       "models_missing.scene", "net_a.scene", "net_b.scene", "online_auth.scene", "online_npcs.scene", "online_replication.scene", "online_session.scene", "online_world.scene",
                                       "particles_blend.scene", "particles_fog.scene", "particles_hdr.scene",
                                       "particles_shapes.scene", "particles_space.scene", "particles_texture.scene",
                                       "physics.scene",
                                       "point_lights.scene",
                                       "point_shadows.scene", "post_processing.scene", "prefabs.scene", "scene_a.scene", "scene_b.scene",
                                       "shader_errors.scene", "shader_graph_demo.scene", "shader_graphs.scene", "shader_light.scene", "shader_screen.scene", "shader_screen_identity.scene", "shader_showcase.scene", "shader_vertex.scene", "shader_vertex_offset.scene", "shader_vertex_offset_reference.scene", "shadows.scene", "sky_cubemap.scene", "sky_cubemap_back.scene", "sky_mismatched.scene",
                                       "sky_missing_face.scene", "sky_procedural.scene", "terrain.scene", "ui.scene", "ui_drag.scene", "ui_effects.scene",
                                       "zone_objects.scene"},
          "tests/project lists its scenes, sorted");

    const std::filesystem::path root = looseTestDir("kast_build_settings_check") / "game";
    std::filesystem::copy(KAST_TEST_PROJECT_DIR, root, std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(root / ".hidden");
    std::filesystem::create_directories(root / "levels");
    std::filesystem::copy_file(root / "scene_b.scene", root / ".hidden/x.scene");
    std::filesystem::copy_file(root / "scene_b.scene", root / ".dot.scene");
    std::filesystem::copy_file(root / "scene_b.scene", root / "levels/c.scene");
    writeFile(root / "broken.scene", "{oops");
    check(projectScenes(root) == std::vector<std::string>{"audio.scene", "broken.scene", "custom_shaders.scene", "events.scene", "fsm.scene",
                                                          "gpu_textures.scene", "gpu_textures_bad.scene", "health.scene", "levels/c.scene", "material_features.scene", "materials.scene",
                                                          "missing_component.scene", "models.scene",
                                                          "models_anim.scene", "models_lua.scene",
                                                          "models_material.scene", "models_missing.scene", "net_a.scene",
                                                          "net_b.scene", "online_auth.scene", "online_npcs.scene", "online_replication.scene", "online_session.scene", "online_world.scene",
                                                          "particles_blend.scene", "particles_fog.scene", "particles_hdr.scene",
                                                          "particles_shapes.scene", "particles_space.scene",
                                                          "particles_texture.scene", "physics.scene",
                                                          "point_lights.scene", "point_shadows.scene",
                                                          "post_processing.scene", "prefabs.scene", "scene_a.scene", "scene_b.scene",
                                                          "shader_errors.scene", "shader_graph_demo.scene", "shader_graphs.scene", "shader_light.scene", "shader_screen.scene", "shader_screen_identity.scene", "shader_showcase.scene", "shader_vertex.scene", "shader_vertex_offset.scene", "shader_vertex_offset_reference.scene", "shadows.scene", "sky_cubemap.scene", "sky_cubemap_back.scene", "sky_mismatched.scene",
                                                          "sky_missing_face.scene", "sky_procedural.scene", "terrain.scene", "ui.scene", "ui_drag.scene", "ui_effects.scene",
                                                          "zone_objects.scene"},
          "Start Scene choices: sorted '/' paths, no dotfile or dot-directory scenes (the pak rule)");

    Document doc;
    std::string error;
    check(openProject(doc, root / kast::kProjectFileName, error) && doc.project->startScene == "scene_a.scene",
          "Build Settings precondition: the project copy opens scene_a.scene");
    createObject(doc, ObjectKind::Cube, entt::null); // unsaved edit: Save must leave it alone
    const std::string scenePath = doc.path;
    const std::string before = readText(root / kast::kProjectFileName);

    FileActions files(doc);
    files.openBuildSettings();
    check(files.buildSettingsOpen() && !files.idle() && !files.canBuild(), "Build Settings open: not idle, Build disabled");
    check(readText(root / kast::kProjectFileName) == before, "opening Build Settings (then Cancel) writes nothing");

    check(!setStartScene(*doc.project, "broken.scene", error) && contains(error, "broken.scene") &&
              readText(root / kast::kProjectFileName) == before && doc.project->startScene == "scene_a.scene",
          "Save of a scene that doesn't load fails, project.kast byte-identical, project unchanged");
    check(!setStartScene(*doc.project, "../escape.scene", error) && readText(root / kast::kProjectFileName) == before,
          "Save of a scene outside the project fails, project.kast byte-identical");
    check(!setStartScene(*doc.project, ".hidden/x.scene", error) && contains(error, "game.pak") &&
              readText(root / kast::kProjectFileName) == before,
          "Save of a dot-folder scene (not in game.pak) fails, project.kast byte-identical");

    check(setStartScene(*doc.project, "levels/c.scene", error), "Save a valid Start Scene");
    check(readText(root / kast::kProjectFileName) ==
              "{\n  \"version\": 1,\n  \"name\": \"Switch Test\",\n  \"startScene\": \"levels/c.scene\",\n"
              "  \"stateMachines\": [\n    \"game_flow\"\n  ]\n}\n",
          "Save writes project.kast with the new startScene, other keys and order kept");
    kast::Project reloaded;
    check(kast::loadProject(root / kast::kProjectFileName, reloaded, error) && reloaded.startScene == "levels/c.scene" &&
              doc.project->startScene == "levels/c.scene",
          "saved project.kast reloads and doc.project is updated");
    check(doc.dirty && doc.path == scenePath, "Save leaves the open scene's dirty flag and path alone");
    check(!std::filesystem::exists(root / ".kast/editor.json"), "Save doesn't write .kast/editor.json");

    // Build / Build and Run with a startScene that can't be built open Build Settings instead of the folder dialog.
    // ponytail: "no folder dialog" is the early return in perform(); an invalid project never reaches SDL here.
    doc.dirty = false;
    for (const std::optional<std::string>& start : {std::optional<std::string>(), std::optional<std::string>("missing.scene")}) {
        for (const bool run : {false, true}) {
            Document buildDoc;
            buildDoc.project = doc.project;
            buildDoc.project->startScene = start;
            FileActions buildFiles(buildDoc);
            run ? buildFiles.buildAndRun() : buildFiles.build();
            const std::string what = std::format("{} with startScene {} opens Build Settings",
                                                 run ? "Build and Run" : "Build", start.value_or("null"));
            check(buildFiles.buildSettingsOpen(), what.c_str());
        }
    }
}

// --- Events ---

// The C++ bus alone: publish and subscription order, next-drain re-publish, subscribe/unsubscribe during a drain,
// a throwing subscriber, RAII handles, dropQueued, reset and the bounded history.
void checkEventBus()
{
    kast::EventBus bus;
    std::vector<std::string> seen;
    const auto record = [&](std::string tag) {
        return [&seen, tag](const nlohmann::json& payload) { seen.push_back(tag + payload.dump()); };
    };
    kast::EventBus::Subscription a1 = bus.subscribe("a", record("a1"));
    kast::EventBus::Subscription b1 = bus.subscribe("b", record("b1"));
    kast::EventBus::Subscription a2 = bus.subscribe("a", record("a2"));
    bus.publish("b", 1);
    bus.publish("a", 2);
    check(seen.empty(), "bus: publish only queues");
    bus.drain();
    check(seen == std::vector<std::string>{"b11", "a12", "a22"}, "bus: publish order, then subscription order");
    check(bus.history().size() == 2 && bus.history()[0].frame == 1 && bus.history()[0].name == "b" &&
              bus.history()[0].payload == "1" && bus.history()[0].publisher == "C++",
          "bus: history records frame, name, compact payload and publisher");

    // A subscriber that re-publishes, subscribes and unsubscribes during a drain.
    seen.clear();
    std::vector<kast::EventBus::Subscription> added;
    kast::EventBus::Subscription cut;
    kast::EventBus::Subscription actor = bus.subscribe("act", [&](const nlohmann::json&) {
        seen.push_back("actor");
        bus.publish("act", "again");                     // next drain
        added.push_back(bus.subscribe("act", record("late"))); // from the next event
        cut.unsubscribe();                                 // immediately, even for this event
        cut.unsubscribe();                                 // twice: no-op
    });
    cut = bus.subscribe("act", record("cut"));
    bus.publish("act", nullptr);
    bus.drain();
    check(seen == std::vector<std::string>{"actor"}, "bus: unsubscribe mid-drain is immediate; a new subscription waits");
    seen.clear();
    bus.drain();
    check(seen == std::vector<std::string>{"actor", "late\"again\""}, "bus: an event published in a drain arrives in the next");
    actor.unsubscribe();
    added.clear();
    bus.drain(); // the last "again"

    // A subscriber publishing mid-delivery grows the queue the event being delivered sits in: the subscribers after it
    // must still get that event (Replication publishes world.entitySpawned from world.spawn, Lua listens after it).
    {
        kast::EventBus grow;
        std::vector<std::string> got;
        kast::EventBus::Subscription publisher = grow.subscribe("spawn", [&](const nlohmann::json&) {
            for (int i = 0; i < 100; ++i)
                grow.publish("spawned", i);
        });
        kast::EventBus::Subscription after =
            grow.subscribe("spawn", [&](const nlohmann::json& payload) { got.push_back(payload.get<std::string>()); });
        grow.publish("spawn", "Guard");
        grow.publish("spawn", "Bat");
        grow.drain();
        check(got == std::vector<std::string>{"Guard", "Bat"},
              "bus: a later subscriber still gets the event after an earlier one published mid-delivery");
    }
    seen.clear();

    bool after = false;
    kast::EventBus::Subscription thrower = bus.subscribe("t", [](const nlohmann::json&) { throw std::runtime_error("bad"); });
    kast::EventBus::Subscription next = bus.subscribe("t", [&](const nlohmann::json&) { after = true; });
    bus.publish("t", nullptr);
    bus.drain();
    check(after && logged("Event 't': C++ subscriber failed: bad"), "bus: a throwing C++ subscriber is logged, the rest run");

    {
        kast::EventBus::Subscription scoped = bus.subscribe("s", record("scoped"));
        kast::EventBus::Subscription moved = std::move(scoped);
    }
    bus.publish("s", nullptr);
    bus.drain();
    check(seen.empty(), "bus: a destroyed handle unsubscribes (after a move too)");

    bus.publish("a", 3);
    bus.dropQueued();
    bus.drain();
    check(seen.empty(), "bus: dropQueued discards undelivered events");

    for (int i = 0; i < 600; ++i)
        bus.publish("h", i);
    bus.drain();
    check(bus.history().size() == kast::EventBus::kHistorySize && bus.history().back().payload == "599" &&
              bus.history().front().payload == "100",
          "bus: the history keeps the last 500 events");
    bus.clearHistory();
    check(bus.history().empty(), "bus: clearHistory (the Events panel's Clear) empties the history");

    bus.publish("a", 4);
    bus.reset();
    bus.publish("a", 5);
    bus.drain();
    check(seen.empty() && bus.history().size() == 1 && bus.history()[0].frame == 1,
          "bus: reset drops the queue, subscriptions and history and restarts the frame count");
    a1.unsubscribe(); // stale after reset: no-op
    kast::EventBus::Subscription fresh = bus.subscribe("a", record("fresh"));
    a2.unsubscribe(); // stale id must not remove the newer subscription
    bus.publish("a", 6);
    bus.drain();
    check(seen == std::vector<std::string>{"fresh6"}, "bus: stale handles never remove newer subscriptions");
}

// Lua -> Lua and Lua -> C++: same frame after every update, shared subscription order, independent payload copies,
// payload conversion.
void checkLuaEvents()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "a.lua", R"(local frame = 0
assert(type(kast.publish) == "function" and type(kast.subscribe) == "function" and type(kast.unsubscribe) == "function")
assert(not pcall(function() kast.publish = nil end), "kast.publish can't be overwritten")
return { update = function(dt)
    frame = frame + 1
    kast.log("ev a update " .. frame)
    if frame == 1 then
        kast.publish("first", {n = 1})
        kast.publish("data", {list = {1, 2, 3}, obj = {a = "x"}, empty = {}, flag = true, s = "str", f = 1.5, i = -7})
        kast.publish("bare")
    end
end })");
    writeFile(dir / "b.lua", R"(return {
    init = function()
        kast.subscribe("first", function(p) kast.log("ev b first " .. p.n); p.n = 99; p.extra = true end)
        kast.subscribe("data", function(p) kast.log("ev b data") end)
    end,
    update = function(dt) kast.log("ev b update") end,
})");
    writeFile(dir / "d.lua", R"(local subscribed = false
return { update = function(dt)
    if not subscribed then
        subscribed = true
        kast.subscribe("first", function(p)
            assert(p.n == 1 and p.extra == nil, "each subscriber gets its own copy")
            kast.log("ev d first")
        end)
    end
end })");
    kast::Scene scene;
    scene.systems = {"a.lua", "b.lua", "d.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    nlohmann::json first, data, bare = "unset";
    kast::EventBus::Subscription s1 = lua.bus().subscribe("first", [&](const nlohmann::json& p) {
        kast::log::info("ev C++ first");
        first = p;
    });
    kast::EventBus::Subscription s2 = lua.bus().subscribe("data", [&](const nlohmann::json& p) { data = p; });
    kast::EventBus::Subscription s3 = lua.bus().subscribe("bare", [&](const nlohmann::json& p) { bare = p; });
    lua.tick(0.1f);
    checkOk(lua, 0, "events: kast.publish/subscribe/unsubscribe exist and are read-only");
    checkOk(lua, 2, "events: Lua subscribers get independent copies");
    check(logIndex("ev a update 1") < logIndex("ev b update") && logIndex("ev b update") < logIndex("ev b first 1"),
          "events: Lua -> Lua in the same frame, after every update");
    check(logIndex("ev b first 1") < logIndex("ev C++ first") && logIndex("ev C++ first") < logIndex("ev d first") &&
              logIndex("ev d first") < logIndex("ev b data") && logIndex("ev b data") != SIZE_MAX,
          "events: publish order, then one subscription order across Lua and C++");
    check(first == nlohmann::json{{"n", 1}}, "events: Lua -> C++ payload, unchanged by Lua subscribers");
    check(data == nlohmann::json::parse(
                      R"({"list": [1, 2, 3], "obj": {"a": "x"}, "empty": {}, "flag": true, "s": "str", "f": 1.5, "i": -7})"),
          "events: payload conversion (arrays, objects, empty table, scalars)");
    check(data["i"].is_number_integer() && bare.is_null(), "events: integral numbers are integers; no payload is null");
    check(lua.bus().history().size() == 3 && lua.bus().history()[0].publisher == "a.lua",
          "events: the publisher is the system's path");
    lua.stop();
}

// C++ -> Lua, JSON -> Lua conversion, re-publish and subscribe during a drain, unsubscribe during a drain.
void checkLuaEventsFromCpp()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "r.lua", R"(local late = 0
local second
return {
    init = function()
        kast.subscribe("cpp", function(p)
            assert(#p.arr == 2 and p.arr[1] == 1 and p.arr[2] == 2, "array -> sequence")
            assert(p.o.k == "v" and p.b == false and p.nul == nil and p.i == 7 and p.s == "x", "object -> table")
            assert(next(p.e) == nil, "empty object -> empty table")
            kast.log("ev r cpp")
            kast.publish("pong", {from = "r"}) -- next frame
            kast.subscribe("cpp", function() late = late + 1; kast.log("ev r late " .. late) end) -- from the next event
        end)
        kast.subscribe("cut", function()
            kast.unsubscribe(second)
            kast.unsubscribe(second) -- twice: no-op
            kast.log("ev r cutter")
        end)
        second = kast.subscribe("cut", function() kast.log("ev r second") end)
    end,
    update = function(dt) end,
})");
    kast::Scene scene;
    scene.systems = {"r.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    int pongs = 0;
    kast::EventBus::Subscription thrower =
        lua.bus().subscribe("pong", [](const nlohmann::json&) { throw std::runtime_error("pong failed"); });
    kast::EventBus::Subscription counter = lua.bus().subscribe("pong", [&](const nlohmann::json&) { ++pongs; });
    const nlohmann::json payload =
        nlohmann::json::parse(R"({"arr": [1, 2], "o": {"k": "v"}, "b": false, "nul": null, "i": 7, "s": "x", "e": {}})");
    lua.bus().publish("cpp", payload);
    lua.bus().publish("cut", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "events: C++ -> Lua payload converts to tables");
    check(logged("ev r cpp") && pongs == 0 && !logged("ev r late"),
          "events: published during a drain waits; a subscription added during a drain misses the current event");
    check(logged("ev r cutter") && !logged("ev r second"), "events: unsubscribe during a drain is immediate");
    lua.bus().publish("cpp", payload);
    lua.tick(0.1f);
    check(pongs == 1 && logged("Event 'pong': C++ subscriber failed: pong failed"),
          "events: re-published event arrives next frame; a throwing C++ subscriber doesn't stop the rest");
    check(logged("ev r late 1") && !logged("ev r late 2"), "events: the late subscription gets later events only");
    checkOk(lua, 0, "events: the system is still fine");
    lua.stop();
}

// Rejections raise at the calling file:line and queue nothing.
void checkLuaEventErrors()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "rej.lua", R"(local cyc = {}; cyc.self = cyc
local deep = {}; local t = deep; for i = 1, 32 do t.x = {}; t = t.x end
local ok32 = {}; t = ok32; for i = 1, 31 do t.x = {}; t = t.x end
local function try(f) local ok, err = pcall(f); kast.log("rej " .. tostring(ok) .. " " .. tostring(err)) end
return { update = function(dt)
    try(function() kast.publish(42) end)
    try(function() kast.publish("") end)
    try(function() kast.publish("e", {f = print}) end)
    try(function() kast.publish("e", {u = newproxy()}) end)
    try(function() kast.publish("e", {co = coroutine.create(print)}) end)
    try(function() kast.publish("e", cyc) end)
    try(function() kast.publish("e", deep) end)
    try(function() kast.publish("e", {1, a = 2}) end)
    try(function() kast.publish("e", {1, nil, 3}) end)
    try(function() kast.publish("e", {[0] = 1}) end)
    try(function() kast.publish("e", {x = 0 / 0}) end)
    try(function() kast.subscribe("e", 5) end)
    try(function() kast.subscribe("", print) end)
    try(function() kast.unsubscribe(5) end)
    try(function() kast.unsubscribe({}) end)
    kast.publish("ok32", ok32)
end })");
    kast::Scene scene;
    scene.systems = {"rej.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "events: rejections are catchable Lua errors");
    const std::pair<int, const char*> cases[] = {
        {6, "kast.publish: event name must be a non-empty string, got 42"},
        {7, "kast.publish: event name must be a non-empty string"},
        {8, "kast.publish: payload.f: a function can't be sent in an event"},
        {9, "kast.publish: payload.u: a userdata can't be sent in an event"},
        {10, "kast.publish: payload.co: a thread can't be sent in an event"},
        {11, "kast.publish: payload.self: the table contains itself (cycle)"},
        {12, "kast.publish: payload.x.x.x"}, // ... 32 levels: checked below
        {13, "kast.publish: payload: table keys must be exactly 1..n (an array) or all strings (an object)"},
        {14, "kast.publish: payload: table keys must be exactly 1..n"},
        {15, "kast.publish: payload: table keys must be exactly 1..n"},
        {16, "kast.publish: payload.x must be a finite number"},
        {17, "kast.subscribe: callback must be a function, got 5"},
        {18, "kast.subscribe: event name must be a non-empty string"},
        {19, "kast.unsubscribe: expected a handle from kast.subscribe, got 5"},
        {20, "kast.unsubscribe: expected a handle from kast.subscribe, got table"},
    };
    for (const auto& [line, message] : cases)
        check(logged(std::format("rej false rej.lua:{}: {}", line, message)),
              std::format("events: rejected at rej.lua:{}: {}", line, message).c_str());
    check(logged("x: tables are nested more than 32 deep"), "events: nesting deeper than 32 is rejected");
    check(lua.bus().history().size() == 1 && lua.bus().history()[0].name == "ok32",
          "events: rejected calls queue nothing; 32 nested tables are fine");
    lua.stop();
}

// A failing subscriber disables only its system (and removes its subscriptions); hot reload, Stop and Play start
// clean subscriptions up.
void checkLuaEventLifetime()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "bad.lua", R"(return {
    init = function()
        kast.subscribe("hit", function(p)
            error("subscriber failed")
        end)
        kast.subscribe("hit", function(p) kast.log("ev bad second") end)
        kast.subscribe("other", function(p) kast.log("ev bad other") end)
    end,
    update = function(dt) end,
})");
    const auto good = [](const char* tag) {
        return std::format(R"(return {{
    init = function() kast.subscribe("hit", function(p) kast.log("ev good {0} " .. p.n) end) end,
    update = function(dt) end,
}})",
                           tag);
    };
    writeFile(dir / "good.lua", good("v1"));
    kast::Scene scene;
    scene.systems = {"bad.lua", "good.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.bus().publish("hit", {{"n", 1}});
    lua.tick(0.1f);
    const kast::LuaSystemStatus bad = statusOf(lua, 0);
    check(!bad.ok && contains(bad.error, "bad.lua:4: subscriber failed") &&
              logged("Lua system disabled: bad.lua:4: subscriber failed"),
          "events: a failing subscriber disables its system with file:line");
    check(!logged("ev bad second") && logged("ev good v1 1") && statusOf(lua, 1).ok,
          "events: the failed system's other subscribers stop; other systems' still run");
    lua.bus().publish("other", nullptr);
    lua.tick(0.1f);
    check(!logged("ev bad other"), "events: disabling removes all of the system's subscriptions");

    rewrite(dir / "good.lua", good("v2"));
    lua.tick(0.1f); // reload: the old instance's subscription goes
    lua.bus().publish("hit", {{"n", 2}});
    lua.tick(0.1f);
    check(logged("ev good v2 2") && !logged("ev good v1 2"), "events: hot reload drops the old instance's subscriptions");

    rewrite(dir / "bad.lua", R"(return {
    init = function() kast.subscribe("hit", function(p) kast.log("ev fixed " .. p.n) end) end,
    update = function(dt) end,
})");
    lua.tick(0.1f);
    lua.bus().publish("hit", {{"n", 3}});
    lua.tick(0.1f);
    check(statusOf(lua, 0).ok && logged("ev fixed 3"), "events: a good hot reload subscribes again");

    int cpp = 0;
    kast::EventBus::Subscription sub = lua.bus().subscribe("hit", [&](const nlohmann::json&) { ++cpp; });
    lua.bus().publish("hit", {{"n", 4}});
    lua.stop();
    lua.bus().drain();
    check(cpp == 0 && !logged("ev good v2 4") && !logged("ev fixed 4"), "events: Stop drops queued events");
    lua.bus().publish("hit", {{"n", 5}});
    lua.bus().drain();
    check(cpp == 1 && !logged("ev good v2 5") && !logged("ev fixed 5"), "events: no Lua subscriber is left after Stop");
    check(!lua.bus().history().empty(), "events: the history outlives Stop");

    lua.start(scene, dir, "", nullptr);
    check(lua.bus().history().empty(), "events: Play start clears the history");
    lua.bus().publish("hit", {{"n", 6}});
    lua.tick(0.1f);
    check(cpp == 1 && logged("ev fixed 6"), "events: Play start clears C++ subscriptions");
    lua.stop();
}

// A subscriber registered inside a coroutine outlives the coroutine.
void checkLuaEventCoroutine()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "co.lua", R"(return {
    init = function()
        coroutine.resume(coroutine.create(function()
            kast.subscribe("co", function(p) kast.log("ev co " .. p.n .. " " .. tostring(coroutine.running())) end)
        end))
    end,
    update = function(dt) collectgarbage(); collectgarbage() end,
})");
    kast::Scene scene;
    scene.systems = {"co.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.bus().publish("co", {{"n", 1}});
    lua.tick(0.1f); // the coroutine is collected before the drain
    lua.bus().publish("co", {{"n", 2}});
    lua.tick(0.1f);
    check(statusOf(lua, 0).ok && logged("ev co 1 nil") && logged("ev co 2 nil"),
          "events: a subscriber from a collected coroutine runs on the main thread");
    lua.stop();
}

// Scene switch: old Lua subscribers are gone, queued events dropped, C++ subscriptions and history kept.
void checkLuaEventSceneSwitch()
{
    const std::filesystem::path dir = projectTestDir("kast_event_switch_check", "Events");
    saveTestScene(dir / "a.scene", "A", {"scripts/a.lua"});
    saveTestScene(dir / "b.scene", "B", {"scripts/b.lua"});
    writeFile(dir / "scripts/a.lua", R"(return {
    init = function()
        kast.subscribe("go", function(p)
            kast.log("ev a go")
            kast.publish("late") -- queued for the next frame: dropped by the switch
            kast.loadScene("b.scene")
        end)
        kast.subscribe("ping", function(p) kast.log("ev a ping") end)
    end,
    update = function(dt) end,
    shutdown = function() kast.publish("from shutdown") end, -- dropped
})");
    writeFile(dir / "scripts/b.lua", R"(return {
    init = function() kast.subscribe("ping", function(p) kast.log("ev b ping") end) end,
    update = function(dt) end,
})");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open a.scene for the event switch");
    startPlay(doc);
    std::vector<std::string> cpp;
    kast::EventBus::Subscription sub = doc.lua.bus().subscribe("ping", [&](const nlohmann::json&) { cpp.push_back("ping"); });
    kast::EventBus::Subscription late = doc.lua.bus().subscribe("late", [&](const nlohmann::json&) { cpp.push_back("late"); });
    kast::EventBus::Subscription shut =
        doc.lua.bus().subscribe("from shutdown", [&](const nlohmann::json&) { cpp.push_back("shutdown"); });
    doc.lua.bus().publish("go", nullptr);
    updatePlay(doc, 0.1f);
    check(logged("ev a go") && doc.playSceneName == "b", "events: a subscriber's loadScene switches at the end of the frame");
    doc.lua.bus().publish("ping", nullptr);
    updatePlay(doc, 0.1f);
    check(logged("ev b ping") && !logged("ev a ping"), "events: no Lua subscriber of the old scene runs after a switch");
    check(cpp == std::vector<std::string>{"ping"}, "events: C++ subscriptions survive a switch; queued events are dropped");
    check(doc.lua.bus().history().size() == 2 && doc.lua.bus().history()[0].name == "go",
          "events: the history carries over a switch");
    stopPlay(doc);
}

// Editor Play: nothing is delivered while paused; Stop discards the queue.
void checkLuaEventPause()
{
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "scripts/tick.lua", R"(local n = 0
return { update = function(dt) n = n + 1; kast.publish("tick", {n = n}) end })");
    Document doc;
    doc.path = (dir / "pause.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Pause"};
    doc.scene.systems = {"scripts/tick.lua"};
    startPlay(doc);
    std::vector<int> ticks;
    kast::EventBus::Subscription sub =
        doc.lua.bus().subscribe("tick", [&](const nlohmann::json& p) { ticks.push_back(p["n"].get<int>()); });
    updatePlay(doc, 0.1f);
    check(ticks == std::vector<int>{1}, "events: delivered in editor Play");
    togglePause(doc);
    doc.lua.bus().publish("tick", {{"n", 100}});
    updatePlay(doc, 0.1f);
    updatePlay(doc, 0.1f);
    check(ticks == std::vector<int>{1}, "events: nothing is delivered while paused");
    togglePause(doc);
    updatePlay(doc, 0.1f);
    check((ticks == std::vector<int>{1, 100, 2}), "events: resuming delivers the held queue in order");
    togglePause(doc);
    doc.lua.bus().publish("tick", {{"n", 200}});
    stopPlay(doc);
    doc.lua.bus().drain();
    check((ticks == std::vector<int>{1, 100, 2}), "events: Stop discards the queue");
    check(doc.lua.bus().history().size() == 3, "events: the Events panel history is kept after Stop");
}

// --- State machines ---

// door (fsms/sub/door.lua, nested to show recursive discovery) and game (fsms/game.lua). Every callback logs
// "<prefix> ..." so each check reads its own lines. In game.lua, line 15 raises.
void writeFsmDefinitions(const std::filesystem::path& dir, const std::string& prefix, const std::string& doorTag = "")
{
    std::filesystem::create_directories(dir / "fsms/sub");
    std::string door = R"(local function say(text)
    return function(ctx)
        kast.log(string.format("%P%%T% %s %s from=%s to=%s entity=%s", text, ctx.machine, tostring(ctx.from), ctx.to,
                               tostring(ctx.entity)))
    end
end
return {
    name = "door",
    initial = "closed",
    states = {
        closed = { allowedFrom = {"open"}, enter = say("enter"), exit = say("exit") },
        open = { allowedFrom = {"closed", "locked"}, enter = say("enter"), exit = say("exit") },
        locked = { enter = say("enter"), exit = say("exit") },
        broken = { allowedFrom = {"*"}, enter = say("enter") },
    },
})";
    std::string game = R"(return {
    name = "game",
    initial = "a",
    states = {
        a = { allowedFrom = {"err"}, enter = function(ctx) kast.log("%P% game enter a from=" .. tostring(ctx.from) .. " entity=" .. tostring(ctx.entity)) end },
        b = { allowedFrom = {"a"},
              enter = function(ctx)
                  kast.log("%P% game enter b")
                  local queued = kast.fsm.change("game", "c")
                  kast.log("%P% game queued " .. tostring(queued) .. " still " .. kast.fsm.state("game"))
              end,
              exit = function(ctx) kast.log("%P% game exit b") end },
        c = { allowedFrom = {"b"}, enter = function(ctx) kast.log("%P% game enter c"); kast.fsm.change("game", "d") end },
        d = { allowedFrom = {"a"} },
        err = { allowedFrom = {"*"}, enter = function(ctx) error("%P% boom") end },
    },
})";
    const auto fill = [&](std::string text) {
        for (size_t at; (at = text.find("%P%")) != std::string::npos;)
            text.replace(at, 3, prefix);
        for (size_t at; (at = text.find("%T%")) != std::string::npos;)
            text.replace(at, 3, doorTag);
        return text;
    };
    writeFile(dir / "fsms/sub/door.lua", fill(door));
    writeFile(dir / "fsms/game.lua", fill(game));
}

std::filesystem::path fsmProject(const char* name, const std::string& prefix)
{
    const std::filesystem::path dir = projectTestDir(name, "FSM");
    writeFsmDefinitions(dir, prefix);
    return dir;
}

kast::EntityId idOf(const kast::Scene& scene, entt::entity entity) { return scene.registry().get<kast::PersistentId>(entity).value; }

template <typename Fn>
bool throwsInvalid(Fn&& fn, const std::string& part = "")
{
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        return part.empty() || contains(e.what(), part);
    }
    return false;
}

std::vector<const kast::EventBus::Delivered*> fsmEvents(const kast::LuaSystems& lua)
{
    std::vector<const kast::EventBus::Delivered*> out;
    for (const kast::EventBus::Delivered& event : lua.bus().history())
        if (event.name == kast::kStateChangedEvent)
            out.push_back(&event);
    return out;
}

const kast::ComponentDefinitionStatus* fsmStatus(const kast::StateMachineDefinitions& defs, const std::string& path)
{
    const auto it = std::ranges::find(defs.status, path, &kast::ComponentDefinitionStatus::path);
    return it == defs.status.end() ? nullptr : &*it;
}

// Discovery, every invalid-definition case naming its file, duplicate names, allowedFrom rules, editor rescans.
void checkStateMachineDefinitions()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_defs", "fsmd");
    std::filesystem::create_directories(dir / "fsms/bad");
    const std::pair<const char*, const char*> bad[] = {
        {"noname", R"(return { initial = "a", states = { a = {} } })"},
        {"numname", R"(return { name = 5, initial = "a", states = { a = {} } })"},
        {"badinitial", R"(return { name = "bi", initial = "zz", states = { a = {} } })"},
        {"noinitial", R"(return { name = "ni", states = { a = {} } })"},
        {"badfrom", R"(return { name = "bf", initial = "a", states = { a = { allowedFrom = {"nope"} } } })"},
        {"fromnotlist", R"(return { name = "fl", initial = "a", states = { a = { allowedFrom = "a" } } })"},
        {"fromnumber", R"(return { name = "fn", initial = "a", states = { a = { allowedFrom = {5} } } })"},
        {"statesnum", R"(return { name = "sn", initial = "a", states = 5 })"},
        {"statesempty", R"(return { name = "se", initial = "a", states = {} })"},
        {"statenum", R"(return { name = "st", initial = "a", states = { a = 5 } })"},
        {"starstate", R"(return { name = "ss", initial = "a", states = { a = {}, ["*"] = {} } })"},
        {"enternum", R"(return { name = "en", initial = "a", states = { a = { enter = 5 } } })"},
        {"exitstr", R"(return { name = "ex", initial = "a", states = { a = { exit = "x" } } })"},
        {"statekey", R"(return { name = "sk", initial = "a", states = { a = { update = function() end } } })"},
        {"topkey", R"(return { name = "tk", initial = "a", states = { a = {} }, extra = 1 })"},
        {"syntax", "return {\n  name = \n"},
        {"notable", "return 5"},
        {"sandbox", "local f = io.open\nreturn { name = \"sb\", initial = \"a\", states = { a = {} } }"},
        {"dup1", R"(return { name = "twin", initial = "a", states = { a = {} } })"},
        {"dup2", R"(return { name = "twin", initial = "b", states = { b = {} } })"},
    };
    for (const auto& [name, text] : bad)
        writeFile(dir / "fsms/bad" / (std::string(name) + ".lua"), text);
    writeFile(dir / "fsms/notes.txt", "not a definition");

    const kast::StateMachineDefinitions defs = kast::loadStateMachineDefinitions(dir);
    check(defs.status.size() == std::size(bad) + 2, "fsm: every .lua file under fsms/ is scanned, recursively");
    check(defs.valid.size() == 2 && defs.valid[0].name == "game" && defs.valid[1].name == "door",
          "fsm: valid definitions load (sorted by path: fsms/game.lua, fsms/sub/door.lua)");
    const std::pair<const char*, const char*> errors[] = {
        {"noname", "\"name\" must be a non-empty string"},
        {"numname", "\"name\" must be a non-empty string"},
        {"badinitial", "\"initial\" must name one of its states, got zz"},
        {"noinitial", "\"initial\" must name one of its states, got nil"},
        {"badfrom", "state \"a\": \"allowedFrom\" names unknown state \"nope\""},
        {"fromnotlist", "state \"a\": \"allowedFrom\" must be a list"},
        {"fromnumber", "state \"a\": \"allowedFrom\" must hold state names, got 5"},
        {"statesnum", "\"states\" must be a table"},
        {"statesempty", "\"states\" must not be empty"},
        {"statenum", "state \"a\" must be a table"},
        {"starstate", "state names must be non-empty strings other than \"*\""},
        {"enternum", "state \"a\": \"enter\" must be a function"},
        {"exitstr", "state \"a\": \"exit\" must be a function"},
        {"statekey", "state \"a\": unknown key update"},
        {"topkey", "unknown key extra"},
        {"syntax", ""},
        {"notable", "must return exactly one table"},
        {"sandbox", "attempt to index"},
        {"dup1", "name \"twin\" is also declared in fsms/bad/dup2.lua"},
        {"dup2", "name \"twin\" is also declared in fsms/bad/dup1.lua"},
    };
    for (const auto& [name, message] : errors) {
        const std::string path = std::format("fsms/bad/{}.lua", name);
        const kast::ComponentDefinitionStatus* status = fsmStatus(defs, path);
        const bool ok = status && !status->ok && status->error.starts_with(path + ":") && contains(status->error, message);
        check(ok, std::format("fsm: {} rejected naming its file ({})", path, message).c_str());
        if (!ok && status)
            kast::log::error(std::format("  got: {}", status->error));
    }
    check(contains(fsmStatus(defs, "fsms/bad/sandbox.lua")->error, "fsms/bad/sandbox.lua:1:"),
          "fsm: definition files are sandboxed; Lua errors give file:line");

    const kast::StateMachineDef& door = defs.valid[1];
    check(door.path == "fsms/sub/door.lua" && door.initial == "closed" &&
              door.stateNames() == std::vector<std::string>{"broken", "closed", "locked", "open"},
          "fsm: name, path, initial and sorted state names");
    check(door.allows("open", "closed") && !door.allows("locked", "closed") && !door.allows("closed", "closed"),
          "fsm: allowedFrom is enforced, self needs listing");
    check(door.allows("closed", "locked") && door.allows("locked", "locked"), "fsm: omitted allowedFrom allows any (and self)");
    check(door.allows("open", "broken") && door.allows("broken", "broken"), "fsm: \"*\" allows any (and self)");
    check(!door.allows("open", "nope"), "fsm: an unknown target state is never allowed");

    check(kast::refreshStateMachineDefinitions(dir) && kast::stateMachineDefinitions().valid.size() == 2,
          "fsm: editor scan loads the project's definitions");
    check(logged("State machine rejected: fsms/bad/noname.lua: "), "fsm: rejected files are logged");
    check(!kast::refreshStateMachineDefinitions(dir), "fsm: editor rescan does nothing when nothing changed");
    rewrite(dir / "fsms/bad/noname.lua", R"(return { name = "fixed", initial = "a", states = { a = {} } })");
    check(kast::refreshStateMachineDefinitions(dir) && kast::stateMachineDefinitions().valid.size() == 3,
          "fsm: editor rescans a modified file");
    check(kast::refreshStateMachineDefinitions(std::nullopt) && kast::stateMachineDefinitions().status.empty(),
          "fsm: no project, no definitions");
}

// The C++ API at Play start and through transitions: entry order, allowedFrom, events, queueing, callback errors,
// misuse.
void checkStateMachineRuntime()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_runtime", "fsm1");
    writeFile(dir / "scripts/probe.lua", R"(return { init = function() kast.log("fsm1 probe init") end, update = function(dt) end })");
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity door = scene.create("Door");
    registry.emplace<kast::StateMachine>(door, "door", "");
    const entt::entity gate = scene.create("Gate");
    registry.emplace<kast::StateMachine>(gate, "door", "open");
    const entt::entity plain = scene.create("Plain");
    const kast::EntityId doorId = idOf(scene, door), gateId = idOf(scene, gate), plainId = idOf(scene, plain);
    scene.systems = {"scripts/probe.lua"};
    kast::LuaSystems lua;
    kast::StateMachines& fsm = lua.fsm();
    lua.start(scene, dir, "", nullptr, {"game", "ghost"});

    check(logged("Game state machine \"ghost\" (project.kast \"stateMachines\") has no valid definition"),
          "fsm: a listed game machine without a definition is logged");
    check(logIndex("fsm1 probe init") < logIndex("fsm1 game enter a from=nil entity=nil") &&
              logIndex("fsm1 game enter a from=nil entity=nil") <
                  logIndex(std::format("fsm1 enter door from=nil to=closed entity={}", doorId)) &&
              logIndex(std::format("fsm1 enter door from=nil to=closed entity={}", doorId)) <
                  logIndex(std::format("fsm1 enter door from=nil to=open entity={}", gateId)),
          "fsm: after the inits, game machines then entities (depth-first) enter their initial state");
    check(logged("Entity 'Door' (id 1): StateMachine \"door\": initialState is empty; using \"closed\""),
          "fsm: an empty initialState falls back to the definition's initial, with a warning");
    check(fsm.state("game") == "a" && fsm.state(doorId) == "closed" && fsm.state(gateId) == "open",
          "fsm: current states after Play start");
    lua.tick(0.0f);
    check(fsmEvents(lua).empty(), "fsm: entering the initial state publishes no event");

    const size_t lines = kast::log::lines().size();
    check(fsm.canChange(doorId, "open") && !fsm.canChange(doorId, "closed") && fsm.canChange(doorId, "locked") &&
              fsm.canChange(doorId, "broken") && !fsm.canChange(doorId, "nope") && fsm.canChange("game", "b") &&
              !fsm.canChange("game", "c"),
          "fsm: canChange follows allowedFrom");
    check(kast::log::lines().size() == lines && fsm.state(doorId) == "closed", "fsm: canChange has no side effects or log");
    check(fsm.states("game") == std::vector<std::string>{"a", "b", "c", "d", "err"}, "fsm: states lists the state names");
    check(fsm.peek("game") == "a" && !fsm.peek(kast::EntityId{999}) && !fsm.peek("ghost"), "fsm: peek");

    check(fsm.change(doorId, "open"), "fsm: an allowed transition returns true");
    const std::string exitClosed = std::format("fsm1 exit door from=closed to=open entity={}", doorId);
    const std::string enterOpen = std::format("fsm1 enter door from=closed to=open entity={}", doorId);
    check(fsm.state(doorId) == "open" && logIndex(exitClosed) < logIndex(enterOpen) && logIndex(enterOpen) != SIZE_MAX,
          "fsm: exit(old), then switch, then enter(new), with ctx {machine, entity, from, to}");
    lua.tick(0.0f);
    std::vector<const kast::EventBus::Delivered*> events = fsmEvents(lua);
    check(events.size() == 1 && events[0]->publisher == "fsms/sub/door.lua" &&
              nlohmann::json::parse(events[0]->payload) ==
                  nlohmann::json{{"machine", "door"}, {"entity", doorId}, {"from", "closed"}, {"to", "open"}},
          "fsm: fsm.changed {machine, entity, from, to} is published by the definition file");

    const ptrdiff_t enters = countLogged("fsm1 enter door"), exits = countLogged("fsm1 exit door");
    check(!fsm.change(doorId, "open") && fsm.state(doorId) == "open" &&
              logged("State machine \"door\" (entity 1): \"open\" can't be entered from \"open\""),
          "fsm: a self-transition not in allowedFrom is denied with a warning");
    check(fsm.change(doorId, "closed") && fsm.state(doorId) == "closed", "fsm: closed is allowed from open");
    check(!fsm.change(doorId, "nope") && logged("State machine \"door\" (entity 1): unknown state \"nope\"; staying in \"closed\""),
          "fsm: an unknown target state is denied with a warning");
    check(!fsm.change(doorId, "closed") && fsm.state(doorId) == "closed", "fsm: denied self-transition");
    check(fsm.change(doorId, "locked") && fsm.change(doorId, "locked") && fsm.state(doorId) == "locked",
          "fsm: omitted allowedFrom allows any state, itself included");
    check(fsm.change(doorId, "broken") && fsm.change(doorId, "broken") && !fsm.change(doorId, "closed"),
          "fsm: \"*\" allows any state, itself included");
    check(countLogged("fsm1 enter door") == enters + 5 && countLogged("fsm1 exit door") == exits + 4,
          "fsm: denied transitions run no callbacks");
    lua.tick(0.0f);
    check(fsmEvents(lua).size() == 6, "fsm: denied transitions publish nothing (6 allowed so far)");

    // Queueing: b.enter asks for c (queued), c.enter asks for d (denied when applied).
    check(fsm.change("game", "b") && fsm.state("game") == "c", "fsm: a change queued in enter is applied after it");
    check(logIndex("fsm1 game enter b") < logIndex("fsm1 game queued true still b") &&
              logIndex("fsm1 game queued true still b") < logIndex("fsm1 game exit b") &&
              logIndex("fsm1 game exit b") < logIndex("fsm1 game enter c"),
          "fsm: queued changes return true and wait for the current transition (no recursion)");
    check(logged("State machine \"game\": \"d\" can't be entered from \"c\""), "fsm: a queued change denied when applied warns");
    lua.tick(0.0f);
    events = fsmEvents(lua);
    check(events.size() == 8 && nlohmann::json::parse(events[7]->payload)["entity"].is_null() &&
              nlohmann::json::parse(events[6]->payload)["to"] == "b" && nlohmann::json::parse(events[7]->payload)["to"] == "c",
          "fsm: game machine events in order, entity null");

    // A callback error is logged with file:line and a traceback; the transition commits.
    check(fsm.change("game", "err") && fsm.state("game") == "err", "fsm: a failing enter still commits");
    check(logged("State machine fsms/game.lua: enter \"err\" failed: fsms/game.lua:15: fsm1 boom") &&
              logged("stack traceback:"),
          "fsm: callback errors are logged with file:line and a traceback");
    check(fsm.change("game", "a") && fsm.state("game") == "a", "fsm: the machine keeps working after a callback error");

    check(throwsInvalid([&] { fsm.state("nope"); }, "no game state machine \"nope\"") &&
              throwsInvalid([&] { fsm.change("ghost", "a"); }, "\"ghost\" has no valid definition") &&
              throwsInvalid([&] { fsm.canChange(kast::EntityId{999}, "a"); }, "invalid entity id 999") &&
              throwsInvalid([&] { fsm.states(plainId); }, "has no \"StateMachine\""),
          "fsm: C++ misuse throws std::invalid_argument");

    scene.destroy(gate);
    const ptrdiff_t exitsBefore = countLogged("fsm1 exit");
    lua.tick(0.0f);
    check(throwsInvalid([&] { fsm.state(gateId); }) && countLogged("fsm1 exit") == exitsBefore,
          "fsm: a destroyed entity's machine goes away without exit");
    lua.stop();
    check(throwsInvalid([&] { fsm.state("game"); }, "no Play session"), "fsm: Stop resets the machines");
}

// kast.fsm.* from Lua: results, misuse raising at the calling line, read-only table; the C++ API gives the same results.
void checkLuaStateMachineApi()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_lua", "fsm2");
    writeFile(dir / "scripts/probe.lua", R"(local door, plain
local function try(f) local ok, err = pcall(f); kast.log("fsm2 rej " .. tostring(ok) .. " " .. tostring(err)) end
return {
    init = function()
        for e in kast.query("StateMachine") do door = e end
        for e in kast.query("Transform") do if not kast.has(e, "StateMachine") then plain = e end end
        kast.log("fsm2 init game=" .. kast.fsm.state("game"))
    end,
    update = function(dt)
        try(function() kast.fsm.state("nope") end)
        try(function() kast.fsm.state(999) end)
        try(function() kast.fsm.state(plain) end)
        try(function() kast.fsm.change(door, 5) end)
        try(function() kast.fsm.state(true) end)
        try(function() kast.fsm.canChange("ghost", "a") end)
        try(function() kast.fsm.states(1.5) end)
        try(function() kast.fsm.change("game") end)
        assert(type(kast.fsm.state) == "function" and not pcall(function() kast.fsm.state = nil end), "kast.fsm is read-only")
        local r = {}
        local function rec(v) r[#r + 1] = tostring(v) end
        rec(kast.fsm.state(door)); rec(kast.fsm.canChange(door, "open")); rec(kast.fsm.canChange(door, "closed"))
        rec(kast.fsm.change(door, "open")); rec(kast.fsm.change(door, "open")); rec(kast.fsm.change(door, "nope"))
        rec(kast.fsm.change(door, "locked")); rec(kast.fsm.change(door, "locked")); rec(kast.fsm.change(door, "closed"))
        rec(kast.fsm.state(door)); rec(table.concat(kast.fsm.states(door), "/")); rec(kast.fsm.state("game"))
        rec(kast.fsm.change("game", "b")); rec(kast.fsm.state("game")); rec(kast.fsm.canChange("game", "a"))
        kast.log("fsm2 results " .. table.concat(r, ","))
    end,
})");
    const auto makeScene = [](kast::Scene& scene) {
        scene.create("Plain");
        scene.registry().emplace<kast::StateMachine>(scene.create("Door"), "door", "closed");
    };
    kast::Scene scene;
    makeScene(scene);
    scene.systems = {"scripts/probe.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr, {"game", "ghost"});
    check(logIndex("fsm2 game enter a from=nil") < logIndex("fsm2 init game=a") && countLogged("fsm2 game enter a") == 1,
          "fsm: a game machine touched during init enters then (and only once)");
    lua.tick(0.1f);
    checkOk(lua, 0, "fsm: kast.fsm rejections are catchable; kast.fsm is read-only");
    const std::pair<int, const char*> cases[] = {
        {10, "kast.fsm.state: no game state machine \"nope\""},
        {11, "kast.fsm.state: invalid entity id 999"},
        {12, "kast.fsm.state: entity 1 has no \"StateMachine\""},
        {13, "kast.fsm.change: state must be a string, got 5"},
        {14, "kast.fsm.state: target must be a state machine name or an entity id, got true"},
        {15, "kast.fsm.canChange: game state machine \"ghost\" has no valid definition"},
        {16, "kast.fsm.states: invalid entity id 1.5"},
        {17, "kast.fsm.change: state must be a string, got nil"},
    };
    for (const auto& [line, message] : cases)
        check(logged(std::format("fsm2 rej false scripts/probe.lua:{}: {}", line, message)),
              std::format("fsm: raised at probe.lua:{}: {}", line, message).c_str());

    // The same operations through the C++ API, on a fresh session.
    kast::Scene scene2;
    makeScene(scene2);
    kast::LuaSystems cpp;
    cpp.start(scene2, dir, "", nullptr, {"game"});
    kast::StateMachines& fsm = cpp.fsm();
    const kast::EntityId door = 2;
    std::vector<std::string> r;
    const auto rec = [&](auto value) {
        if constexpr (std::is_same_v<decltype(value), bool>)
            r.push_back(value ? "true" : "false");
        else
            r.push_back(value);
    };
    rec(fsm.state(door)); rec(fsm.canChange(door, "open")); rec(fsm.canChange(door, "closed"));
    rec(fsm.change(door, "open")); rec(fsm.change(door, "open")); rec(fsm.change(door, "nope"));
    rec(fsm.change(door, "locked")); rec(fsm.change(door, "locked")); rec(fsm.change(door, "closed"));
    std::string states;
    for (const std::string& s : fsm.states(door))
        states += (states.empty() ? "" : "/") + s;
    rec(fsm.state(door)); rec(states); rec(fsm.state("game"));
    rec(fsm.change("game", "b")); rec(fsm.state("game")); rec(fsm.canChange("game", "a"));
    std::string joined;
    for (const std::string& s : r)
        joined += (joined.empty() ? "" : ",") + s;
    check(joined == "closed,true,false,true,false,false,true,true,false,locked,broken/closed/locked/open,a,true,c,false",
          std::format("fsm: expected results ({})", joined).c_str());
    check(logged("fsm2 results " + joined), "fsm: the C++ API gives the same results as kast.fsm");
    cpp.stop();
    lua.stop();
}

// Game machines keep their state across kast.loadScene (no callbacks; the new Lua state's callbacks are used after),
// entity machines of the new scene enter after its inits, and Stop then Play starts over.
void checkStateMachineSceneSwitch()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_switch", "fsm3");
    saveTestScene(dir / "a.scene", "A", {"scripts/a.lua"});
    {
        kast::Scene b;
        b.registry().emplace<kast::StateMachine>(b.create("Door"), "door", "open");
        b.systems = {"scripts/b.lua"};
        std::string error;
        check(kast::saveScene(b, (dir / "b.scene").string(), error), "fsm: write b.scene");
    }
    writeFile(dir / "scripts/a.lua", R"(return { update = function(dt)
    kast.fsm.change("game", "b")
    kast.loadScene("b.scene")
end })");
    writeFile(dir / "scripts/b.lua", R"(local frame = 0
return {
    init = function() kast.log("fsm3 b init game=" .. kast.fsm.state("game")) end,
    update = function(dt)
        frame = frame + 1
        if frame == 1 then kast.fsm.change("game", "err") end
    end,
})");
    kast::Scene scene;
    std::string error;
    check(kast::loadScene((dir / "a.scene").string(), scene, error), "fsm: load a.scene");
    kast::LuaSystems lua;
    lua.start(scene, dir, "a.scene", nullptr, {"game"});
    check(lua.fsm().state("game") == "a" && countLogged("fsm3 game enter") == 1, "fsm: game machine entered at start");
    check(lua.tick(0.1f), "fsm: switched to b.scene");
    check(logged("fsm3 b init game=c") && countLogged("fsm3 game enter") == 3,
          "fsm: game state kept across kast.loadScene, with no callbacks on the switch");
    check(logIndex("fsm3 b init game=c") < logIndex("fsm3 enter door from=nil to=open"),
          "fsm: the new scene's entity machines enter after its inits");
    lua.tick(0.1f);
    check(logged("fsm3 boom") && lua.fsm().state("game") == "err",
          "fsm: after a switch the game machine's callbacks come from the new Lua state");
    lua.stop();
    check(lua.fsm().peek("game") == std::nullopt, "fsm: Stop resets game machines");
    kast::Scene again;
    check(kast::loadScene((dir / "a.scene").string(), again, error), "fsm: reload a.scene");
    lua.start(again, dir, "a.scene", nullptr, {"game"});
    check(lua.fsm().state("game") == "a" && countLogged("fsm3 game enter a from=nil") == 2,
          "fsm: the next Play starts from the initial state again");
    lua.stop();
}

// Entity machines added at runtime (kast.add + kast.set, a registry edit, Duplicate), destroyed, removed or naming a
// missing definition.
void checkEntityStateMachines()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_entities", "fsm4");
    writeFile(dir / "scripts/r.lua", R"(local frame, made = 0, nil
return { update = function(dt)
    frame = frame + 1
    if frame == 1 then
        made = kast.create("Made")
        kast.add(made, "StateMachine")
        local ok, err = pcall(function() local s = kast.fsm.state(made); return s end)
        kast.log("fsm4 empty " .. tostring(ok) .. " " .. tostring(err))
        kast.set(made, "StateMachine", {machine = "door", initialState = "locked"})
        kast.log("fsm4 made " .. kast.fsm.state(made))
        kast.set(made, "StateMachine", {initialState = "open"})
        kast.log("fsm4 still " .. kast.fsm.state(made))
    elseif frame == 2 then
        kast.destroy(made)
    end
end })");
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity gate = scene.create("Gate");
    registry.emplace<kast::StateMachine>(gate, "door", "open");
    const entt::entity later = scene.create("Later");
    const entt::entity missing = scene.create("Missing");
    scene.systems = {"scripts/r.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr, {});
    lua.tick(0.1f);
    checkOk(lua, 0, "fsm: runtime StateMachine script runs");
    check(logged("fsm4 empty false scripts/r.lua:7: kast.fsm.state: entity 4: StateMachine has no \"machine\""),
          "fsm: a StateMachine with no machine yet raises on use");
    check(logged("fsm4 made locked") && logged("fsm4 enter door from=nil to=locked entity=4") && logged("fsm4 still locked"),
          "fsm: kast.add + kast.set machine enters initialState; changing only initialState doesn't re-enter");
    lua.tick(0.1f);
    check(!logged("fsm4 exit door from=locked"), "fsm: destroying an entity runs no exit");

    registry.emplace<kast::StateMachine>(later, "door", ""); // an Inspector edit during Play
    lua.tick(0.1f);
    check(lua.fsm().peek(idOf(scene, later)) == "closed", "fsm: an entity that gains StateMachine enters on the next tick");
    lua.fsm().change(idOf(scene, gate), "locked");
    const entt::entity copy = scene.duplicate(gate);
    lua.tick(0.1f);
    check(lua.fsm().peek(idOf(scene, copy)) == "open" && lua.fsm().peek(idOf(scene, gate)) == "locked",
          "fsm: a duplicate enters its own initialState");
    registry.emplace<kast::StateMachine>(missing, "nosuch", "");
    lua.tick(0.1f);
    lua.tick(0.1f);
    check(countLogged("Entity 'Missing' (id 3): StateMachine: no valid state machine definition \"nosuch\"") == 1,
          "fsm: a missing definition is logged once");
    check(throwsInvalid([&] { lua.fsm().state(idOf(scene, missing)); }, "no valid state machine definition \"nosuch\""),
          "fsm: a missing definition raises on use");
    registry.remove<kast::StateMachine>(later);
    lua.tick(0.1f);
    check(!registry.all_of<kast::StateMachineRuntime>(later), "fsm: removing StateMachine forgets the machine");
    lua.stop();
}

// An exit that swaps or removes the entity's StateMachine ends the transition; a stale runtime survives a reload.
void checkStateMachineExitChanges()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_exit_changes", "fsm7");
    writeFile(dir / "fsms/swap.lua", R"(return { name = "swap", initial = "a", states = {
    a = { exit = function(ctx) kast.set(ctx.entity, "StateMachine", {machine = "door"}) end },
    b = { enter = function(ctx) kast.log("fsm7 swap entered b") end } } })");
    writeFile(dir / "fsms/drop.lua", R"(return { name = "drop", initial = "a", states = {
    a = { exit = function(ctx) kast.remove(ctx.entity, "StateMachine") end },
    b = { enter = function(ctx) kast.log("fsm7 drop entered b") end } } })");
    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity swap = scene.create("Swap");
    registry.emplace<kast::StateMachine>(swap, "swap", "");
    const entt::entity drop = scene.create("Drop");
    registry.emplace<kast::StateMachine>(drop, "drop", "");
    const entt::entity door = scene.create("Door");
    registry.emplace<kast::StateMachine>(door, "door", "");
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr, {});
    kast::StateMachines& fsm = lua.fsm();
    fsm.change(idOf(scene, swap), "b");
    check(!logged("fsm7 swap entered b") && fsm.state(idOf(scene, swap)) == "closed" &&
              registry.get<kast::StateMachineRuntime>(swap).machine == "door",
          "fsm: an exit that swaps the machine keeps the new machine's state, no enter of the old target");
    fsm.change(idOf(scene, drop), "b");
    check(!logged("fsm7 drop entered b") && registry.get<kast::StateMachineRuntime>(drop).current == "a",
          "fsm: an exit that removes StateMachine stops the transition");

    fsm.change(idOf(scene, door), "open");
    registry.remove<kast::StateMachine>(door); // as kast.remove: the runtime stays until the next syncAll
    kast::StateMachineDef def{.name = "door", .path = "fsms/sub/door.lua", .initial = "closed"};
    def.states["closed"] = {};
    fsm.define(std::move(def)); // "open" is gone
    check(registry.get<kast::StateMachineRuntime>(door).current == "open",
          "fsm: a reload skips a runtime whose StateMachine was removed");
    lua.stop();
}

// Runaway scripts are stopped by the watchdog (2 s budget each) instead of freezing the editor.
void checkLuaWatchdog()
{
    const std::filesystem::path dir = fsmProject("kast_lua_watchdog", "dog");
    writeFile(dir / "fsms/spin.lua", R"(return { name = "spin", initial = "a", states = {
    a = {}, b = { enter = function(ctx) while true do end end } } })");
    writeFile(dir / "scripts/loop.lua", R"(return { update = function(dt)
    while true do pcall(function() while true do end end) end
end })");
    writeFile(dir / "scripts/after.lua", R"(return { update = function(dt) kast.log("dog after ran") end })");
    kast::Scene scene;
    const entt::entity spin = scene.create("Spin");
    scene.registry().emplace<kast::StateMachine>(spin, "spin", "");
    scene.systems = {"scripts/loop.lua", "scripts/after.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr, {});
    const auto start = std::chrono::steady_clock::now();
    lua.tick(0.1f);
    const kast::LuaSystemStatus loop = statusOf(lua, 0);
    check(!loop.ok && contains(loop.error, "scripts/loop.lua:2: script ran longer than 2 seconds") &&
              logged("dog after ran") && statusOf(lua, 1).ok,
          "watchdog: an endless update (even inside pcall) disables its system; the others run");
    lua.fsm().change(idOf(scene, spin), "b");
    check(logged("State machine fsms/spin.lua: enter \"b\" failed: fsms/spin.lua:2: script ran longer than 2 seconds"),
          "watchdog: an endless state machine callback is stopped and logged");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(6), "watchdog: stops each within its budget");
    lua.stop();

    writeFile(dir / "fsms/hang.lua", "while true do end");
    const kast::StateMachineDefinitions defs = kast::loadStateMachineDefinitions(dir); // the editor's scan
    check(std::ranges::any_of(defs.status, [](const kast::ComponentDefinitionStatus& s) {
              return !s.ok && contains(s.error, "fsms/hang.lua:1: script ran longer than 2 seconds");
          }) && defs.valid.size() == 3,
          "watchdog: an endless definition file is rejected; the others load");
}

// A Lua component definition with an endless loop is stopped by the same watchdog instead of freezing the editor.
void checkComponentWatchdog()
{
    const std::filesystem::path dir = componentProject("kast_components_watchdog");
    writeFile(dir / "components/spin.lua", "while true do end\nreturn { name = \"Spin\", fields = {} }");
    const auto start = std::chrono::steady_clock::now();
    const kast::ComponentDefinitions defs = kast::loadComponentDefinitions(dir);
    const auto spin = std::ranges::find(defs.status, std::string("components/spin.lua"), &kast::ComponentDefinitionStatus::path);
    check(spin != defs.status.end() && !spin->ok &&
              contains(spin->error, "components/spin.lua:1: script ran longer than 2 seconds") && defs.valid.size() == 1,
          "watchdog: an endless component definition is rejected; the others load");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(4), "watchdog: stops it within its budget");

    const std::filesystem::path fsmDir = fsmProject("kast_fsm_watchdog", "dog");
    writeFile(fsmDir / "fsms/spin.lua", "while true do end");
    const auto fsmStart = std::chrono::steady_clock::now();
    const kast::StateMachineDefinitions fsms = kast::loadStateMachineDefinitions(fsmDir); // the editor's scan
    check(std::ranges::any_of(fsms.status, [](const kast::ComponentDefinitionStatus& s) {
              return s.path == "fsms/spin.lua" && !s.ok && contains(s.error, "fsms/spin.lua:1: script ran longer than 2 seconds");
          }) && std::chrono::steady_clock::now() - fsmStart < std::chrono::seconds(4),
          "watchdog: an endless state machine definition is rejected by the editor's scan within its budget");

    // Reading the returned table runs no Lua: one with a metatable (here an endless __index) is rejected unread.
    const char* endlessIndex = "setmetatable(t, { __index = function() while true do end end })";
    writeFile(dir / "components/meta.lua", std::format("local t = {{ fields = {{}} }}\n{}\nreturn t", endlessIndex));
    writeFile(fsmDir / "fsms/meta.lua",
              std::format("local t = {{ initial = \"a\", states = {{ a = {{}} }} }}\n{}\nreturn t", endlessIndex));
    std::filesystem::remove(dir / "components/spin.lua");
    std::filesystem::remove(fsmDir / "fsms/spin.lua");
    const auto metaStart = std::chrono::steady_clock::now();
    const kast::ComponentDefinitions metaDefs = kast::loadComponentDefinitions(dir);
    const kast::StateMachineDefinitions metaFsms = kast::loadStateMachineDefinitions(fsmDir);
    const auto endless = [](const std::vector<kast::ComponentDefinitionStatus>& status, const std::string& path) {
        return std::ranges::any_of(status, [&](const kast::ComponentDefinitionStatus& s) {
            return s.path == path && !s.ok && contains(s.error, path + ": tables must be plain data (no metatables)");
        });
    };
    check(endless(metaDefs.status, "components/meta.lua") && endless(metaFsms.status, "fsms/meta.lua") &&
              std::chrono::steady_clock::now() - metaStart < std::chrono::seconds(1),
          "definitions: a returned table with a metatable (endless __index) is rejected without running it");
}

// Hot reload during Play: kept state, reset when the state is gone, last valid definition kept when broken.
void checkStateMachineHotReload()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_reload", "fsm5");
    kast::Scene scene;
    const entt::entity door = scene.create("Door");
    scene.registry().emplace<kast::StateMachine>(door, "door", "closed");
    const kast::EntityId id = idOf(scene, door);
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr, {"game"});
    kast::StateMachines& fsm = lua.fsm();
    fsm.change(id, "open");
    lua.tick(0.1f);

    const auto rewriteDoor = [&](const std::string& tag) {
        const auto before = std::filesystem::last_write_time(dir / "fsms/sub/door.lua");
        writeFsmDefinitions(dir, "fsm5", tag);
        std::filesystem::last_write_time(dir / "fsms/sub/door.lua", before + std::chrono::seconds(2));
    };
    const ptrdiff_t enters = countLogged("fsm5 enter");
    rewriteDoor(" v2");
    lua.tick(0.1f);
    check(logged("Reloaded state machine definitions (2 OK, 0 rejected)") && fsm.state(id) == "open" &&
              countLogged("fsm5 enter") == enters,
          "fsm: a reloaded definition keeps an existing state, running no callbacks");
    check(fsm.change(id, "locked") && logged("fsm5 v2 enter door from=open to=locked"), "fsm: reloaded callbacks are used");

    // v3: no "locked" state.
    const auto before = std::filesystem::last_write_time(dir / "fsms/sub/door.lua");
    std::string text = readText(dir / "fsms/sub/door.lua");
    text.replace(text.find("        locked = "), text.find("        broken = ") - text.find("        locked = "), "");
    for (size_t at; (at = text.find(" v2")) != std::string::npos;)
        text.replace(at, 3, " v3");
    text.replace(text.find("{\"closed\", \"locked\"}"), 20, "{\"closed\"}");
    writeFile(dir / "fsms/sub/door.lua", text);
    std::filesystem::last_write_time(dir / "fsms/sub/door.lua", before + std::chrono::seconds(4));
    lua.tick(0.1f);
    check(logged("Entity 'Door' (id 1): state machine \"door\": state \"locked\" no longer exists; reset to \"closed\"") &&
              logged("fsm5 v3 enter door from=nil to=closed") && fsm.state(id) == "closed",
          "fsm: a removed current state resets to initialState with a warning, enter with no from");

    rewrite(dir / "fsms/sub/door.lua", "return {\n  name = \n");
    lua.tick(0.1f);
    check(logged("State machine rejected: fsms/sub/door.lua:") &&
              logged("Reloaded state machine definitions (1 OK, 1 rejected)"),
          "fsm: a broken definition is logged");
    check(fsm.state(id) == "closed" && fsm.change(id, "open") && logged("fsm5 v3 enter door from=closed to=open") &&
              !fsm.canChange(id, "locked"),
          "fsm: machines keep their last valid definition while the file is broken");

    // Game machine: its current state "a" removed.
    const auto gameBefore = std::filesystem::last_write_time(dir / "fsms/game.lua");
    writeFile(dir / "fsms/game.lua", R"(return { name = "game", initial = "z", states = {
    z = { enter = function(ctx) kast.log("fsm5 game z from=" .. tostring(ctx.from)) end }, b = {} } })");
    std::filesystem::last_write_time(dir / "fsms/game.lua", gameBefore + std::chrono::seconds(2));
    lua.tick(0.1f);
    check(logged("State machine \"game\": state \"a\" no longer exists; reset to \"z\"") && logged("fsm5 game z from=nil") &&
              fsm.state("game") == "z",
          "fsm: game machines follow the same hot reload rules");
    lua.stop();
}

// StateMachine data: scene round trip, Duplicate, Play/Stop restore; project.kast "stateMachines".
void checkStateMachineFiles()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_files", "fsm6");
    kast::Scene scene;
    const entt::entity door = scene.create("Door");
    scene.registry().emplace<kast::StateMachine>(door, "door", "open");
    scene.registry().emplace<kast::StateMachine>(scene.create("Lost"), "gone_machine", "x"); // no definition: kept
    std::string error;
    const std::string first = (dir / "first.scene").string(), second = (dir / "second.scene").string();
    kast::Scene loaded;
    check(kast::saveScene(scene, first, error) && kast::loadScene(first, loaded, error) &&
              kast::saveScene(loaded, second, error) && readText(first) == readText(second),
          "fsm: StateMachine save/load is byte-identical");
    const nlohmann::json saved = nlohmann::json::parse(readText(first));
    check(saved["entities"][0]["components"]["StateMachine"] ==
                  nlohmann::json{{"machine", "door"}, {"initialState", "open"}} &&
              saved["entities"][1]["components"]["StateMachine"]["machine"] == "gone_machine",
          "fsm: StateMachine scene JSON; a missing definition's data is kept");
    const entt::entity copy = scene.duplicate(door);
    const auto& copied = scene.registry().get<kast::StateMachine>(copy);
    check(copied.machine == "door" && copied.initialState == "open", "fsm: Duplicate copies StateMachine");

    Document doc;
    check(openProject(doc, dir / kast::kProjectFileName, error), "fsm: open the test project");
    doc.scene.registry().emplace<kast::StateMachine>(doc.scene.create("Door"), "door", "closed");
    const nlohmann::ordered_json before = kast::sceneToJson(doc.scene);
    startPlay(doc);
    updatePlay(doc, 0.1f);
    for (auto [entity, machine] : doc.scene.registry().view<kast::StateMachine>().each())
        machine.machine = "changed";
    stopPlay(doc);
    check(kast::sceneToJson(doc.scene) == before, "fsm: Stop restores StateMachine");

    // project.kast
    const std::filesystem::path file = dir / kast::kProjectFileName;
    kast::Project project;
    check(kast::loadProject(file, project, error) && kast::saveProject(project, error), "fsm: normalize project.kast");
    const std::string plain = readText(file);
    check(kast::loadProject(file, project, error) && !project.stateMachines && kast::saveProject(project, error) &&
              readText(file) == plain && !contains(plain, "stateMachines"),
          "fsm: project.kast without stateMachines round-trips byte-identical");
    const std::string listed = "{\n  \"version\": 1,\n  \"name\": \"FSM\",\n  \"startScene\": null,\n"
                               "  \"stateMachines\": [\n    \"game\",\n    \"door\"\n  ]\n}\n";
    writeFile(file, listed);
    check(kast::loadProject(file, project, error) && project.stateMachines == std::vector<std::string>{"game", "door"} &&
              kast::saveProject(project, error) && readText(file) == listed,
          "fsm: project.kast stateMachines round-trips byte-identical, in key order");
    const std::pair<const char*, const char*> invalid[] = {
        {R"("x")", "\"stateMachines\" must be an array of state machine names"},
        {"[1]", "\"stateMachines\" must hold non-empty strings"},
        {R"([""])", "\"stateMachines\" must hold non-empty strings"},
        {R"(["a", "a"])", "\"stateMachines\" lists \"a\" twice"},
    };
    for (const auto& [value, message] : invalid) {
        writeFile(file, std::format(R"({{"version": 1, "name": "FSM", "startScene": null, "stateMachines": {}}})", value));
        check(!kast::loadProject(file, project, error) && contains(error, message),
              std::format("fsm: project.kast rejects stateMachines {}", value).c_str());
    }
}

// New State Machine naming and template, the Global toggle (project.kast) and their Play blocking.
void checkStateMachineEditor()
{
    const std::filesystem::path dir = fsmProject("kast_fsm_editor", "fsm7");
    writeFile(dir / "fsms/Odd.lua", R"(return { name = "odd_one", initial = "a", states = { a = {} } })");
    Document doc;
    std::string error;
    check(openProject(doc, dir / kast::kProjectFileName, error), "fsm: open the editor test project");
    refreshStateMachines(doc);
    check(newStateMachineNameError(doc, "  ") == "Enter a name" &&
              newStateMachineNameError(doc, "1door") == "Use letters, digits and _ only, not starting with a digit" &&
              newStateMachineNameError(doc, "door") == "A state machine named \"door\" already exists" &&
              newStateMachineNameError(doc, "Odd") == "fsms/Odd.lua already exists" &&
              newStateMachineNameError(doc, " TrafficLight ").empty(),
          "fsm: New State Machine name rules");
    check(stateMachineFile(doc.project->root, "TrafficLight") == dir / "fsms/traffic_light.lua",
          "fsm: definition file is fsms/<snake_name>.lua");
    check(newStateMachine(doc, " TrafficLight ", error), "fsm: New State Machine creates the file");
    const kast::StateMachineDefinitions& defs = kast::stateMachineDefinitions();
    const auto created = std::ranges::find(defs.valid, "TrafficLight", &kast::StateMachineDef::name);
    check(created != defs.valid.end() && created->path == "fsms/traffic_light.lua" && created->initial == "idle" &&
              created->stateNames() == std::vector<std::string>{"active", "idle"},
          "fsm: the template is a valid definition named as asked");
    const std::string text = readText(dir / "fsms/traffic_light.lua");
    check(!newStateMachine(doc, "TrafficLight", error) && contains(error, "already exists") &&
              readText(dir / "fsms/traffic_light.lua") == text,
          "fsm: New State Machine never overwrites");

    const std::string file = (dir / kast::kProjectFileName).string();
    check(!isGlobalStateMachine(doc, "door") && setGlobalStateMachine(doc, "door", true, error) &&
              isGlobalStateMachine(doc, "door") &&
              readText(file) == "{\n  \"version\": 1,\n  \"name\": \"FSM\",\n  \"startScene\": null,\n"
                                "  \"stateMachines\": [\n    \"door\"\n  ]\n}\n",
          "fsm: Global adds the machine to project.kast (other keys and order kept)");
    check(setGlobalStateMachine(doc, "door", false, error) && !isGlobalStateMachine(doc, "door") &&
              contains(readText(file), "\"stateMachines\": []"),
          "fsm: unchecking Global removes it");

    doc.play = PlayState::Playing;
    check(newStateMachineNameError(doc, "Fresh") == "Stop Play to change state machines" &&
              !setGlobalStateMachine(doc, "door", true, error) && !newStateMachine(doc, "Fresh", error) &&
              !std::filesystem::exists(dir / "fsms/fresh.lua"),
          "fsm: New State Machine and Global are blocked during Play");
    doc.play = PlayState::Stopped;
}

std::vector<std::string> assetNames(const std::vector<AssetEntry>& entries)
{
    std::vector<std::string> names;
    for (const AssetEntry& entry : entries)
        names.push_back(entry.name + (entry.folder ? "/" : ""));
    return names;
}

bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    return a.lexically_normal() == b.lexically_normal();
}

// The names in dir exactly as stored on disk (case included), sorted.
std::vector<std::string> diskNames(const std::filesystem::path& dir)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        names.push_back(utf8(entry.path().filename()));
    std::ranges::sort(names);
    return names;
}

void checkAssetScan()
{
    const std::filesystem::path root = projectTestDir("kast_assets_scan", "Assets"); // project.kast, empty scripts/
    for (const char* dir : {"Zeta", "alpha/Empty", ".kast", "out/game", "fsms/.hidden"})
        std::filesystem::create_directories(root / dir);
    for (const char* file : {"b.scene", "A.lua", ".gitignore", "alpha/z.txt", "fsms/m.lua", "fsms/.hidden/x.lua", "out/game.pak"})
        writeFile(root / file, "");
    writeFile(root / u8"\u00e9t\u00e9.lua", ""); // non-ASCII name

    std::string error;
    check(saveBuildFolder(root, root / "out", error), "save an in-project build folder");
    std::vector<AssetEntry> entries = scanAssets(root);
    const std::vector<std::string> expected = {"alpha/", "fsms/", "scripts/", "Zeta/", "A.lua", "b.scene", "\xc3\xa9t\xc3\xa9.lua"};
    check(assetNames(entries) == expected,
          "scanAssets: folders first, A-Z ignoring case; dot-entries, project.kast and the build folder hidden");
    if (assetNames(entries) != expected)
        for (const std::string& name : assetNames(entries))
            kast::log::error(std::format("  listed '{}'", name));
    const AssetEntry* alpha = findAsset(entries, "alpha");
    check(alpha && alpha->folder && assetNames(alpha->children) == std::vector<std::string>{"Empty/", "z.txt"},
          "scanAssets lists an empty folder before a file");
    const AssetEntry* empty = findAsset(entries, "alpha/Empty");
    check(empty && empty->folder && empty->children.empty() && empty->path == "alpha/Empty",
          "an empty folder has a project-relative path and no children");
    check(findAsset(entries, "fsms/m.lua") && !findAsset(entries, "fsms/.hidden") && !findAsset(entries, "fsms/.hidden/x.lua"),
          "a dot-folder and its contents are hidden");
    check(!findAsset(entries, "missing") && !findAsset(entries, "alpha/missing"), "findAsset: no such entry");

    check(saveBuildFolder(root, root / "alpha", error) && findAsset(scanAssets(root), "alpha/z.txt") != nullptr,
          "a remembered in-project folder without game.pak (not a build) hides nothing");
    check(saveBuildFolder(root, root.parent_path() / "kast_assets_out", error), "save a build folder outside");
    check(findAsset(scanAssets(root), "out") != nullptr, "a build folder outside the project hides nothing");
    check(saveBuildFolder(root, root, error) && findAsset(scanAssets(root), "alpha") != nullptr,
          "a build folder equal to the project root hides nothing");
}

void checkAssetNames()
{
    for (const char* bad : {"", ".git", ".", "a<b", "a>b", "a:b", "a\"b", "a/b", "a\\b", "a|b", "a?b", "a*b", "a\tb",
                            "a\x7f", "name ", "name.", "CON", "con.txt", "Com1", "LPT9.lua", "nul", "aux.tar.gz"})
        check(!assetNameError(bad).empty(), std::format("assetNameError refuses '{}'", bad).c_str());
    for (const char* good : {"health.lua", "New Folder", "CONSOLE", "a.b.c", " lead", "COM10", "\xc3\xa9t\xc3\xa9"})
        check(assetNameError(good).empty(), std::format("assetNameError accepts '{}'", good).c_str());

    const std::filesystem::path dir = looseTestDir("kast_assets_names");
    check(uniqueFolderName(dir) == "New Folder", "uniqueFolderName: New Folder first");
    std::filesystem::create_directory(dir / "new folder"); // taken, ignoring case
    check(uniqueFolderName(dir) == "New Folder 2", "uniqueFolderName: New Folder 2 next");
    writeFile(dir / "New Folder 2", ""); // a file takes the name too
    check(uniqueFolderName(dir) == "New Folder 3", "uniqueFolderName: New Folder 3 next");

    std::string error;
    check(createAssetFolder(dir, uniqueFolderName(dir), error) && std::filesystem::is_directory(dir / "New Folder 3"),
          "createAssetFolder creates the folder");
    check(!createAssetFolder(dir, "NEW FOLDER 3", error) && contains(error, "already exists"),
          "createAssetFolder refuses a taken name (ignoring case)");
    check(!createAssetFolder(dir, "bad?", error) && !std::filesystem::exists(dir / "bad?"),
          "createAssetFolder refuses an invalid name");
}

void checkAssetPaths()
{
    const std::filesystem::path root = testRoot() / "kast_assets_paths";
    check(isSameOrInside(root / "a" / "b", root / "a"), "isSameOrInside: a child");
    check(isSameOrInside(root / "a", root / "a"), "isSameOrInside: itself");
    check(isSameOrInside(root / "A" / "x", root / "a"), "isSameOrInside ignores case");
    check(isSameOrInside(root / "a" / "b", (root / "a").string() + "\\"), "isSameOrInside: trailing separator");
    check(!isSameOrInside(root / "ab", root / "a"), "isSameOrInside: a sibling sharing a prefix is not inside");
    check(!isSameOrInside(root / "a", root / "a" / "b"), "isSameOrInside: a parent is not inside its child");

    const std::filesystem::path scene = root / "lvl" / "a.scene";
    const std::optional<std::filesystem::path> renamed = remapPath(scene, scene, root / "lvl" / "b.scene");
    check(renamed && sameFile(*renamed, root / "lvl" / "b.scene"), "remapPath follows a renamed scene");
    const std::optional<std::filesystem::path> moved = remapPath(scene, root / "lvl", root / "x" / "lvl");
    check(moved && sameFile(*moved, root / "x" / "lvl" / "a.scene"), "remapPath follows a moved parent folder");
    const std::optional<std::filesystem::path> folded = remapPath(root / "LVL" / "a.scene", root / "lvl", root / "w");
    check(folded && sameFile(*folded, root / "w" / "a.scene"), "remapPath ignores case");
    check(!remapPath(scene, root / "other", root / "x"), "remapPath: an unrelated path is unchanged");
    check(!remapPath(scene, root / "lvl" / "a", root / "x"), "remapPath: a name prefix is not a parent");

    check(!deleteBlocker(scene, scene).empty(), "deleteBlocker refuses the open scene");
    check(!deleteBlocker(root / "lvl", scene).empty(), "deleteBlocker refuses a folder holding the open scene");
    check(!deleteBlocker(root, scene).empty(), "deleteBlocker refuses an ancestor of the open scene");
    check(deleteBlocker(root / "lvl" / "b.scene", scene).empty(), "deleteBlocker allows another file");
    check(deleteBlocker(scene, {}).empty(), "deleteBlocker allows anything with no saved scene");
}

void checkAssetMoves()
{
    const std::filesystem::path root = looseTestDir("kast_assets_moves");
    std::filesystem::create_directories(root / "r");
    writeFile(root / "r" / "one.lua", "one");
    writeFile(root / "r" / "two.lua", "two");
    std::string error;

    check(!renameAsset(root / "r" / "one.lua", "TWO.lua", error) && contains(error, "already exists"),
          "renameAsset refuses a taken name (ignoring case)");
    check(diskNames(root / "r") == std::vector<std::string>{"one.lua", "two.lua"} && readText(root / "r" / "two.lua") == "two",
          "a refused rename leaves the disk unchanged");
    for (const char* bad : {"", ".one.lua", "bad?.lua", "con.lua", "trail."})
        check(!renameAsset(root / "r" / "one.lua", bad, error) &&
                  diskNames(root / "r") == std::vector<std::string>{"one.lua", "two.lua"},
              std::format("renameAsset refuses '{}' and changes nothing", bad).c_str());
    check(renameAsset(root / "r" / "one.lua", "One.lua", error) &&
              diskNames(root / "r") == std::vector<std::string>{"One.lua", "two.lua"},
          "renameAsset allows a case-only rename");
    check(renameAsset(root / "r" / "One.lua", "three.lua", error) && readText(root / "r" / "three.lua") == "one",
          "renameAsset renames a file");
    check(renameAsset(root / "r", "renamed", error) && std::filesystem::exists(root / "renamed" / "two.lua"),
          "renameAsset renames a folder with its contents");

    std::filesystem::create_directories(root / "m" / "sub" / "deep");
    writeFile(root / "m" / "f.txt", "mine");
    writeFile(root / "m" / "sub" / "F.TXT", "theirs");
    check(!moveAsset(root / "m" / "f.txt", root / "m" / "sub", error) && contains(error, "already exists") &&
              readText(root / "m" / "f.txt") == "mine" && readText(root / "m" / "sub" / "F.TXT") == "theirs",
          "moveAsset refuses a taken name (ignoring case) and changes nothing");
    check(!moveAsset(root / "m", root / "m", error) && std::filesystem::exists(root / "m" / "f.txt"),
          "moveAsset refuses moving a folder into itself");
    check(!moveAsset(root / "m", root / "M" / "sub" / "deep", error) && std::filesystem::exists(root / "m" / "sub" / "deep"),
          "moveAsset refuses moving a folder into its descendant");
    check(moveAsset(root / "m" / "f.txt", root / "m", error) && readText(root / "m" / "f.txt") == "mine",
          "moveAsset onto its own folder is a no-op");
    check(moveAsset(root / "m" / "sub", root, error) && std::filesystem::exists(root / "sub" / "deep") &&
              !std::filesystem::exists(root / "m" / "sub"),
          "moveAsset moves a folder with its contents");
    check(moveAsset(root / "m" / "f.txt", root / "sub" / "deep", error) && readText(root / "sub" / "deep" / "f.txt") == "mine",
          "moveAsset moves a file");
    check(!moveAsset(root / "missing.txt", root / "sub", error) && !error.empty(), "moveAsset reports an OS failure");
}

void checkOpenSceneAction()
{
    const std::filesystem::path root = projectTestDir("kast_assets_open", "Open");
    saveTestScene(root / "a.scene", "A", {});
    saveTestScene(root / "b.scene", "B", {});
    Document doc;
    std::string error;
    check(openFile(doc, (root / "a.scene").string(), error), "open a.scene");
    FileActions files(doc);
    files.openScene((root / "b.scene").string());
    check(doc.path == (root / "b.scene").string(), "FileActions::openScene opens a clean document's scene right away");
    markDirty(doc);
    files.openScene((root / "a.scene").string());
    check(doc.path == (root / "b.scene").string() && doc.dirty,
          "FileActions::openScene waits on the unsaved-changes prompt when dirty");
}

// Assets panel Create > Lua System / State Machine: unique template files, the fsms/ rule, the first-rename machine
// rename and scene system paths following renames and moves.
void checkAssetCreate()
{
    std::string error;
    const std::filesystem::path loose = looseTestDir("kast_assets_create");
    std::filesystem::path file;
    check(writeNewSystem(loose / "logic", file, error) && file == loose / "logic/new_system.lua" &&
              contains(readText(file), "-- A Lua system: update(dt)"),
          "writeNewSystem writes new_system.lua from the template, creating its folder");
    const std::string systemText = readText(file);
    writeFile(loose / "logic/NEW_SYSTEM_2.lua", "keep");
    check(writeNewSystem(loose / "logic", file, error) && file == loose / "logic/new_system_3.lua" &&
              readText(file) == systemText && readText(loose / "logic/NEW_SYSTEM_2.lua") == "keep" &&
              readText(loose / "logic/new_system.lua") == systemText,
          "writeNewSystem skips names taken ignoring case and never overwrites");

    const std::filesystem::path dir = fsmProject("kast_assets_create_fsm", "fsm8");
    check(!isInStateMachinesDir(dir, dir) && isInStateMachinesDir(dir, dir / "fsms") &&
              isInStateMachinesDir(dir, dir / "fsms/entities") && !isInStateMachinesDir(dir, dir / "scripts") &&
              isInStateMachinesDir(dir, dir / "FSMS/x") && !isInStateMachinesDir(dir, dir / "fsmsx"),
          "isInStateMachinesDir: fsms/ and inside it only, ignoring case");

    writeFile(dir / "fsms/sub/new_state_machine.lua", R"(return { name = "taken_file", initial = "a", states = { a = {} } })");
    writeFile(dir / "fsms/sub/x.lua", R"(return { name = "new_state_machine_2", initial = "a", states = { a = {} } })");
    Document doc;
    check(openProject(doc, dir / kast::kProjectFileName, error), "create: open the test project");
    refreshStateMachines(doc);
    std::vector<std::string> names;
    for (const kast::ComponentDefinitionStatus& status : kast::stateMachineDefinitions().status)
        names.push_back(status.name);
    check(writeNewStateMachine(dir / "fsms/sub", names, file, error) && file == dir / "fsms/sub/new_state_machine_3.lua" &&
              contains(readText(file), "name = \"new_state_machine_3\",") && !contains(readText(file), "%NAME%"),
          "writeNewStateMachine skips a taken file name and a taken machine name, and names the machine after the file");
    refreshStateMachines(doc);
    check(std::ranges::find(kast::stateMachineDefinitions().valid, "new_state_machine_3", &kast::StateMachineDef::name) !=
              kast::stateMachineDefinitions().valid.end(),
          "the new state machine loads as a valid definition");

    names.clear();
    for (const kast::ComponentDefinitionStatus& status : kast::stateMachineDefinitions().status)
        names.push_back(status.name);
    const std::string before = readText(file);
    for (const char* bad : {"My FSM.lua", "1st.lua", "game.lua", "enemy_ai.txt", "x.lua"}) {
        const bool existed = std::filesystem::exists(dir / "fsms/sub" / bad); // x.lua: a taken file name
        check(!renameNewStateMachine(file, bad, names, error) && !error.empty() && readText(file) == before &&
                  std::filesystem::exists(dir / "fsms/sub" / bad) == existed,
              std::format("renameNewStateMachine refuses '{}' and leaves the disk unchanged", bad).c_str());
    }
    check(renameNewStateMachine(file, "enemy_ai.lua", names, error) && !std::filesystem::exists(file),
          "renameNewStateMachine renames the file");
    const std::string renamed = readText(dir / "fsms/sub/enemy_ai.lua");
    check(contains(renamed, "name = \"enemy_ai\",") && contains(renamed, "machine = \"enemy_ai\"") &&
              contains(renamed, "kast.log(\"enemy_ai: \"") && !contains(renamed, "new_state_machine_3"),
          "renameNewStateMachine renames the machine's template occurrences");
    refreshStateMachines(doc);
    check(std::ranges::find(kast::stateMachineDefinitions().valid, "enemy_ai", &kast::StateMachineDef::name) !=
              kast::stateMachineDefinitions().valid.end(),
          "the renamed state machine loads under its new name");
    writeFile(dir / "fsms/sub/edited.lua", "return {}");
    check(!renameNewStateMachine(dir / "fsms/sub/edited.lua", "fresh.lua", names, error) &&
              readText(dir / "fsms/sub/edited.lua") == "return {}",
          "renameNewStateMachine refuses a file without its name = line");

    std::vector<std::string> systems{"scripts/a.lua", "scripts/deep/b.lua", "other/c.lua", "scriptsx/d.lua"};
    check(remapProjectPaths(systems, "scripts/a.lua", "scripts/z.lua") &&
              (systems == std::vector<std::string>{"scripts/z.lua", "scripts/deep/b.lua", "other/c.lua", "scriptsx/d.lua"}),
          "remapProjectPaths follows a renamed file");
    check(remapProjectPaths(systems, "Scripts", "game/scripts") &&
              (systems ==
               std::vector<std::string>{"game/scripts/z.lua", "game/scripts/deep/b.lua", "other/c.lua", "scriptsx/d.lua"}),
          "remapProjectPaths follows a moved parent folder, ignoring case");
    check(!remapProjectPaths(systems, "nothing", "x") && systems[2] == "other/c.lua",
          "remapProjectPaths leaves unrelated paths unchanged");
}

void checkAssets()
{
    checkAssetScan();
    checkAssetNames();
    checkAssetPaths();
    checkAssetMoves();
    checkOpenSceneAction();
    checkAssetCreate();
}

// --- game GUI: builder document model, template/rename, UIDocument, button click -> event bus ---

constexpr const char* kGuiSample = R"(<rml>
    <head>
        <title>Menu</title>
        <link type="text/rcss" href="menu.rcss"/>
        <style>p { color: red; }</style>
    </head>
    <body class="root">
        <!-- keep me -->
        <div id="panel" class="box" style="width: 10px;" data-custom="1">
            <p id="title">Hello &amp; welcome</p>
            <p id="mixed">An <span class="hi">odd</span> one</p>
            <img src="logo.png" alt="Logo"><p>inside unknown</p></img>
            <button id="go" data-event="menu.go" title="Go!">Go</button>
        </div>
        <button id="quiet">Quiet</button>
    </body>
</rml>
)";

pugi::xml_node guiNode(const GuiDoc& doc, const char* id)
{
    std::vector<pugi::xml_node> nodes;
    doc.previewRml("", nodes);
    const auto it = std::ranges::find_if(nodes, [&](pugi::xml_node node) { return GuiDoc::attribute(node, "id") == id; });
    return it == nodes.end() ? pugi::xml_node() : *it;
}

std::vector<std::string> guiIds(pugi::xml_node parent)
{
    std::vector<std::string> ids;
    for (pugi::xml_node node : GuiDoc::elements(parent))
        ids.push_back(GuiDoc::attribute(node, "id").empty() ? node.name() : GuiDoc::attribute(node, "id"));
    return ids;
}

void checkGuiDocRoundTrip()
{
    GuiDoc doc;
    std::string error;
    check(!doc.parse("<rml><body>", error) && !error.empty(), "GuiDoc refuses malformed RML");
    check(!doc.parse("<rml><head/></rml>", error) && error.find("body") != std::string::npos, "GuiDoc needs <rml><body>");
    check(doc.parse(kGuiSample, error), "GuiDoc parses the sample");
    const std::string saved = doc.serialize();
    for (const char* kept : {"<!-- keep me -->", R"(<img src="logo.png" alt="Logo">)", "<p>inside unknown</p>",
                             R"(title="Go!")", R"(style="width: 10px;")", R"(data-custom="1")", "Hello &amp; welcome",
                             R"(<span class="hi">odd</span>)", "<style>p { color: red; }</style>", R"(<body class="root">)",
                             R"(<link type="text/rcss" href="menu.rcss" />)"})
        check(contains(saved, kept), std::format("round trip keeps {}", kept).c_str());
    GuiDoc again;
    check(again.parse(saved, error) && again.serialize() == saved, "serialize -> parse -> serialize is unchanged");
    check(doc.stylesheetHref() == "menu.rcss", "stylesheetHref is the linked .rcss");

    const pugi::xml_node panel = guiNode(doc, "panel");
    check(guiKind(panel) == GuiKind::Panel && guiKind(guiNode(doc, "title")) == GuiKind::Text &&
              guiKind(guiNode(doc, "go")) == GuiKind::Button && guiKind(panel.child("img")) == GuiKind::Unknown,
          "div/p/button are Panel/Text/Button, anything else Unknown");
    check(GuiDoc::hasEditableText(guiNode(doc, "title")) && GuiDoc::text(guiNode(doc, "title")) == "Hello & welcome" &&
              !GuiDoc::hasEditableText(guiNode(doc, "mixed")) && !GuiDoc::hasEditableText(panel),
          "text is editable on Text/Button holding only text");

    std::vector<pugi::xml_node> nodes;
    const std::string preview = doc.previewRml("div > p { color: blue; }", nodes);
    check(nodes.size() == 8 && nodes[0] == panel && contains(preview, R"(data-kast-node="0")") &&
              contains(preview, R"(data-kast-node="7")") && !contains(preview, "menu.rcss") &&
              contains(preview, "<style>div > p { color: blue; }</style>") && !contains(doc.serialize(), "data-kast-node"),
          "previewRml numbers elements pre-order, inlines the sheet unescaped and leaves the document alone");
}

void checkGuiDocEdits()
{
    GuiDoc doc;
    std::string error;
    check(doc.parse(kGuiSample, error), "GuiDoc edits: parse");
    const pugi::xml_node body = doc.body();
    pugi::xml_node panel = guiNode(doc, "panel");

    const pugi::xml_node added = doc.add(GuiKind::Panel, body);
    check(added && guiIds(body).back() == "div" && GuiDoc::elements(added).empty(), "add Panel appends an empty <div>");
    const pugi::xml_node text = doc.add(GuiKind::Text, added);
    const pugi::xml_node button = doc.add(GuiKind::Button, added);
    check(std::string_view(text.name()) == "p" && GuiDoc::text(text) == "Text" &&
              std::string_view(button.name()) == "button" && GuiDoc::text(button) == "Button",
          "add Text / Button: placeholder text");
    check(!doc.add(GuiKind::Unknown, body), "Unknown elements can't be created");

    check(doc.move(guiNode(doc, "quiet"), -1) && guiIds(body) == std::vector<std::string>{"quiet", "panel", "div"},
          "move -1 swaps with the previous element, skipping comments");
    check(!doc.move(guiNode(doc, "quiet"), -1), "move -1 at the start fails");
    check(doc.move(guiNode(doc, "quiet"), 1) && guiIds(body) == std::vector<std::string>{"panel", "quiet", "div"},
          "move +1 swaps with the next element");
    check(!doc.move(added, 1) && !doc.move(body, -1), "move +1 at the end, and moving <body>, fail");

    const pugi::xml_node img = panel.child("img");
    check(!doc.reparent(panel, panel) && !doc.reparent(panel, img) && !doc.reparent(body, added),
          "reparent refuses the node itself, its own subtree and <body>");
    check(doc.reparent(guiNode(doc, "go"), added) && guiIds(added) == std::vector<std::string>{"p", "button", "go"} &&
              guiNode(doc, "go").parent() == added,
          "reparent moves an element to the end of the new parent");
    check(doc.reparent(img, body) && img.parent() == body && GuiDoc::elements(img).size() == 1 &&
              contains(doc.serialize(), "<p>inside unknown</p>"),
          "Unknown elements move with their children");

    pugi::xml_node go = guiNode(doc, "go");
    GuiDoc::setAttribute(go, "data-event", "menu.start");
    check(GuiDoc::attribute(go, "data-event") == "menu.start" && contains(doc.serialize(), R"(data-event="menu.start")"),
          "data-event set");
    GuiDoc::setAttribute(go, "data-event", "");
    check(!go.attribute("data-event") && !contains(doc.serialize(), "data-event=\"\""), "empty data-event removes it");
    GuiDoc::setText(guiNode(doc, "title"), "New <title>");
    check(contains(doc.serialize(), "New &lt;title&gt;"), "setText escapes markup");

    check(!doc.remove(body), "remove refuses <body>");
    check(doc.remove(added) && !guiNode(doc, "go") && guiIds(body) == std::vector<std::string>{"panel", "quiet", "img"},
          "remove drops the element and its subtree");
    GuiDoc reopened;
    check(reopened.parse(doc.serialize(), error) && reopened.serialize() == doc.serialize(),
          "an edited document saves and reopens identically");
}

void checkGuiFiles()
{
    std::string error;
    const std::filesystem::path dir = looseTestDir("kast_gui_files");
    std::filesystem::path rml;
    check(writeNewGui(dir, rml, error) && rml == dir / "new_gui.rml" && std::filesystem::exists(dir / "new_gui.rcss"),
          "writeNewGui writes new_gui.rml + .rcss");
    const std::string first = readText(rml);
    GuiDoc doc;
    check(doc.parse(first, error) && doc.stylesheetHref() == "new_gui.rcss" && guiNode(doc, "panel") &&
              guiKind(guiNode(doc, "title")) == GuiKind::Text && guiKind(guiNode(doc, "button")) == GuiKind::Button &&
              GuiDoc::attribute(guiNode(doc, "button"), "data-event") == "ui.clicked",
          "the template: a Panel with a Text and a Button, linking its .rcss");
    check(linkedStylesheet(rml) == dir / "new_gui.rcss", "linkedStylesheet resolves the link next to the .rml");
    writeFile(dir / "NEW_GUI_3.rcss", "keep");
    check(writeNewGui(dir, rml, error) && rml == dir / "new_gui_2.rml", "a second GUI is new_gui_2");
    check(writeNewGui(dir, rml, error) && rml == dir / "new_gui_4.rml" && readText(dir / "NEW_GUI_3.rcss") == "keep" &&
              readText(dir / "new_gui.rml") == first,
          "writeNewGui skips a stem whose .rcss is taken (ignoring case) and never overwrites");
    check(contains(readText(rml), R"(href="new_gui_4.rcss")"), "each template links its own .rcss");

    check(renameGui(dir / "new_gui.rml", "hud.rml", error) && !std::filesystem::exists(dir / "new_gui.rml") &&
              !std::filesystem::exists(dir / "new_gui.rcss") && std::filesystem::exists(dir / "hud.rcss") &&
              linkedStylesheet(dir / "hud.rml") == dir / "hud.rcss",
          "renameGui renames the linked sibling .rcss and rewrites the link");
    writeFile(dir / "taken.rcss", "other");
    const std::string before = readText(dir / "hud.rml");
    check(!renameGui(dir / "hud.rml", "taken.rml", error) && !error.empty() && readText(dir / "hud.rml") == before &&
              std::filesystem::exists(dir / "hud.rcss") && readText(dir / "taken.rcss") == "other" &&
              !std::filesystem::exists(dir / "taken.rml"),
          "renameGui refuses when the new .rcss name is taken, leaving the disk unchanged");
    writeFile(dir / "plain.rml", "<rml><head/><body/></rml>");
    check(renameGui(dir / "plain.rml", "plainer.rml", error) && std::filesystem::exists(dir / "plainer.rml"),
          "renameGui renames an .rml that links nothing like any file");
}

void checkUIDocumentComponent()
{
    const kast::ComponentType type = kast::findAnyComponentType("UIDocument");
    check(static_cast<bool>(type) && std::string_view(type.displayName()) == "UI Document", "UIDocument is registered");
    kast::Scene scene;
    const entt::entity entity = scene.create("Menu");
    scene.registry().emplace<kast::UIDocument>(entity, kast::UIDocument{"ui/menu.rml", false});
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(json.dump().find(R"("UIDocument":{"path":"ui/menu.rml","visible":false})") != std::string::npos,
          "UIDocument saves path and visible");
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(json, loaded, error), "a scene with a UIDocument loads");
    bool found = false;
    for (auto [e, ui] : loaded.registry().view<const kast::UIDocument>().each())
        found = ui.path == "ui/menu.rml" && !ui.visible;
    check(found, "UIDocument round-trips through scene JSON");
    check(kast::UIDocument{}.visible, "UIDocument is visible by default");
}

void checkGuiEvents()
{
    kast::EventBus bus;
    nlohmann::json received;
    int calls = 0;
    kast::EventBus::Subscription subscription = bus.subscribe("menu.start", [&](const nlohmann::json& payload) {
        received = payload;
        ++calls;
    });
    check(kast::publishButtonClick(bus, "menu.start", "start_button", "ui/main_menu.rml"), "a click with an event publishes");
    bus.drain();
    check(calls == 1 && received == nlohmann::json{{"id", "start_button"}, {"document", "ui/main_menu.rml"}} &&
              bus.history().back().publisher == "UI" && bus.history().back().name == "menu.start",
          "the click payload is {id, document}, published by UI");
    check(!kast::publishButtonClick(bus, "", "quiet", "ui/main_menu.rml"), "an empty data-event publishes nothing");
    bus.drain();
    check(calls == 1 && bus.history().size() == 1, "nothing was delivered for the empty event");
    kast::publishButtonClick(bus, "menu.start", "", "ui/main_menu.rml");
    bus.drain();
    check(calls == 2 && received["id"] == "", "a button without an id sends id \"\"");
}

// The GUI Builder saves the linked .rcss: a link that leaves the project (or a loose .rml's folder) is not followed.
void checkGuiStylesheetBounds()
{
    const std::filesystem::path root = projectTestDir("kast_gui_bounds", "bounds");
    std::filesystem::create_directories(root / "ui");
    const auto rmlLinking = [](const char* href) {
        return std::format(R"(<rml><head><link type="text/rcss" href="{}"/></head><body/></rml>)", href);
    };
    writeFile(root / "ui/in.rml", rmlLinking("../styles/main.rcss"));
    check(linkedStylesheet(root / "ui/in.rml") == root / "styles/main.rcss", "a sheet elsewhere in the project is linked");
    writeFile(root / "ui/out.rml", rmlLinking("../../outside.rcss"));
    check(linkedStylesheet(root / "ui/out.rml").empty(), "a sheet outside the project is not linked");
    writeFile(root / "ui/abs.rml", rmlLinking("C:/Windows/evil.rcss"));
    check(linkedStylesheet(root / "ui/abs.rml").empty(), "an absolute sheet path outside the project is not linked");
    const std::filesystem::path loose = looseTestDir("kast_gui_bounds_loose");
    writeFile(loose / "a.rml", rmlLinking("../b.rcss"));
    check(linkedStylesheet(loose / "a.rml").empty(), "a GUI in no project links only sheets in its own folder");
}

// Ctrl+V / Ctrl+C in an RmlUi text field use the OS clipboard (SDL's), through GuiContext::onEvent like the game;
// Tab focuses the field unless it is styled tab-index: none.
// Headless: bgfx's Noop renderer, no window. Saves and restores the clipboard's text.
void checkGuiClipboard()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        check(false, "SDL video starts for the clipboard check");
        return;
    }
    char* saved = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr;
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    check(bgfx::init(init) && kast::gui::init(), "headless bgfx and RmlUi start");
    {
        kast::GuiContext gui("clipboard_check");
        gui.resize(400, 200);
        Rml::Context* rml = gui.context();
        Rml::ElementDocument* doc =
            rml ? rml->LoadDocumentFromMemory(R"(<rml><body><input type="text" id="field"/></body></rml>)") : nullptr;
        check(doc != nullptr, "clipboard: the document loads");
        if (doc) {
            doc->Show();
            rml->Update();
            auto* field = dynamic_cast<Rml::ElementFormControl*>(doc->GetElementById("field"));
            check(field && field->Focus(), "clipboard: the text field takes focus");
            const auto keyDown = [&](SDL_Keycode key) {
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.key = key;
                event.key.mod = SDL_KMOD_LCTRL;
                gui.onEvent(event);
                rml->Update();
            };
            SDL_SetClipboardText("pasted-token_123");
            keyDown(SDLK_V);
            check(field && field->GetValue() == "pasted-token_123", "Ctrl+V pastes the OS clipboard into the field");
            SDL_SetClipboardText("other");
            keyDown(SDLK_A);
            keyDown(SDLK_C);
            char* copied = SDL_GetClipboardText();
            check(std::string_view(copied) == "pasted-token_123", "Ctrl+A, Ctrl+C copies the field to the OS clipboard");
            SDL_free(copied);
            // Tab with nothing focused moves focus into a text field (RmlUi's default), unless the field has
            // "tab-index: none": samples/starter-mmo's chat input does, so its Tab (the target key) stays the game's.
            const auto tabFocuses = [&](Rml::ElementDocument& document) {
                if (Rml::Element* focus = rml->GetFocusElement())
                    focus->Blur();
                document.Show();
                rml->Update();
                SDL_Event tab{};
                tab.type = SDL_EVENT_KEY_DOWN;
                tab.key.key = SDLK_TAB;
                gui.onEvent(tab);
                rml->Update();
                Rml::Element* focus = rml->GetFocusElement();
                return focus && dynamic_cast<Rml::ElementFormControl*>(focus);
            };
            check(tabFocuses(*doc), "Tab focuses an unstyled text field");
            doc->Hide();
            // The sample's own chat box, its style sheet inlined (the link needs a project to resolve).
            const std::filesystem::path ui = std::filesystem::path(KAST_SOURCE_DIR) / "samples/starter-mmo/ui";
            std::string sheet, chatRml;
            check(kast::readFile(ui / "starter.rcss", sheet) && kast::readFile(ui / "chat.rml", chatRml),
                  "the starter-mmo chat box and style sheet read");
            const std::string link = R"(<link type="text/rcss" href="starter.rcss"/>)";
            if (const size_t at = chatRml.find(link); at != std::string::npos)
                chatRml.replace(at, link.size(), "<style>" + sheet + "</style>");
            Rml::ElementDocument* chat = rml->LoadDocumentFromMemory(chatRml);
            check(chat && chat->GetElementById("chat_input") && !tabFocuses(*chat),
                  "Tab does not focus the starter-mmo chat input (inline tab-index: none)");
        }
    }
    kast::gui::shutdown();
    bgfx::shutdown();
    SDL_SetClipboardText(saved ? saved : "");
    SDL_free(saved);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void checkGui()
{
    checkGuiDocRoundTrip();
    checkGuiDocEdits();
    checkGuiFiles();
    checkGuiStylesheetBounds();
    checkUIDocumentComponent();
    checkGuiEvents();
}

void checkExternalEditor()
{
    const std::filesystem::path plain = R"(C:\game\scripts\report.lua)";
    const std::filesystem::path spaced = R"(C:\My Game\ui\main menu.rcss)";
    check(expandEditorCommand(kDefaultEditorCommand, plain) == R"(code "C:\game\scripts\report.lua")",
          "default command: {file} replaced by the path");
    check(expandEditorCommand(kDefaultEditorCommand, spaced) == R"(code "C:\My Game\ui\main menu.rcss")",
          "path with spaces stays inside the template's quotes");
    check(expandEditorCommand("notepad++.exe -multiInst", spaced) ==
              R"(notepad++.exe -multiInst "C:\My Game\ui\main menu.rcss")",
          "no {file}: the quoted path is appended");
    check(expandEditorCommand("", plain) == R"("C:\game\scripts\report.lua")", "empty command: just the quoted path");
    check(expandEditorCommand("ed {file} --diff {file}", plain) ==
              R"(ed C:\game\scripts\report.lua --diff C:\game\scripts\report.lua)",
          "every {file} replaced");
    check(expandEditorCommand(kDefaultEditorCommand, std::filesystem::path(u8"C:\\j\u00fcgo\\a.json")) ==
              "code \"C:\\j\xC3\xBCgo\\a.json\"",
          "non-ASCII path expanded as UTF-8");

    const std::filesystem::path dir = looseTestDir("kast_editor_settings_check");
    const std::filesystem::path file = dir / "Kast/Editor/editor_settings.json";
    std::string error;
    check(loadEditorCommand(file) == kDefaultEditorCommand, "no settings file: the default command");
    check(saveEditorCommand(file, "subl \"{file}\"", error) && loadEditorCommand(file) == "subl \"{file}\"",
          "save/load round trip, creating the folder");
    check(contains(readText(file), "\"externalEditor\": \"subl \\\"{file}\\\"\""), "stored under externalEditor");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "{ not json";
    check(loadEditorCommand(file) == kDefaultEditorCommand && logged("Editor settings"),
          "invalid settings file: the default command, with a warning");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << R"({"other": 1})";
    check(loadEditorCommand(file) == kDefaultEditorCommand, "missing key: the default command");

    // cmd.exe runs the command: a %VAR% in the file name must reach it literally, not expanded (%OS% -> Windows_NT).
    // copy stands in for an editor; poll() is never called, so a failure can't fall back to the file association.
    const std::filesystem::path source = dir / "%OS%.lua";
    const std::filesystem::path copied = dir / "copied.lua";
    writeFile(source, "-- percent");
    {
        ExternalEditor editor(dir / "percent_settings.json");
        check(editor.setCommand(std::format("copy /y \"{{file}}\" \"{}\"", copied.string()), error),
              "set a copy command");
        editor.open(source);
        for (int i = 0; i < 200 && readText(copied) != "-- percent"; ++i) // until copy has finished writing it
            SDL_Delay(50);
    }
    check(readText(copied) == "-- percent", "a %VAR% in the file name reaches the command unexpanded");
    ExternalEditor editor(file);
    check(editor.command() == kDefaultEditorCommand && editor.setCommand("vim {file}", error) &&
              editor.command() == "vim {file}" && loadEditorCommand(file) == "vim {file}",
          "setCommand saves and is used from then on");
}

// Point light falloff and per-mesh selection (renderer.h), PointLight validation and scene ambient in scene files.
void checkPointLights()
{
    check(kast::pointLightFalloff(5.0f, 5.0f) == 0.0f && kast::pointLightFalloff(7.0f, 5.0f) == 0.0f,
          "falloff is 0 at and beyond range");
    float previous = kast::pointLightFalloff(0.0f, 5.0f);
    bool decreasing = previous > 0.0f;
    for (float d = 0.05f; d < 5.0f; d += 0.05f) {
        const float f = kast::pointLightFalloff(d, 5.0f);
        decreasing = decreasing && f > 0.0f && f < previous;
        previous = f;
    }
    check(decreasing, "falloff is positive and decreasing inside range");

    // Ten lights on the +X axis at 1..10 m from a unit box at the origin, listed far to near; every range reaches it.
    std::vector<kast::PointLightSample> lights;
    for (int i = 10; i >= 1; --i)
        lights.push_back({{static_cast<float>(i), 0, 0}, 20.0f, {1, 1, 1}});
    const kast::Aabb box{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    check(kast::nearestPointLights(lights, box) == std::vector<size_t>{9, 8, 7, 6, 5, 4, 3, 2},
          "nearest 8 of 10, nearest first");
    lights[9].range = 0.4f; // 1 m away, its range stops 0.1 m short of the box face
    lights[8].range = 1.6f; // 2 m away, reaches the face at 1.5 m
    check(kast::nearestPointLights(lights, box) == std::vector<size_t>{8, 7, 6, 5, 4, 3, 2, 1},
          "lights whose range misses the box are skipped");
    check(kast::nearestPointLights({}, box).empty(), "no lights");

    const entt::meta_type type = kast::findComponentType("PointLight");
    const kast::PointLight defaults;
    check(type && kast::componentInfo(type).validate && defaults.intensity == 1.0f && defaults.range == 10.0f &&
              near(defaults.color, {1, 1, 1}),
          "PointLight registered with defaults white / 1 / 10 m");
    const std::pair<const char*, bool> values[] = {
        {R"({"color": [1, 0.5, 0], "intensity": 0, "range": 0.001})", true},
        {R"({"color": [1, 1, 1], "intensity": 1, "range": 0})", false},
        {R"({"color": [1, 1, 1], "intensity": 1, "range": -2})", false},
        {R"({"color": [1, 1, 1], "intensity": -0.1, "range": 5})", false},
        {R"({"color": [-0.1, 1, 1], "intensity": 1, "range": 5})", false},
        {R"({"color": [1, 1, 1.01], "intensity": 1, "range": 5})", false},
    };
    for (const auto& [text, valid] : values) {
        std::string error;
        check(static_cast<bool>(kast::componentFromJson(type, nlohmann::ordered_json::parse(text), error)) == valid, text);
    }

    // Round trip: point lights and custom ambient; a file without "lighting" gets the defaults.
    kast::Scene scene;
    const entt::entity e = scene.create("Lamp");
    scene.registry().emplace<kast::PointLight>(e, kast::PointLight{{0.25f, 0.5f, 1.0f}, 2.5f, 7.0f});
    scene.lighting = {{0.1f, 0.2f, 0.3f}, 0.45f};
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(json["version"] == kast::kSceneFileVersion && json.contains("lighting"), "saved with lighting, same version");
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(json, loaded, error), "point light scene loads");
    const entt::entity lamp = loaded.children(entt::null).at(0);
    const auto* light = loaded.registry().try_get<kast::PointLight>(lamp);
    check(light && near(light->color, {0.25f, 0.5f, 1.0f}) && light->intensity == 2.5f && light->range == 7.0f,
          "PointLight round trip");
    check(near(loaded.lighting.ambientColor, {0.1f, 0.2f, 0.3f}) && loaded.lighting.ambientIntensity == 0.45f,
          "lighting round trip");
    check(kast::sceneToJson(loaded) == json, "second save is identical");
    nlohmann::ordered_json noLighting = json;
    noLighting.erase("lighting");
    check(kast::sceneFromJson(noLighting, loaded, error) && near(loaded.lighting.ambientColor, {1, 1, 1}) &&
              loaded.lighting.ambientIntensity == 0.2f,
          "missing lighting: default white at 0.2");
}

// Shadow math (shadow.h), shadowed point light selection (renderer.h) and the shadow fields in scene files.
void checkShadows()
{
    // Cascade splits: from near to far, increasing, between the uniform and logarithmic schemes; past count = far.
    const auto splits = kast::cascadeSplits(0.1f, 100.0f, 4);
    bool increasing = true;
    for (size_t i = 1; i < splits.size(); ++i)
        increasing = increasing && splits[i] > splits[i - 1];
    check(splits[0] == 0.1f && splits[4] == 100.0f && increasing, "4 splits run from near to far, increasing");
    const float logarithmic = 0.1f * std::pow(1000.0f, 0.25f), uniform = 0.1f + 99.9f * 0.25f;
    check(splits[1] > logarithmic && splits[1] < uniform &&
              near(splits[1], kast::kCascadeSplitLambda * logarithmic + (1.0f - kast::kCascadeSplitLambda) * uniform),
          "first split mixes logarithmic and uniform by lambda");
    const auto one = kast::cascadeSplits(0.5f, 40.0f, 1);
    check(one[0] == 0.5f && one[1] == 40.0f && one[2] == 40.0f && one[4] == 40.0f, "1 cascade covers near..far");
    check(kast::cascadeSplits(0.1f, 100.0f, 9) == splits && kast::cascadeSplits(0.5f, 40.0f, 0) == one,
          "cascade count clamped to 1..4");

    // A camera at (0, 2, 0) looking down -Z, 90 degree FOV, square: at view depth d the slice is d wide each side.
    for (const bool homogeneousDepth : {false, true}) {
        float view[16], proj[16];
        bx::mtxLookAt(view, {0, 2, 0}, {0, 2, -1}, {0, 1, 0}, bx::Handedness::Right);
        bx::mtxProj(proj, 90.0f, 1.0f, 0.1f, 1000.0f, homogeneousDepth, bx::Handedness::Right);
        const auto corners = kast::frustumSliceCorners(view, proj, 2.0f, 10.0f);
        bool onSlice = true;
        for (size_t i = 0; i < 8; ++i) {
            const float d = i < 4 ? 2.0f : 10.0f;
            onSlice = onSlice && near(corners[i].z, -d) && near(std::abs(corners[i].x), d) &&
                      near(std::abs(corners[i].y - 2.0f), d);
        }
        check(onSlice, "slice corners at the near and far view depths");

        // The cascade's ortho bounds hold every corner, and a caster toward the light stays inside its depth range.
        const bx::Vec3 lightDir = bx::normalize(bx::Vec3{0.3f, -0.8f, -0.5f});
        const bx::Vec3 caster{0.0f, 30.0f, -5.0f}; // high above the slice, toward the light
        const kast::Cascade cascade =
            kast::fitCascade(corners, lightDir, std::min(bx::dot(caster, lightDir), 0.0f), homogeneousDepth);
        float viewProj[16];
        bx::mtxMul(viewProj, cascade.view.data(), cascade.proj.data());
        const float zMin = homogeneousDepth ? -1.0f : 0.0f;
        bool inside = true;
        for (const bx::Vec3& corner : corners) {
            const bx::Vec3 p = bx::mulH(corner, viewProj);
            inside = inside && std::abs(p.x) <= 1.0f && std::abs(p.y) <= 1.0f && p.z >= zMin && p.z <= 1.0f;
        }
        check(inside, "cascade bounds contain the frustum slice");
        const bx::Vec3 casterNdc = bx::mulH(caster, viewProj);
        check(casterNdc.z >= zMin && casterNdc.z <= 1.0f, "caster toward the light is within the cascade's depth");
        check(near(cascade.texelSize * kast::kCascadeSize, 2.0f / cascade.proj[0]) && cascade.depthRange > 0.0f,
              "texel size is the cascade's width over its resolution");

        // Stable: a camera move shifts the projection by whole texels only (here 0 or 1 for a tenth-texel move).
        std::array<bx::Vec3, 8> moved = corners;
        for (bx::Vec3& corner : moved)
            corner = bx::add(corner, bx::Vec3{cascade.texelSize * 0.1f, 0.0f, 0.0f});
        const kast::Cascade after = kast::fitCascade(moved, lightDir, std::min(bx::dot(caster, lightDir), 0.0f),
                                                     homogeneousDepth);
        const float shift = (after.proj[12] - cascade.proj[12]) / cascade.proj[0] / cascade.texelSize;
        check(std::abs(shift - std::round(shift)) < 1e-2f, "cascade moves in whole texels");
    }

    // The 4 castShadows point lights nearest the eye, nearest first; ties keep list order.
    std::vector<kast::PointLightSample> lights;
    for (int i = 0; i < 7; ++i) // at x = 7 .. 1, every other one casting
        lights.push_back({{static_cast<float>(7 - i), 0, 0}, 10.0f, {1, 1, 1}, i % 2 == 0, 0.05f});
    // Casting: indices 0 (x 7), 2 (x 5), 4 (x 3), 6 (x 1).
    check(kast::shadowedPointLights(lights, {0, 0, 0}) == std::vector<size_t>{6, 4, 2, 0}, "4 casting lights, nearest first");
    lights.push_back({{-1, 0, 0}, 10.0f, {1, 1, 1}, true, 0.05f}); // ties with index 6, later in the list
    check(kast::shadowedPointLights(lights, {0, 0, 0}) == std::vector<size_t>{6, 7, 4, 2},
          "only the 4 nearest casting lights shadow; ties keep list order");
    check(kast::shadowedPointLights(lights, {8, 0, 0}) == std::vector<size_t>{0, 2, 4, 6}, "nearest to the eye");
    for (auto& light : lights)
        light.castShadows = false;
    check(kast::shadowedPointLights(lights, {0, 0, 0}).empty(), "no casting lights, no shadows");

    // Defaults; scenes saved before shadows (fields missing) load with them.
    const kast::DirectionalLight sun;
    const kast::PointLight lamp;
    const kast::MeshRenderer mesh;
    const kast::ModelRenderer model;
    check(sun.castShadows && sun.maxDistance == 100.0f && sun.cascadeCount == 4 && sun.bias > 0.0f &&
              sun.normalBias > 0.0f && !lamp.castShadows && lamp.bias > 0.0f && mesh.castShadows &&
              mesh.receiveShadows && model.castShadows && model.receiveShadows,
          "shadow field defaults");
    std::string error;
    const entt::meta_any oldSun = kast::componentFromJson(kast::findComponentType("DirectionalLight"),
                                                     nlohmann::ordered_json::parse(R"({"color": [1, 1, 1], "intensity": 2})"),
                                                     error);
    const auto* oldSunValue = oldSun ? &oldSun.cast<const kast::DirectionalLight&>() : nullptr;
    check(oldSunValue && oldSunValue->intensity == 2.0f && oldSunValue->castShadows && oldSunValue->cascadeCount == 4 &&
              oldSunValue->maxDistance == 100.0f,
          "DirectionalLight without shadow fields loads with the defaults");
    const char* oldScene = R"({"version": 5, "entities": [
        {"id": 1, "name": "Box", "parent": null, "components": {
            "Transform": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
            "MeshRenderer": {"primitive": "cube", "material": ""},
            "ModelRenderer": {"model": "", "materials": []},
            "PointLight": {"color": [1, 1, 1], "intensity": 1, "range": 5}}}]})";
    kast::Scene loaded;
    check(kast::sceneFromJson(nlohmann::ordered_json::parse(oldScene), loaded, error), "pre-shadow scene loads");
    const entt::entity box = loaded.children(entt::null).at(0);
    const auto& registry = loaded.registry();
    check(registry.get<kast::MeshRenderer>(box).castShadows && registry.get<kast::MeshRenderer>(box).receiveShadows &&
              registry.get<kast::ModelRenderer>(box).castShadows && registry.get<kast::ModelRenderer>(box).receiveShadows &&
              !registry.get<kast::PointLight>(box).castShadows,
          "pre-shadow scene gets the shadow defaults");

    // Validation and round trip.
    const entt::meta_type sunType = kast::findComponentType("DirectionalLight");
    const std::pair<const char*, bool> values[] = {
        {R"({"color": [1, 1, 1], "intensity": 1, "cascadeCount": 1, "maxDistance": 0.5, "bias": 0, "normalBias": 0})", true},
        {R"({"color": [1, 1, 1], "intensity": 1, "cascadeCount": 0})", false},
        {R"({"color": [1, 1, 1], "intensity": 1, "cascadeCount": 5})", false},
        {R"({"color": [1, 1, 1], "intensity": 1, "maxDistance": 0})", false},
        {R"({"color": [1, 1, 1], "intensity": 1, "bias": -0.1})", false},
        {R"({"color": [1, 1, 1], "intensity": 1, "normalBias": -1})", false},
    };
    for (const auto& [text, valid] : values)
        check(static_cast<bool>(kast::componentFromJson(sunType, nlohmann::ordered_json::parse(text), error)) == valid, text);
    check(!kast::componentFromJson(kast::findComponentType("PointLight"),
                                   nlohmann::ordered_json::parse(R"({"color": [1, 1, 1], "intensity": 1, "range": 5, "bias": -1})"),
                                   error),
          "PointLight bias must be >= 0");
    kast::Scene scene;
    const entt::entity e = scene.create("Lit");
    scene.registry().emplace<kast::DirectionalLight>(e, kast::DirectionalLight{{1, 1, 1}, 1.0f, false, 0.2f, 3.0f, 50.0f, 2});
    scene.registry().emplace<kast::PointLight>(e, kast::PointLight{{1, 1, 1}, 1.0f, 5.0f, true, 0.1f});
    scene.registry().emplace<kast::MeshRenderer>(e, kast::MeshRenderer{kast::Primitive::Cube, "", false, false});
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(kast::sceneFromJson(json, loaded, error), "shadow fields scene loads");
    const entt::entity lit = loaded.children(entt::null).at(0);
    const auto& d = loaded.registry().get<kast::DirectionalLight>(lit);
    const auto& p = loaded.registry().get<kast::PointLight>(lit);
    const auto& m = loaded.registry().get<kast::MeshRenderer>(lit);
    check(!d.castShadows && d.bias == 0.2f && d.normalBias == 3.0f && d.maxDistance == 50.0f && d.cascadeCount == 2 &&
              p.castShadows && p.bias == 0.1f && !m.castShadows && !m.receiveShadows,
          "shadow fields round trip");
}

// --- Post-processing ---

void checkFrustumCulling()
{
    const auto box = [](bx::Vec3 center, float half) {
        return kast::Aabb{bx::sub(center, {half, half, half}), bx::add(center, {half, half, half})};
    };
    // A camera at the origin looking down -Z, 90 degree FOV, square, near 0.1, far 100: at depth d the view is d wide
    // each side.
    for (const bool homogeneousDepth : {false, true}) {
        float view[16], proj[16], viewProj[16];
        bx::mtxLookAt(view, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, bx::Handedness::Right);
        bx::mtxProj(proj, 90.0f, 1.0f, 0.1f, 100.0f, homogeneousDepth, bx::Handedness::Right);
        bx::mtxMul(viewProj, view, proj);
        const kast::Frustum frustum = kast::frustumFromViewProj(viewProj, homogeneousDepth);
        check(kast::intersects(frustum, box({0, 0, -10}, 1)), "box ahead is in the frustum");
        check(kast::intersects(frustum, box({10.5f, 0, -10}, 1)), "box over the right edge is in the frustum");
        check(!kast::intersects(frustum, box({13, 0, -10}, 1)), "box past the right edge is culled");
        check(!kast::intersects(frustum, box({0, -13, -10}, 1)), "box below the view is culled");
        check(!kast::intersects(frustum, box({0, 0, 10}, 1)), "box behind the eye is culled");
        check(!kast::intersects(frustum, box({0, 0, -0.06f}, 0.02f)), "box before the near plane is culled");
        check(kast::intersects(frustum, box({0, 0, -100}, 1)), "box across the far plane is in the frustum");
        check(!kast::intersects(frustum, box({0, 0, -102}, 1)), "box past the far plane is culled");

        // A cascade fitted to a slice keeps a caster between the light and the slice, culls one beyond it.
        const auto corners = kast::frustumSliceCorners(view, proj, 2.0f, 10.0f);
        const bx::Vec3 lightDir = bx::normalize(bx::Vec3{0.3f, -0.8f, -0.5f});
        const bx::Vec3 caster = bx::add(bx::Vec3{0, 0, -6}, bx::mul(lightDir, -40.0f)); // up the light from the slice
        const kast::Cascade cascade =
            kast::fitCascade(corners, lightDir, bx::dot(caster, lightDir) - 1.0f, homogeneousDepth);
        float cascadeViewProj[16];
        bx::mtxMul(cascadeViewProj, cascade.view.data(), cascade.proj.data());
        const kast::Frustum light = kast::frustumFromViewProj(cascadeViewProj, homogeneousDepth);
        check(kast::intersects(light, box(caster, 1)), "caster toward the light is in the cascade");
        check(!kast::intersects(light, box(bx::add(bx::Vec3{0, 0, -6}, bx::mul(lightDir, 40.0f)), 1)),
              "box past the slice, away from the light, is culled from the cascade");
        check(!kast::intersects(light, box({200, 0, -6}, 1)), "box beside the cascade is culled");
    }
    // A point light cube face (left-handed, 0..1 depth, as the Renderer's): +X sees a box on +X, -X doesn't.
    float proj[16], face[16], viewProj[16];
    bx::mtxProj(proj, 90.0f, 1.0f, kast::kPointShadowNear, 10.0f, false, bx::Handedness::Left);
    bx::mtxLookAt(face, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, bx::Handedness::Left);
    bx::mtxMul(viewProj, face, proj);
    check(kast::intersects(kast::frustumFromViewProj(viewProj, false), box({5, 1, 1}, 0.5f)), "+X face sees a box on +X");
    bx::mtxLookAt(face, {0, 0, 0}, {-1, 0, 0}, {0, 1, 0}, bx::Handedness::Left);
    bx::mtxMul(viewProj, face, proj);
    check(!kast::intersects(kast::frustumFromViewProj(viewProj, false), box({5, 1, 1}, 0.5f)), "-X face culls a box on +X");
}

// Skies: the "sky" block, the gradient and face checks behind the renderer, and kast.sky.
void checkSky()
{
    kast::Scene scene;
    scene.sky = {kast::SkyMode::Cubemap, {0.1f, 0.2f, 0.3f}, {0.4f, 0.5f, 0.6f}, {0.7f, 0.8f, 0.9f},
                 {"s/px.png", "s/nx.png", "s/py.png", "s/ny.png", "s/pz.png", "s/nz.jpg"}};
    nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(json["sky"]["mode"] == "cubemap" && json["sky"]["faces"]["nz"] == "s/nz.jpg" && json["sky"]["top"][2] == kast::jsonNumber(0.3f),
          "sky saved as a \"sky\" block, faces by name");
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(json, loaded, error) && loaded.sky.mode == kast::SkyMode::Cubemap &&
              near(loaded.sky.top, {0.1f, 0.2f, 0.3f}) && near(loaded.sky.horizon, {0.4f, 0.5f, 0.6f}) &&
              near(loaded.sky.bottom, {0.7f, 0.8f, 0.9f}) && loaded.sky.faces == scene.sky.faces,
          "sky round trip");
    check(kast::sceneToJson(loaded) == json, "sky saves back identically");
    check(!json["sky"].contains("cube"), "no cube file: no \"cube\" key (older scenes save unchanged)");
    scene.sky.cube = "s/sky.ktx";
    check(kast::sceneFromJson(kast::sceneToJson(scene), loaded, error) && loaded.sky.cube == "s/sky.ktx" &&
              loaded.sky.faces == scene.sky.faces,
          "sky cube file round trip");
    json.erase("sky");
    check(kast::sceneFromJson(json, loaded, error) && loaded.sky.mode == kast::SkyMode::Off &&
              near(loaded.sky.top, kast::SceneSky{}.top) && loaded.sky.faces == kast::SceneSky{}.faces,
          "missing sky: off, defaults");
    json["sky"] = {{"mode", "procedural"}, {"faces", {{"py", "up.png"}}}};
    check(kast::sceneFromJson(json, loaded, error) && loaded.sky.mode == kast::SkyMode::Procedural &&
              near(loaded.sky.horizon, kast::SceneSky{}.horizon) && loaded.sky.faces[2] == "up.png" &&
              loaded.sky.faces[0].empty(),
          "sky fields are optional");

    // fs_sky.sc's gradient.
    kast::SceneSky sky;
    sky.top = {0, 0, 1};
    sky.horizon = {1, 0, 0};
    sky.bottom = {0, 1, 0};
    check(near(kast::proceduralSkyColor(sky, 0.0f), {1, 0, 0}) && near(kast::proceduralSkyColor(sky, 1.0f), {0, 0, 1}) &&
              near(kast::proceduralSkyColor(sky, -1.0f), {0, 1, 0}),
          "sky gradient: horizon at eye level, top straight up, bottom straight down");
    check(near(kast::proceduralSkyColor(sky, 0.25f), {0.5f, 0, 0.5f}) &&
              near(kast::proceduralSkyColor(sky, -0.25f), {0.5f, 0.5f, 0}),
          "sky gradient: blended by sqrt(|y|)");
    bool smooth = true; // no jumps, across the horizon or anywhere else
    for (int i = -100; i < 100; ++i)
        smooth = smooth && bx::length(bx::sub(kast::proceduralSkyColor(sky, i / 100.0f),
                                              kast::proceduralSkyColor(sky, (i + 1) / 100.0f))) < 0.15f;
    check(smooth, "sky gradient is continuous");

    // Cube faces: square, all px's size.
    std::array<kast::MipImage, 6> faces;
    for (kast::MipImage& face : faces)
        face.width = face.height = 16;
    check(kast::skyFacesError(faces).empty(), "six 16x16 faces make a cube");
    faces[3].height = 8;
    check(kast::skyFacesError(faces) == "face ny is 16x8, not square", "a non-square face is named");
    faces[3].width = 8;
    check(kast::skyFacesError(faces) == "face ny is 8x8, but px is 16x16", "a face of another size is named");

    // kast.sky: get, partial set, invalid sets raise and change nothing; Stop restores the scene's sky.
    const std::filesystem::path dir = luaTestDir();
    writeFile(dir / "scripts/sky.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
    assert(string.find(err, "sky.lua:%d+:"), "error '" .. tostring(err) .. "' has no file:line")
end
local done = false
return { update = function(dt)
    if done then return end
    done = true
    local s = kast.sky.get()
    assert(s.mode == "procedural" and s.top.z == 1 and s.horizon.x == 1 and s.bottom.y == 1 and s.faces.px == "",
           "get: mode name, colors as {x, y, z}, faces by name")
    kast.sky.set({mode = "cubemap", faces = {nz = "sky/nz.png"}, top = {x = 0.5, y = 0.25, z = 0}})
    s = kast.sky.get()
    assert(s.mode == "cubemap" and s.faces.nz == "sky/nz.png" and s.faces.px == "" and s.top.x == 0.5 and
           s.horizon.x == 1, "partial set keeps the other fields and faces")
    kast.sky.set({cube = "sky/storm.ktx"})
    assert(kast.sky.get().cube == "sky/storm.ktx" and kast.sky.get().faces.nz == "sky/nz.png", "cube file set")
    kast.sky.set({cube = ""})
    assert(kast.sky.get().cube == nil, "cube file cleared")
    fails('"cube" must be a string', function() kast.sky.set({cube = 3}) end)
    fails('"mode" must be', function() kast.sky.set({top = {x = 0, y = 0, z = 0}, mode = "night"}) end)
    fails('unknown field "sun"', function() kast.sky.set({sun = 1}) end)
    fails('"horizon" must be an array of 3 numbers between 0 and 1',
          function() kast.sky.set({horizon = {x = 2, y = 0, z = 0}}) end)
    fails('"bottom" must be a table {x=, y=, z=}', function() kast.sky.set({bottom = {1, 0, 0}}) end)
    fails('unknown face "up"', function() kast.sky.set({faces = {up = "x.png"}}) end)
    fails('"px" must be a string', function() kast.sky.set({faces = {px = 3}}) end)
    fails('"faces" must be an object', function() kast.sky.set({faces = "x.png"}) end)
    fails("kast.sky.set: expected a table", function() kast.sky.set("procedural") end)
    fails("kast is read-only", function() kast.sky.get = nil end)
    s = kast.sky.get()
    assert(s.mode == "cubemap" and s.top.x == 0.5 and s.top.y == 0.25 and s.horizon.x == 1 and s.faces.px == "",
           "invalid sets leave the sky unchanged")
    kast.log("sky checks done")
end })");
    Document doc;
    doc.path = (dir / "sky.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Sky"};
    doc.scene.sky = sky; // the gradient above
    doc.scene.sky.mode = kast::SkyMode::Procedural;
    doc.scene.systems = {"scripts/sky.lua"};
    startPlay(doc);
    updatePlay(doc, 0.1f);
    checkOk(doc.lua, 0, "kast.sky asserts pass");
    check(logged("sky checks done"), "kast.sky checks ran");
    check(doc.scene.sky.mode == kast::SkyMode::Cubemap && doc.scene.sky.faces[5] == "sky/nz.png" &&
              near(doc.scene.sky.top, {0.5f, 0.25f, 0.0f}),
          "kast.sky.set changes the scene's sky");
    stopPlay(doc);
    check(doc.scene.sky.mode == kast::SkyMode::Procedural && doc.scene.sky.faces[5].empty() &&
              near(doc.scene.sky.top, {0, 0, 1}),
          "Stop restores the sky kast.sky.set changed");
}

// ParticleEmitter JSON: round trip with its enums by name; bad values are rejected, also at scene load.
void checkParticleJson()
{
    const entt::meta_type type = kast::findComponentType("ParticleEmitter");
    check(type && kast::componentInfo(type).removable && !kast::componentInfo(type).runtime,
          "particles: ParticleEmitter is registered and authored");
    kast::ParticleEmitter custom;
    custom.shape = kast::ParticleShape::Box;
    custom.space = kast::ParticleSpace::Local;
    custom.blend = kast::ParticleBlend::Additive;
    custom.maxParticles = 65536;
    custom.startColor = {16.0f, 4.0f, 0.0f};
    custom.texture = "textures/spark.png";
    const nlohmann::ordered_json json = kast::componentToJson(entt::meta_any{custom});
    check(json["shape"] == "box" && json["space"] == "local" && json["blend"] == "additive" &&
              json["maxParticles"] == 65536 && json["texture"] == "textures/spark.png",
          "particles: enums save by name, the fields by key");
    std::string error;
    const entt::meta_any loaded = kast::componentFromJson(type, json, error);
    check(loaded && error.empty() && kast::componentToJson(loaded) == json, "particles: JSON round trip");
    check(kast::componentToJson(entt::meta_any{kast::ParticleEmitter{}}).size() == 26, "particles: 26 fields");

    const std::pair<const char*, nlohmann::ordered_json> bad[] = {
        {"torus", {{"shape", "torus"}}},
        {"multiply", {{"blend", "multiply"}}},
        {"maxParticles", {{"maxParticles", 0}}},
        {"maxParticles", {{"maxParticles", 65537}}},
        {"startAlpha", {{"startAlpha", 1.5}}},
        {"endAlpha", {{"endAlpha", -0.1}}},
        {"rate", {{"rate", "fast"}}},
        {"rate", {{"rate", -1}}},
        {"startColor", {{"startColor", {17, 0, 0}}}},
        {"duration", {{"duration", 0}}},
        {"lifetimeMin", {{"lifetimeMin", 0}}},
        {"coneAngle", {{"coneAngle", 181}}},
        {"boxSize", {{"boxSize", {1, -1, 1}}}},
        {"burstCount", {{"burstCount", -1}}},
        {"looping", {{"looping", 1}}},
    };
    for (const auto& [mention, change] : bad) {
        nlohmann::ordered_json value = json;
        for (const auto& [key, v] : change.items())
            value[key] = v;
        error.clear();
        check(!kast::componentFromJson(type, value, error) && contains(error, mention),
              std::format("particles: {} rejected ({})", change.dump(), error).c_str());
    }

    // At scene load: a clear error naming the field, and the scene being loaded into is left as it was.
    kast::Scene scene;
    scene.create("Kept");
    nlohmann::ordered_json file = kast::sceneToJson(scene);
    nlohmann::ordered_json bogus = json;
    bogus["maxParticles"] = 0;
    file["entities"][0]["components"]["ParticleEmitter"] = bogus;
    error.clear();
    check(!kast::sceneFromJson(file, scene, error) && contains(error, "maxParticles") && contains(error, "65536"),
          "particles: a scene with a bad emitter does not load, and says why");
    check(scene.children(entt::null).size() == 1 && !scene.registry().all_of<kast::ParticleEmitter>(scene.children(entt::null)[0]),
          "particles: the failed load leaves the scene as it was");
    file["entities"][0]["components"]["ParticleEmitter"] = json;
    check(kast::sceneFromJson(file, scene, error) &&
              scene.registry().get<kast::ParticleEmitter>(scene.children(entt::null)[0]).shape == kast::ParticleShape::Box,
          "particles: a scene with an emitter loads");
}

// The emitter clock: frame-rate independent spawning with the fraction carried, bursts per cycle, one-shot end, the
// large-dt clamp, pause; particleSystem's states, and Play start / Stop.
void checkParticleClock()
{
    check(kast::clampParticleDt(5.0f) == kast::kMaxParticleDt && kast::clampParticleDt(-1.0f) == 0.0f &&
              kast::clampParticleDt(std::nanf("")) == 0.0f && kast::clampParticleDt(0.02f) == 0.02f,
          "particles: dt clamps to 0..0.1 s (NaN: 0)");
    kast::ParticleEmitter emitter;
    emitter.rate = 10.0f;
    const auto running = [] {
        kast::ParticleEmitterState state;
        state.playing = true;
        return state;
    };
    kast::ParticleEmitterState state = running();
    const int first = kast::advanceEmitter(emitter, state, 0.05f);
    const int second = kast::advanceEmitter(emitter, state, 0.05f);
    const int third = kast::advanceEmitter(emitter, state, 0.05f);
    check(first == 0 && second == 1 && third == 0 && near(state.accumulator, 0.5f) && state.spawned == 1,
          "particles: rate x dt with the fraction carried to the next frame");
    kast::ParticleEmitterState fine = running(), coarse = running();
    for (int i = 0; i < 300; ++i)
        kast::advanceEmitter(emitter, fine, 1.0f / 300.0f);
    for (int i = 0; i < 10; ++i)
        kast::advanceEmitter(emitter, coarse, 0.1f);
    check((fine.spawned == 10 || fine.spawned == 9) && coarse.spawned == 10 && std::abs(fine.simTime - coarse.simTime) < 1e-3,
          "particles: the same second spawns the same at 300 fps and at 10 fps (to float rounding)");
    kast::ParticleEmitterState hitch = running();
    check(kast::advanceEmitter(emitter, hitch, 30.0f) == 1 && near(static_cast<float>(hitch.simTime), 0.1f),
          "particles: a 30 s hitch simulates and spawns only 0.1 s");

    kast::ParticleEmitter bursts;
    bursts.rate = 0.0f;
    bursts.burstCount = 5;
    bursts.duration = 1.0f;
    state = running();
    check(kast::advanceEmitter(bursts, state, 0.05f) == 5, "particles: the burst goes out at the start");
    int later = 0;
    for (int i = 0; i < 18; ++i) // to 0.95 s
        later += kast::advanceEmitter(bursts, state, 0.05f);
    check(later == 0 && state.cycle == 0, "particles: no burst within the cycle");
    check(kast::advanceEmitter(bursts, state, 0.06f) == 5 && state.cycle == 1 && state.playing,
          "particles: looping: the next cycle starts with its burst");
    bursts.duration = 0.01f;
    const int many = kast::advanceEmitter(bursts, state, 0.1f);
    check(many >= 45 && many <= 55, "particles: one burst per cycle, several cycles in a frame");

    kast::ParticleEmitter oneShot = emitter;
    oneShot.looping = false;
    oneShot.duration = 0.2f;
    state = running();
    int total = 0;
    for (int i = 0; i < 10; ++i)
        total += kast::advanceEmitter(oneShot, state, 0.05f);
    check(total == 2 && !state.playing && near(static_cast<float>(state.simTime), 0.5f),
          "particles: a one-shot stops emitting after its cycle; its particles keep simulating");
    state = running();
    state.paused = true;
    check(kast::advanceEmitter(emitter, state, 1.0f) == 0 && state.simTime == 0.0, "particles: paused: nothing moves");

    kast::Scene scene;
    entt::registry& registry = scene.registry();
    const entt::entity a = scene.create("A");
    const entt::entity b = scene.create("B");
    const entt::entity c = scene.create("C");
    registry.emplace<kast::ParticleEmitter>(a, emitter);
    registry.emplace<kast::ParticleEmitter>(b, emitter);
    registry.emplace<kast::ParticleEmitter>(c).playOnStart = false;
    kast::particleSystem(scene, 0.1f);
    const auto* sa = registry.try_get<kast::ParticleEmitterState>(a);
    const auto* sb = registry.try_get<kast::ParticleEmitterState>(b);
    const auto* sc = registry.try_get<kast::ParticleEmitterState>(c);
    check(sa && sb && sc && sa->playing && !sc->playing && sa->spawned == 1 && sc->spawned == 0,
          "particles: particleSystem makes the states, playing if playOnStart");
    check(sa && sb && sa->id != sb->id && sa->seed != sb->seed, "particles: each emitter has its own id and seed");
    const uint64_t oldId = sa ? sa->id : 0;
    registry.remove<kast::ParticleEmitter>(a);
    kast::particleSystem(scene, 0.1f);
    check(!registry.all_of<kast::ParticleEmitterState>(a), "particles: removing the emitter drops its state");
    registry.emplace<kast::ParticleEmitter>(a, emitter);
    kast::particleSystem(scene, 0.1f);
    check(registry.get<kast::ParticleEmitterState>(a).id != oldId, "particles: re-adding starts a fresh emitter");

    // Play starts afresh from the edit mode preview; Stop's restored scene has no state until the preview ticks again.
    Document doc;
    resetToDefault(doc);
    const entt::entity cube = doc.scene.children(entt::null)[2];
    doc.scene.registry().emplace<kast::ParticleEmitter>(cube, emitter);
    for (int i = 0; i < 10; ++i)
        updatePlay(doc, 0.1f);
    const auto* edit = doc.scene.registry().try_get<kast::ParticleEmitterState>(cube);
    check(edit && edit->spawned == 10, "particles: edit mode previews the emitter");
    const uint64_t editId = edit ? edit->id : 0;
    startPlay(doc);
    updatePlay(doc, 0.1f);
    const auto* play = doc.scene.registry().try_get<kast::ParticleEmitterState>(cube);
    check(play && play->id != editId && play->spawned == 1, "particles: Play starts the emitter afresh");
    const uint64_t playId = play ? play->id : 0;
    stopPlay(doc);
    const entt::entity restored = doc.scene.children(entt::null)[2];
    check(!doc.scene.registry().all_of<kast::ParticleEmitterState>(restored), "particles: Stop drops the Play state");
    updatePlay(doc, 0.1f);
    const auto* after = doc.scene.registry().try_get<kast::ParticleEmitterState>(restored);
    check(after && after->id != playId && after->spawned == 1, "particles: after Stop the preview starts again");
}

// The culling bound: shape reach (scaled), flight at the top speed for the longest life, the fall, half the largest size.
void checkParticleBounds()
{
    kast::ParticleEmitter emitter; // radius 0.25, speed <= 1, life <= 3, size <= 0.5 x endSize 2
    check(near(kast::particleReach(emitter, 1.0f), 0.25f + 3.0f + 0.5f), "particles: default reach");
    emitter.gravity = {0.0f, -2.0f, 0.0f};
    check(near(kast::particleReach(emitter, 1.0f), 0.25f + 3.0f + 9.0f + 0.5f), "particles: gravity adds g t^2 / 2");
    emitter.gravity = {0.0f, 0.0f, 0.0f};
    check(near(kast::particleReach(emitter, 2.0f), 0.5f + 3.0f + 0.5f), "particles: world space: scale grows the shape only");
    emitter.space = kast::ParticleSpace::Local;
    check(near(kast::particleReach(emitter, 2.0f), 0.5f + 6.0f + 0.5f), "particles: local space: scale grows the flight too");
    emitter.space = kast::ParticleSpace::World;
    emitter.shape = kast::ParticleShape::Box;
    emitter.boxSize = {2.0f, 0.0f, 0.0f};
    emitter.speedMin = -4.0f; // either order, either sign
    check(near(kast::particleReach(emitter, 1.0f), 1.0f + 12.0f + 0.5f), "particles: box half diagonal, |speed|");

    kast::ParticleEmitter small;
    small.lifetimeMin = small.lifetimeMax = 1.0f;
    float world[16];
    kast::mtxFromTrs(world, {10.0f, 0.0f, -5.0f}, bx::Quaternion{bx::InitIdentity}, {1.0f, 3.0f, 1.0f});
    const kast::Aabb box = kast::particleBounds(small, world);
    const float reach = 0.25f * 3.0f + 1.0f + 0.5f;
    check(near(box.min, {10.0f - reach, -reach, -5.0f - reach}) && near(box.max, {10.0f + reach, reach, -5.0f + reach}),
          "particles: bounds around the emitter's world position, by the largest scale axis");
    float view[16], proj[16], viewProj[16];
    bx::mtxLookAt(view, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, bx::Handedness::Right);
    bx::mtxProj(proj, 60.0f, 1.0f, 0.1f, 100.0f, false, bx::Handedness::Right);
    bx::mtxMul(viewProj, view, proj);
    const kast::Frustum frustum = kast::frustumFromViewProj(viewProj, false);
    kast::mtxFromTrs(world, {0.0f, 0.0f, -10.0f}, bx::Quaternion{bx::InitIdentity}, {1.0f, 1.0f, 1.0f});
    check(kast::intersects(frustum, kast::particleBounds(small, world)), "particles: an emitter in view is drawn");
    kast::mtxFromTrs(world, {0.0f, 0.0f, 10.0f}, bx::Quaternion{bx::InitIdentity}, {1.0f, 1.0f, 1.0f});
    check(!kast::intersects(frustum, kast::particleBounds(small, world)), "particles: one behind the camera is culled");
    small.speedMin = small.speedMax = 30.0f; // flies into view within its life
    check(kast::intersects(frustum, kast::particleBounds(small, world)), "particles: unless its particles can fly into view");
}

// Headless bgfx (Noop renderer): without compute, one warning and no GPU work; with it (Noop claims compute support)
// the renderer's emitter buffers come and go with emitters, scenes and Play, and nothing is left after shutdown.
void checkParticlesNoop()
{
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init)) {
        check(false, "particles: headless bgfx starts");
        return;
    }
    const auto handles = [] {
        bgfx::frame(); // frees what was destroyed
        const bgfx::Stats* stats = bgfx::getStats();
        return std::array<uint32_t, 4>{stats->numDynamicVertexBuffers, stats->numVertexBuffers, stats->numPrograms,
                                       stats->numUniforms};
    };
    const auto buffers = [&] { return handles()[0]; };
    const std::array<uint32_t, 4> start = handles();

    kast::Scene scene;
    const entt::entity a = scene.create("A");
    const entt::entity b = scene.create("B");
    scene.registry().emplace<kast::ParticleEmitter>(a);
    scene.registry().emplace<kast::ParticleEmitter>(b);
    kast::particleSystem(scene, 0.1f);
    {
        const std::string warning = "Particles: the GPU has no compute shader support";
        const auto count = [&] {
            return std::ranges::count_if(kast::log::lines(), [&](const std::string& line) { return contains(line, warning); });
        };
        const auto before = count();
        kast::ParticleRenderer noCompute;
        noCompute.init(0);
        const bool none = noCompute.simulate(scene, 0, nullptr).empty() && noCompute.simulate(scene, 0, nullptr).empty();
        check(none && noCompute.emitters() == 0 && buffers() == start[0], "particles: no compute: nothing simulated or drawn");
        check(count() - before == 1, "particles: no compute: one warning");
        noCompute.shutdown();
        check(handles() == start, "particles: no compute: no GPU handles");
    }

    { // off screen: not drawn, still simulated (its buffer stays and is stepped, so there's no pop on return)
        kast::ParticleRenderer particles;
        particles.init(bgfx::getCaps()->supported);
        float away[16], proj[16], viewProj[16];
        bx::mtxLookAt(away, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 20.0f}, {0.0f, 1.0f, 0.0f}, bx::Handedness::Right);
        bx::mtxProj(proj, 60.0f, 1.0f, 0.1f, 100.0f, false, bx::Handedness::Right);
        bx::mtxMul(viewProj, away, proj);
        const kast::Frustum frustum = kast::frustumFromViewProj(viewProj, false);
        const size_t shown = particles.simulate(scene, 0, nullptr).size();
        const size_t culled = particles.simulate(scene, 0, &frustum).size();
        check(shown == 2 && culled == 0 && particles.emitters() == 2, "particles: off screen: culled, still simulated");
        particles.shutdown();
        check(handles() == start, "particles: ParticleRenderer shutdown frees everything");
    }

    kast::Renderer renderer;
    renderer.init();
    const uint32_t base = buffers();
    float view[16], proj[16];
    bx::mtxLookAt(view, {0.0f, 1.0f, 5.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, bx::Handedness::Right);
    bx::mtxProj(proj, 60.0f, 1.0f, 0.1f, 100.0f, false, bx::Handedness::Right);
    const auto render = [&](kast::Scene& s) {
        kast::particleSystem(s, 0.1f);
        renderer.render(s, {}, kast::Renderer::kShadowViews, view, proj, 0);
        return buffers() - base;
    };
    const uint32_t two = render(scene);
    scene.registry().get<kast::ParticleEmitter>(a).maxParticles = 50;
    const uint32_t resized = render(scene);
    scene.registry().remove<kast::ParticleEmitter>(a);
    const uint32_t removed = render(scene);
    scene.destroy(b);
    const uint32_t destroyed = render(scene);
    check(two == 2 && resized == 2 && removed == 1 && destroyed == 0,
          "particles: a buffer per emitter: remade on resize, freed with the component and the entity");
    kast::Scene other;
    other.registry().emplace<kast::ParticleEmitter>(other.create("C"));
    const uint32_t switched = render(other);
    other.registry().clear<kast::ParticleEmitterState>(); // as startPlay does
    const uint32_t replayed = render(other);
    std::string error;
    const nlohmann::ordered_json snapshot = kast::sceneToJson(other);
    check(kast::sceneFromJson(snapshot, other, error), "particles: the snapshot restores");
    const uint32_t restored = render(other);
    const uint32_t back = render(scene);
    check(switched == 1 && replayed == 1 && restored == 1 && back == 0,
          "particles: scene switch, Play start and Stop's restore replace the buffers, never add to them");
    render(other);
    renderer.shutdown();
    const std::array<uint32_t, 4> end = handles();
    kast::log::info(std::format("particles leak check: dynamic vertex buffers {} at start, {} after Renderer init; over "
                                "that: 2 emitters +{}, one resized +{}, component removed +{}, entity destroyed +{}, scene "
                                "switch +{}, Play start +{}, Stop +{}, back to the empty scene +{}; after shutdown {}. "
                                "Vertex buffers {} -> {}, programs {} -> {}, uniforms {} -> {}",
                                start[0], base, two, resized, removed, destroyed, switched, replayed, restored, back, end[0],
                                start[1], end[1], start[2], end[2], start[3], end[3]));
    check(end == start, "particles: Renderer shutdown leaves no GPU handles");
    bgfx::shutdown();
}

// --probe-png <png> <fx,fy,r,g,b,tolerance>...: each pixel at (fx, fy) (0..1 across and down the image) is within
// tolerance of r, g, b (0..255) per channel. Screenshot tests use it to read rendered colors.
int probePng(int argc, char** argv)
{
    std::string bytes;
    const std::optional<kast::MipImage> image = kast::readFile(argv[2], bytes) ? kast::decodeImage(bytes, false) : std::nullopt;
    if (!image) {
        std::printf("probe: cannot read '%s' as PNG\n", argv[2]);
        return 1;
    }
    int failed = 0;
    for (int i = 3; i < argc; ++i) {
        float fx = 0, fy = 0;
        int r = 0, g = 0, b = 0, tolerance = 0;
        if (std::sscanf(argv[i], "%f,%f,%d,%d,%d,%d", &fx, &fy, &r, &g, &b, &tolerance) != 6) {
            std::printf("probe: bad probe '%s' (expected fx,fy,r,g,b,tolerance)\n", argv[i]);
            return 1;
        }
        const int x = std::clamp(static_cast<int>(fx * image->width), 0, image->width - 1);
        const int y = std::clamp(static_cast<int>(fy * image->height), 0, image->height - 1);
        const uint8_t* p = &image->rgba[(static_cast<size_t>(y) * image->width + x) * 4];
        const bool ok = std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
        std::printf("probe %s (%d, %d): %d %d %d, expected %d %d %d +-%d\n", ok ? "ok  " : "FAIL", x, y, p[0], p[1], p[2], r, g,
                    b, tolerance);
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}

// --diff-png <a.png> <b.png> <maxMeanAbsDiff> [x,y,w,h[,bx,by]]: the mean absolute difference per RGB channel (0..255)
// over every pixel of two same-sized images is at most maxMeanAbsDiff. Prints the mean and the largest single
// difference. With a rect, only the w x h pixels at x, y (from the top left) are compared, at bx, by in b if given (e.g.
// an editor shot's Game view against a player screenshot of the view's size).
int diffPng(int argc, char** argv)
{
    std::array<std::optional<kast::MipImage>, 2> images;
    for (size_t i = 0; i < 2; ++i) {
        std::string bytes;
        if (kast::readFile(argv[2 + i], bytes))
            images[i] = kast::decodeImage(bytes, false);
        if (!images[i]) {
            std::printf("diff: cannot read '%s' as PNG\n", argv[2 + i]);
            return 1;
        }
    }
    const kast::MipImage& a = *images[0];
    const kast::MipImage& b = *images[1];
    int x = 0, y = 0, w = a.width, h = a.height, bx = -1, by = -1;
    if (argc == 6) {
        const int fields = std::sscanf(argv[5], "%d,%d,%d,%d,%d,%d", &x, &y, &w, &h, &bx, &by);
        if (fields != 4 && fields != 6) {
            std::printf("diff: bad rect '%s' (expected x,y,w,h or x,y,w,h,bx,by)\n", argv[5]);
            return 1;
        }
    } else if (a.width != b.width || a.height != b.height) {
        std::printf("diff FAIL: sizes differ (%dx%d, %dx%d)\n", a.width, a.height, b.width, b.height);
        return 1;
    }
    if (bx < 0) {
        bx = x;
        by = y;
    }
    const auto fits = [&](const kast::MipImage& image, int left, int top) {
        return left >= 0 && top >= 0 && w > 0 && h > 0 && left + w <= image.width && top + h <= image.height;
    };
    if (!fits(a, x, y) || !fits(b, bx, by)) {
        std::printf("diff FAIL: the rect %s is outside an image (%dx%d, %dx%d)\n", argv[5], a.width, a.height, b.width,
                    b.height);
        return 1;
    }
    const double limit = std::atof(argv[4]);
    double sum = 0.0;
    int max = 0;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col) {
            const size_t pa = (static_cast<size_t>(y + row) * a.width + x + col) * 4;
            const size_t pb = (static_cast<size_t>(by + row) * b.width + bx + col) * 4;
            for (size_t c = 0; c < 3; ++c) {
                const int d = std::abs(a.rgba[pa + c] - b.rgba[pb + c]);
                sum += d;
                max = std::max(max, d);
            }
        }
    const double mean = sum / (static_cast<double>(w) * h * 3.0);
    const bool ok = mean <= limit;
    std::printf("diff %s: mean %.6f (limit %.6f), max %d over %dx%d\n", ok ? "ok" : "FAIL", mean, limit, max, w, h);
    return ok ? 0 : 1;
}

void checkPostProcessing()
{
    // Shader math mirrors.
    check(kast::exposureScale(0.0f) == 1.0f && near(kast::exposureScale(1.0f), 2.0f) && near(kast::exposureScale(-1.0f), 0.5f),
          "exposure: EV 0 identity, +1 doubles, -1 halves");
    bool monotonic = true;
    for (float x = 0.0f, last = -1.0f; x < 100.0f; x += 0.05f) {
        const float y = kast::acesTonemap(x);
        monotonic = monotonic && y >= last;
        last = y;
    }
    check(monotonic, "ACES monotonic");
    check(kast::acesTonemap(0.0f) == 0.0f, "ACES 0 -> 0");
    check(kast::acesTonemap(4.0f) < 1.0f && kast::acesTonemap(1000.0f) > 0.99f && kast::acesTonemap(1000.0f) <= 1.0f,
          "ACES: bright values stay below 1, large ones approach it");
    check(kast::fogFactor(5.0f, 0.1f, 10.0f) == 0.0f && kast::fogFactor(10.0f, 0.1f, 10.0f) == 0.0f,
          "fog: none up to start");
    check(kast::fogFactor(1000.0f, 0.0f, 0.0f) == 0.0f, "fog: density 0 means none");
    bool fogMonotonic = true;
    for (float d = 0.0f, last = 0.0f; d < 200.0f; d += 0.5f) {
        const float f = kast::fogFactor(d, 0.05f, 2.0f);
        fogMonotonic = fogMonotonic && f >= last && f <= 1.0f;
        last = f;
    }
    check(fogMonotonic && kast::fogFactor(1000.0f, 0.05f, 2.0f) > 0.999f, "fog: monotonic toward 1");
    check(near(kast::fogFactor(12.0f, 0.1f, 2.0f), 1.0f - std::exp(-1.0f)), "fog: exp2 at density * distance 1");
    check(near(kast::srgbToLinear(0.0f), 0.0f) && near(kast::srgbToLinear(1.0f), 1.0f) &&
              near(kast::srgbToLinear(0.5f), 0.214041f),
          "sRGB decode");
    kast::ScenePostProcessing fogged;
    check(near(kast::sceneClearColor(fogged, 0xffffffff), {1, 1, 1}), "clear: background without fog");
    fogged.fog.enabled = true;
    check(near(kast::sceneClearColor(fogged, 0xffffffff), fogged.fog.color), "clear: fog color with fog");

    // Scene file: round trip, defaults, partial blocks, invalid values.
    kast::Scene scene;
    scene.create("Thing");
    kast::ScenePostProcessing& post = scene.postProcessing;
    post.tonemapping = {true, 1.5f, kast::Tonemap::Aces};
    post.bloom = {true, 0.8f, 1.25f, 0.7f};
    post.colorGrading = {true, 1.2f, 0.6f, {0.9f, 0.8f, 0.7f}, -0.4f};
    post.vignette = {true, 0.6f, 0.25f};
    post.fxaa.enabled = true;
    post.fog = {true, {0.3f, 0.4f, 0.5f}, 0.08f, 3.0f};
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    check(json["version"] == kast::kSceneFileVersion && json.contains("postProcessing"), "saved with postProcessing, current version");
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(json, loaded, error), "post scene loads");
    const kast::ScenePostProcessing& p = loaded.postProcessing;
    check(p.tonemapping.enabled && p.tonemapping.exposure == 1.5f && p.tonemapping.op == kast::Tonemap::Aces,
          "tonemapping round trip");
    check(p.bloom.enabled && p.bloom.threshold == 0.8f && p.bloom.intensity == 1.25f && p.bloom.radius == 0.7f,
          "bloom round trip");
    check(p.colorGrading.enabled && p.colorGrading.contrast == 1.2f && p.colorGrading.saturation == 0.6f &&
              near(p.colorGrading.colorFilter, {0.9f, 0.8f, 0.7f}) && p.colorGrading.temperature == -0.4f,
          "color grading round trip");
    check(p.vignette.enabled && p.vignette.intensity == 0.6f && p.vignette.smoothness == 0.25f, "vignette round trip");
    check(p.fxaa.enabled, "fxaa round trip");
    check(p.fog.enabled && near(p.fog.color, {0.3f, 0.4f, 0.5f}) && p.fog.density == 0.08f && p.fog.start == 3.0f,
          "fog round trip");
    check(kast::sceneToJson(loaded) == json, "post: second save is identical");

    const nlohmann::ordered_json defaultJson = kast::sceneToJson(kast::Scene())["postProcessing"];
    nlohmann::ordered_json without = json;
    without.erase("postProcessing");
    check(kast::sceneFromJson(without, loaded, error) && kast::sceneToJson(loaded)["postProcessing"] == defaultJson,
          "missing postProcessing: defaults");
    const kast::ScenePostProcessing defaults;
    check(!defaults.tonemapping.enabled && defaults.tonemapping.op == kast::Tonemap::None &&
              defaults.tonemapping.exposure == 0.0f && !defaults.bloom.enabled && !defaults.colorGrading.enabled &&
              !defaults.vignette.enabled && !defaults.fxaa.enabled && !defaults.fog.enabled,
          "post defaults: everything off");
    nlohmann::ordered_json partial = without;
    partial["postProcessing"] = nlohmann::ordered_json::parse(R"({"bloom": {"enabled": true}, "fog": {"density": 0.2}})");
    const kast::ScenePostProcessing& q = loaded.postProcessing;
    check(kast::sceneFromJson(partial, loaded, error) && q.bloom.enabled && q.bloom.threshold == defaults.bloom.threshold &&
              q.bloom.intensity == defaults.bloom.intensity && !q.fog.enabled && q.fog.density == 0.2f &&
              near(q.fog.color, defaults.fog.color) && !q.vignette.enabled,
          "partial postProcessing: missing fields default");

    const std::pair<const char*, const char*> bad[] = {
        {R"(3)", "postProcessing"},
        {R"({"bloom": 1})", "bloom"},
        {R"({"sharpen": {}})", "sharpen"},
        {R"({"bloom": {"glow": 1}})", "glow"},
        {R"({"bloom": {"enabled": 1}})", "enabled"},
        {R"({"bloom": {"threshold": -1}})", "threshold"},
        {R"({"bloom": {"intensity": "lots"}})", "intensity"},
        {R"({"bloom": {"radius": 1.5}})", "radius"},
        {R"({"tonemapping": {"operator": "Reinhard"}})", "operator"},
        {R"({"tonemapping": {"operator": 1}})", "operator"},
        {R"({"tonemapping": {"exposure": 11}})", "exposure"},
        {R"({"colorGrading": {"contrast": 2.5}})", "contrast"},
        {R"({"colorGrading": {"saturation": -0.1}})", "saturation"},
        {R"({"colorGrading": {"colorFilter": [1, 1]}})", "colorFilter"},
        {R"({"colorGrading": {"colorFilter": [1, 1.2, 1]}})", "colorFilter"},
        {R"({"colorGrading": {"temperature": -2}})", "temperature"},
        {R"({"vignette": {"intensity": 2}})", "intensity"},
        {R"({"vignette": {"smoothness": 0}})", "smoothness"},
        {R"({"fxaa": {"enabled": "yes"}})", "enabled"},
        {R"({"fog": {"density": -0.1}})", "density"},
        {R"({"fog": {"start": -1}})", "start"},
        {R"({"fog": {"color": [0.5, 0.5, -1]}})", "color"},
    };
    const std::filesystem::path dir = looseTestDir("kast_post_check");
    const std::string path = (dir / "bad.scene").string();
    kast::Scene current;
    current.create("Keep me");
    current.postProcessing.vignette.intensity = 0.9f;
    for (const auto& [text, mention] : bad) {
        nlohmann::ordered_json file = json;
        file["postProcessing"] = nlohmann::ordered_json::parse(text);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << file.dump();
        std::string reason;
        const bool ok = kast::loadScene(path, current, reason);
        check(!ok && reason.find(mention) != std::string::npos, text);
        if (!ok)
            kast::log::info(std::format("expected error ({}): {}", text, reason));
    }
    check(current.children(entt::null).size() == 1 && current.postProcessing.vignette.intensity == 0.9f &&
              !current.postProcessing.bloom.enabled,
          "failed post loads leave the scene untouched");
}

// --- Materials ---

void checkMaterialVersion2();
void checkShaders();
void checkShaderBuild();
void checkShaderLibraryAsync();
void checkShaderGraphs();
void checkShaderGraphNodes();
void checkShaderGraphEdits();
void checkShaderParamEditing();
void checkSceneCopies();

std::string materialError(const std::string& text)
{
    kast::Material material;
    std::string error;
    kast::materialFromJson(nlohmann::ordered_json::parse(text), material, error);
    return error;
}

void checkMaterialFiles()
{
    kast::Material m;
    std::string error;
    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 1, "baseColor": [0.1, 0.2, 0.3, 0.4],
        "baseColorTexture": "textures/a.png", "metallic": 1, "roughness": 0.25, "metallicRoughnessTexture": "textures/mr.png",
        "emissiveEnabled": true, "emissive": [0, 0.5, 1], "emissiveIntensity": 2.5, "alphaMode": "mask", "alphaCutoff": 0.75})"), m, error),
          "a .mat with every field parses");
    check(m.baseColor == std::array<float, 4>{0.1f, 0.2f, 0.3f, 0.4f} && m.baseColorTexture == "textures/a.png" &&
              m.metallic == 1.0f && m.roughness == 0.25f && m.metallicRoughnessTexture == "textures/mr.png" &&
              m.emissiveEnabled && near(m.emissive, {0, 0.5f, 1}) && m.emissiveIntensity == 2.5f &&
              m.alphaMode == kast::AlphaMode::Mask && m.alphaCutoff == 0.75f,
          ".mat field values");
    check(near(kast::emissiveRadiance(m), {0, 1.25f, 2.5f}), "emissive radiance is color * intensity");
    m.emissiveEnabled = false;
    check(near(kast::emissiveRadiance(m), {0, 0, 0}), "emission off: no emissive radiance");
    m.emissiveEnabled = true;
    kast::Material back;
    check(kast::materialFromJson(kast::materialToJson(m), back, error) && kast::materialToJson(back) == kast::materialToJson(m),
          ".mat round trip");

    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 1})"), m, error), "a .mat with only a version parses");
    check(m.baseColor == std::array<float, 4>{0.8f, 0.8f, 0.8f, 1.0f} && m.baseColorTexture.empty() && m.metallic == 0.0f &&
              m.roughness == 0.5f && m.metallicRoughnessTexture.empty() && !m.emissiveEnabled && near(m.emissive, {0, 0, 0}) &&
              m.emissiveIntensity == 1.0f && m.alphaMode == kast::AlphaMode::Opaque && m.alphaCutoff == 0.5f,
          "missing .mat fields get their defaults");

    const std::pair<const char*, const char*> bad[] = {
        {R"({"metallic": 0})", "\"version\" must be 1, 2 or 3"},
        {R"({"version": 4})", "\"version\" must be 1, 2 or 3"},
        {R"({"version": 0})", "\"version\" must be 1, 2 or 3"},
        {R"([1, 2])", "must be a JSON object"},
        {R"({"version": 1, "shininess": 3})", "unknown field \"shininess\""},
        {R"({"version": 1, "metallic": 1.5})", "\"metallic\" must be a number between 0 and 1"},
        {R"({"version": 1, "roughness": -0.1})", "\"roughness\" must be a number between 0 and 1"},
        {R"({"version": 1, "roughness": "rough"})", "\"roughness\" must be a number between 0 and 1"},
        {R"({"version": 1, "alphaCutoff": 2})", "\"alphaCutoff\" must be a number between 0 and 1"},
        {R"({"version": 1, "baseColor": [1, 1, 1]})", "\"baseColor\" must be an array of 4 numbers between 0 and 1"},
        {R"({"version": 1, "baseColor": [1, 1, 1, 1.2]})", "\"baseColor\" must be an array of 4 numbers"},
        {R"({"version": 1, "emissive": [1, "x", 1]})", "\"emissive\" must be an array of 3 numbers"},
        {R"({"version": 1, "emissive": [1, -0.5, 1]})", "\"emissive\" must be an array of 3 numbers between 0 and 1"},
        {R"({"version": 1, "emissiveEnabled": true, "emissive": [0, 2, 0]})", "\"emissive\" must be an array of 3 numbers between 0 and 1"},
        {R"({"version": 1, "emissiveEnabled": 1})", "\"emissiveEnabled\" must be true or false"},
        {R"({"version": 1, "emissiveIntensity": -1})", "\"emissiveIntensity\" must be a number >= 0"},
        {R"({"version": 1, "alphaMode": "glow"})", "\"alphaMode\" must be \"opaque\", \"mask\", \"blend\" or \"additive\""},
        {R"({"version": 2, "alphaMode": 2})", "\"alphaMode\" must be \"opaque\", \"mask\", \"blend\" or \"additive\""},
        {R"({"version": 2, "cullMode": "both"})", "\"cullMode\" must be \"none\", \"back\" or \"front\""},
        {R"({"version": 2, "cullMode": true})", "\"cullMode\" must be \"none\", \"back\" or \"front\""},
        {R"({"version": 2, "occlusionStrength": 1.5})", "\"occlusionStrength\" must be a number between 0 and 1"},
        {R"({"version": 2, "occlusionStrength": -0.1})", "\"occlusionStrength\" must be a number between 0 and 1"},
        {R"({"version": 2, "normalScale": "big"})", "\"normalScale\" must be a finite number"},
        {R"({"version": 2, "uvScale": [4]})", "\"uvScale\" must be an array of 2 finite numbers"},
        {R"({"version": 2, "uvScale": [4, "x"]})", "\"uvScale\" must be an array of 2 finite numbers"},
        {R"({"version": 2, "uvOffset": [0, 0, 0]})", "\"uvOffset\" must be an array of 2 finite numbers"},
        {R"({"version": 2, "uvOffset": 0.5})", "\"uvOffset\" must be an array of 2 finite numbers"},
        {R"({"version": 2, "unlit": "yes"})", "\"unlit\" must be true or false"},
        {R"({"version": 2, "normalTexture": 1})", "\"normalTexture\" must be a string"},
        {R"({"version": 2, "occlusionTexture": null})", "\"occlusionTexture\" must be a string"},
        {R"({"version": 2, "emissiveTexture": ["a.png"]})", "\"emissiveTexture\" must be a string"},
        {R"({"version": 1, "baseColorTexture": 7})", "\"baseColorTexture\" must be a string"},
    };
    for (const auto& [text, expected] : bad)
        check(contains(materialError(text), expected), std::format(".mat {} is rejected: {}", text, expected).c_str());

    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 1, "emissive": [0, 0.5, 1]})"), m, error) &&
              m.emissiveEnabled && near(m.emissive, {0, 0.5f, 1}) && m.emissiveIntensity == 1.0f,
          "legacy .mat: a non-black emissive turns emission on");
    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 1, "emissive": [0, 0, 0]})"), m, error) &&
              !m.emissiveEnabled,
          "legacy .mat: a black emissive leaves emission off");
    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 1, "emissive": [0, 2, 4]})"), m, error) &&
              m.emissiveEnabled && near(m.emissive, {0, 0.5f, 1}) && m.emissiveIntensity == 4.0f &&
              near(kast::emissiveRadiance(m), {0, 2, 4}),
          "legacy .mat: emissive above 1 moves into the intensity and renders the same");

    kast::Material field = kast::defaultMaterial();
    check(!kast::setMaterialField(field, "metallic", 3.0, error) && field.metallic == 0.0f,
          "a rejected field leaves the material unchanged");
    check(kast::materialField(field, "alphaMode") == nlohmann::ordered_json("opaque") && !kast::materialField(field, "nope"),
          "materialField by name");
    checkMaterialVersion2();
}

// Version 2 fields: normal / occlusion / emissive textures, UV transform, cull mode, unlit, additive.
void checkMaterialVersion2()
{
    using Json = nlohmann::ordered_json;
    check(kast::kMaterialFileVersion == 3, "material files are version 3");
    check(kast::materialFieldNames() ==
              std::vector<std::string>{"baseColor", "baseColorTexture", "metallic", "roughness", "metallicRoughnessTexture",
                                       "normalTexture", "normalScale", "occlusionTexture", "occlusionStrength",
                                       "emissiveEnabled", "emissive", "emissiveIntensity", "emissiveTexture", "uvScale",
                                       "uvOffset", "alphaMode", "alphaCutoff", "cullMode", "unlit", "shader", "params"},
          "material field names in file order");

    kast::Material m;
    std::string error;
    const Json full = Json::parse(R"({"version": 2, "baseColor": [0.1, 0.2, 0.3, 0.4], "baseColorTexture": "textures/a.png",
        "metallic": 0.5, "roughness": 0.25, "metallicRoughnessTexture": "textures/orm.png", "normalTexture": "textures/n.png",
        "normalScale": -1.5, "occlusionTexture": "textures/orm.png", "occlusionStrength": 0.75, "emissiveEnabled": true,
        "emissive": [1, 0.5, 0], "emissiveIntensity": 3, "emissiveTexture": "textures/e.png", "uvScale": [4, -2],
        "uvOffset": [0.25, 10.5], "alphaMode": "additive", "alphaCutoff": 0.5, "cullMode": "front", "unlit": true})");
    check(kast::materialFromJson(full, m, error), "a version 2 .mat with every field parses");
    check(m.normalTexture == "textures/n.png" && m.normalScale == -1.5f && m.occlusionTexture == "textures/orm.png" &&
              m.occlusionStrength == 0.75f && m.emissiveTexture == "textures/e.png" &&
              m.uvScale == std::array<float, 2>{4.0f, -2.0f} && m.uvOffset == std::array<float, 2>{0.25f, 10.5f} &&
              m.alphaMode == kast::AlphaMode::Additive && m.cullMode == kast::CullMode::Front && m.unlit,
          "version 2 field values (a packed ORM texture can be both occlusion and metallic-roughness)");
    Json full3 = full; // saved as version 3: the version 2 fields, then an empty shader and params
    full3["version"] = 3;
    full3["shader"] = "";
    full3["params"] = Json::object();
    check(kast::materialToJson(m) == full3, "a version 2 .mat round-trips exactly as version 3 (every field, file order)");
    kast::Material back;
    check(kast::materialFromJson(kast::materialToJson(m), back, error) && kast::materialToJson(back) == full3,
          "parse of the written file");
    for (const char* mode : {"none", "back", "front"})
        check(kast::setMaterialField(m, "cullMode", mode, error) && kast::materialField(m, "cullMode") == Json(mode),
              std::format("cullMode \"{}\" sets and reads back", mode).c_str());
    for (const char* mode : {"opaque", "mask", "blend", "additive"})
        check(kast::setMaterialField(m, "alphaMode", mode, error) && kast::materialField(m, "alphaMode") == Json(mode),
              std::format("alphaMode \"{}\" sets and reads back", mode).c_str());

    // Version 1 files load with every version 2 default (cull back included) and save as version 2.
    check(kast::materialFromJson(Json::parse(R"({"version": 1, "metallic": 1})"), m, error), "a version 1 .mat still loads");
    check(m.metallic == 1.0f && m.normalTexture.empty() && m.normalScale == 1.0f && m.occlusionTexture.empty() &&
              m.occlusionStrength == 1.0f && m.emissiveTexture.empty() && m.uvScale == std::array<float, 2>{1.0f, 1.0f} &&
              m.uvOffset == std::array<float, 2>{0.0f, 0.0f} && m.cullMode == kast::CullMode::Back && !m.unlit,
          "version 1: the new fields get their defaults");
    const Json saved = kast::materialToJson(m);
    check(saved["version"] == 3 && saved.size() == kast::materialFieldNames().size() + 1 && saved["cullMode"] == "back",
          "saving a version 1 material writes version 3 with every field");
    check(kast::defaultMaterial().cullMode == kast::CullMode::Back && !kast::defaultMaterial().unlit,
          "the default material culls back faces, like a .mat without cullMode");
    for (const std::filesystem::path& file : std::filesystem::directory_iterator(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "materials")) {
        if (file.filename() == "broken.mat")
            continue;
        check(kast::loadMaterial(file, m, error), std::format("{} loads ({})", file.filename().string(), error).c_str());
    }

    // setMaterialField / materialField (what kast.set/getMaterialProperty use) for each new field.
    const std::pair<const char*, Json> fields[] = {
        {"normalTexture", "t/n.png"}, {"normalScale", 2.5}, {"occlusionTexture", "t/ao.png"}, {"occlusionStrength", 0.5},
        {"emissiveTexture", "t/e.png"}, {"uvScale", Json::array({2, 3})}, {"uvOffset", Json::array({-1, 0.5})},
        {"cullMode", "none"}, {"unlit", true}, {"alphaMode", "additive"}};
    kast::Material f;
    for (const auto& [name, value] : fields)
        check(kast::setMaterialField(f, name, value, error) && kast::materialField(f, name) == value,
              std::format("setMaterialField / materialField \"{}\"", name).c_str());
    check(!kast::setMaterialField(f, "uvScale", Json::array({1, "x"}), error) && f.uvScale == std::array<float, 2>{2.0f, 3.0f},
          "a rejected uvScale leaves the material unchanged");
    const double inf = std::numeric_limits<double>::infinity(); // JSON text can't spell it; Lua and C++ can pass it
    check(!kast::setMaterialField(f, "uvScale", Json::array({inf, 1}), error) &&
              contains(error, "\"uvScale\" must be an array of 2 finite numbers"),
          "a non-finite uvScale is rejected");
    check(!kast::setMaterialField(f, "uvOffset", Json::array({0, std::nan("")}), error) &&
              contains(error, "\"uvOffset\" must be an array of 2 finite numbers"),
          "a NaN uvOffset is rejected");
    check(!kast::setMaterialField(f, "normalScale", -inf, error) && contains(error, "\"normalScale\" must be a finite number"),
          "a non-finite normalScale is rejected");
}

// .kshader files: params, generated code, error line mapping, real shaderc compiles, the library; .mat version 3.
void checkShaders()
{
    using Json = nlohmann::ordered_json;
    kast::ShaderSource source;
    std::string error;
    const std::string valid = "// every param type\n"
                              "param float speed = 2.5 range(0, 10);\n"
                              "param float plain;\n"
                              "  param vec2 scroll = vec2(0.1, -0.2); // trailing comment\n"
                              "param vec3 tint = vec3(0.5);\n"
                              "param vec4 data = vec4(1, 2, 3, 4);\n"
                              "param color glow = color(1.0, 0.5, 0.25);\n"
                              "param color shade;\n"
                              "param texture2D noise = black srgb;\n"
                              "param texture2D bumps = normal;\n"
                              "param texture2D mask;\n"
                              "void fragment()\n"
                              "{\n"
                              "    ALBEDO *= tint * speed;\n"
                              "}\n";
    check(kast::parseShader(valid, source, error), std::format("a .kshader with every param type parses ({})", error).c_str());
    const auto param = [&](const char* name) -> const kast::ShaderParam& {
        static const kast::ShaderParam none;
        const auto it = std::ranges::find(source.params, std::string(name), &kast::ShaderParam::name);
        return it == source.params.end() ? none : *it;
    };
    using Type = kast::ShaderParamType;
    check(source.params.size() == 10, "10 params");
    check(param("speed").type == Type::Float && param("speed").value[0] == 2.5f && param("speed").range &&
              (*param("speed").range)[0] == 0.0f && (*param("speed").range)[1] == 10.0f && param("speed").line == 2,
          "float with a default and a range, and its line");
    check(param("plain").type == Type::Float && param("plain").value[0] == 0.0f && !param("plain").range,
          "float without a default: 0, no range");
    check(param("scroll").type == Type::Vec2 && param("scroll").value[0] == 0.1f && param("scroll").value[1] == -0.2f &&
              param("scroll").line == 4,
          "vec2 (indented, trailing comment)");
    check(param("tint").type == Type::Vec3 && param("tint").value == std::array<float, 4>{0.5f, 0.5f, 0.5f, 0.0f},
          "vec3(x) fills every component");
    check(param("data").type == Type::Vec4 && param("data").value == std::array<float, 4>{1, 2, 3, 4}, "vec4");
    check(param("glow").type == Type::Color && param("glow").value == std::array<float, 4>{1.0f, 0.5f, 0.25f, 1.0f},
          "color(r, g, b): alpha 1");
    check(param("shade").type == Type::Color && param("shade").value == std::array<float, 4>{1, 1, 1, 1},
          "color without a default: white");
    check(param("noise").type == Type::Texture2D && param("noise").texture == kast::TextureDefault::Black &&
              param("noise").srgb,
          "texture2D black srgb");
    check(param("bumps").texture == kast::TextureDefault::Normal && !param("bumps").srgb, "texture2D normal, linear");
    check(param("mask").texture == kast::TextureDefault::White, "texture2D without a default: white");
    check(param("speed").slot == 0 && param("data").slot == 4 && param("shade").slot == 6 && param("noise").slot == 0 &&
              param("mask").slot == 2,
          "numeric params take u_kastParams slots in order, textures their own");
    check(std::ranges::count(source.code, '\n') == 15 && !contains(source.code, "param float") &&
              contains(source.code, "ALBEDO *= tint * speed;"),
          "param lines are blanked, the line count kept");
    check(!source.customNormal, "no NORMAL / NORMAL_MAP: the built-in normal mapping rule");
    check(kast::parseShader("void fragment() { NORMAL_MAP = vec3(0.5, 0.5, 1.0); }", source, error) && source.customNormal,
          "writing NORMAL_MAP turns on the always-applied normal map");

    const std::pair<const char*, const char*> bad[] = {
        {"void fragment() {}\nparam vec5 x;\n", "line 2: unknown param type 'vec5'"},
        {"\n\nparam float x = ;\nvoid fragment() {}", "line 3: a float default must be a number"},
        {"param float x = 1.0\nvoid fragment() {}", "line 1: expected ';' at the end"},
        {"param vec3 x = vec3(1, 2);\nvoid fragment() {}", "line 1: vec3(...) takes 3 numbers, not 2"},
        {"param vec2 x = vec3(1, 2, 3);\nvoid fragment() {}", "line 1: expected 'vec2', found 'vec3'"},
        {"param texture2D t = grey;\nvoid fragment() {}", "line 1: a texture2D default must be white, black or normal"},
        {"param vec2 x range(0, 1);\nvoid fragment() {}", "line 1: range(min, max) is only for float params"},
        {"param float x range(1, 0);\nvoid fragment() {}", "line 1: range(min, max) needs min < max"},
        {"param float x srgb;\nvoid fragment() {}", "line 1: srgb is only for texture2D params"},
        {"param float x = 1; extra\nvoid fragment() {}", "line 1: unexpected 'extra' after ';'"},
        {"param float ALBEDO = 1.0;\nvoid fragment() {}", "line 1: param 'ALBEDO' has the name of a built-in"},
        {"param vec2 UV;\nvoid fragment() {}", "line 1: param 'UV' has the name of a built-in"},
        {"param float sampleBaseColor;\nvoid fragment() {}", "param 'sampleBaseColor' has the name of a built-in"},
        {"param float float;\nvoid fragment() {}", "line 1: param 'float' has a reserved name"},
        {"param float u_time;\nvoid fragment() {}", "line 1: param 'u_time' has a reserved name"},
        {"param float a;\nparam vec3 a;\nvoid fragment() {}", "line 2: param 'a' is already declared on line 1"},
        {"param float speed;\n", "no fragment() function"},
        {"// void fragment() {}\nvoid fragments() {}\n", "no fragment() function"},
        {"/* void fragment() {} */\n", "no fragment() function"},
    };
    for (const auto& [text, expected] : bad) {
        const bool ok = kast::parseShader(text, source, error);
        check(!ok && contains(error, expected), std::format(".kshader {} is rejected: {} (got: {})", text, expected, error).c_str());
    }
    // The sampler-stage limit: 4 texture2D params (stages 10..13), 16 numeric ones.
    std::string textures, numbers;
    for (int i = 0; i < 5; ++i)
        textures += std::format("param texture2D t{};\n", i);
    check(!kast::parseShader(textures + "void fragment() {}", source, error) &&
              contains(error, "line 5: too many texture2D params: at most 4 (sampler stages 10..13; a draw has 16, the mesh "
                              "shader uses 0..9 and keeps 14..15 for SCREEN_TEXTURE and DEPTH_TEXTURE)"),
          std::format("a 5th texture2D param is rejected with the sampler limit ({})", error).c_str());
    for (int i = 0; i < 17; ++i)
        numbers += std::format("param float f{};\n", i);
    check(!kast::parseShader(numbers + "void fragment() {}", source, error) &&
              contains(error, "line 17: too many params: at most 16"),
          "a 17th numeric param is rejected");

    // Material values: defaults unless the .mat sets them; wrong types fail; unknown names are reported, not failed.
    check(kast::parseShader(valid, source, error), "parse again");
    kast::Material material;
    material.params["speed"] = {{4.0f}, ""};
    material.params["glow"] = {{0.0f, 1.0f, 0.0f}, ""};
    material.params["noise"] = {{}, "textures/noise.png"};
    const auto values = kast::shaderParamValues(material, source);
    check(values[0][0] == 4.0f && values[2] == std::array<float, 4>{0.1f, -0.2f, 0.0f, 0.0f} &&
              values[5] == std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f} && values[6] == std::array<float, 4>{1, 1, 1, 1},
          "u_kastParams: material values over defaults (a 3-number color gets alpha 1)");
    check(kast::shaderParamTexture(material, param("noise")) == "textures/noise.png" &&
              kast::shaderParamTexture(material, param("mask")).empty(),
          "texture params: the material's path, else the default");
    std::vector<std::string> unknown;
    material.params["gone"] = {{1.0f}, ""};
    check(kast::checkShaderParams(material, source, error, unknown) && unknown == std::vector<std::string>{"gone"},
          "a value for an undeclared param is reported as unknown, not an error");
    for (const auto& [name, value, expected] :
         std::vector<std::tuple<std::string, kast::ShaderParamValue, std::string>>{
             {"speed", {{1, 2}, ""}, "param 'speed' is a float: its value must be a number"},
             {"scroll", {{1, 2, 3}, ""}, "param 'scroll' is a vec2: its value must be an array of 2 numbers"},
             {"glow", {{1, 2}, ""}, "param 'glow' is a color: its value must be an array of 3 or 4 numbers"},
             {"noise", {{1}, ""}, "param 'noise' is a texture2D: its value must be a texture path"},
             {"tint", {{}, "a.png"}, "param 'tint' is a vec3: its value must be an array of 3 numbers"}}) {
        kast::Material wrong = material;
        wrong.params[name] = value;
        check(!kast::checkShaderParams(wrong, source, error, unknown) && error == expected,
              std::format("wrong-typed value for '{}' fails: {}", name, error).c_str());
    }

    // .mat version 3: "shader" and "params" round-trip; versions 1 and 2 still load; params must have a value shape.
    kast::Material m;
    const Json v3 = Json::parse(R"({"version": 3, "shader": "shaders/water.kshader",
        "params": {"speed": 2.5, "tint": [1, 0.5, 0], "noise": "textures/noise.png", "data": [1, 2, 3, 4]}})");
    check(kast::materialFromJson(v3, m, error) && m.shader == "shaders/water.kshader" && m.params.size() == 4 &&
              m.params["speed"].numbers == std::vector<float>{2.5f} && m.params["tint"].numbers.size() == 3 &&
              m.params["noise"].texture == "textures/noise.png" && m.params["noise"].numbers.empty(),
          std::format("a version 3 .mat with a shader and params loads ({})", error).c_str());
    const Json saved = kast::materialToJson(m);
    std::vector<std::string> keys;
    for (const auto& [key, value] : saved.items())
        keys.push_back(key);
    std::vector<std::string> expectedKeys{"version"};
    expectedKeys.insert(expectedKeys.end(), kast::materialFieldNames().begin(), kast::materialFieldNames().end());
    check(saved["version"] == 3 && keys == expectedKeys, "saving writes version 3, every field in materialFieldNames order");
    check(saved["shader"] == "shaders/water.kshader" &&
              nlohmann::json::parse(saved["params"].dump()) == nlohmann::json::parse(v3["params"].dump()),
          "shader and params round-trip (params sorted by name)");
    kast::Material back;
    check(kast::materialFromJson(saved, back, error) && kast::materialToJson(back) == saved && back.params == m.params,
          "a saved version 3 .mat parses back the same");
    for (const char* text : {R"({"version": 1, "metallic": 0.5})", R"({"version": 2, "unlit": true})",
                             R"({"version": 3, "shader": ""})"})
        check(kast::materialFromJson(Json::parse(text), m, error) && m.shader.empty() && m.params.empty(),
              std::format("{} loads with the built-in shader", text).c_str());
    for (const auto& [text, expected] : std::vector<std::pair<const char*, const char*>>{
             {R"({"version": 3, "shader": 5})", "\"shader\" must be a string"},
             {R"({"version": 3, "params": [1]})", "\"params\" must be an object"},
             {R"({"version": 3, "params": {"a": true}})", "\"params\": \"a\" must be a finite number, an array of 2 to 4"},
             {R"({"version": 3, "params": {"a": [1]}})", "\"params\": \"a\" must be a finite number, an array of 2 to 4"},
             {R"({"version": 3, "params": {"a": [1, 2, 3, 4, 5]}})", "\"params\": \"a\" must be"},
             {R"({"version": 3, "params": {"a": [1, "x"]}})", "\"params\": \"a\" must be"}})
        check(contains(materialError(text), expected), std::format(".mat {} is rejected: {}", text, expected).c_str());

    // Generated code: params and markers at the template's //@user line; the template's built-ins are all reserved.
    const std::filesystem::path shaderDir = std::filesystem::path(KAST_SOURCE_DIR) / "engine/shaders";
    std::string templateText = readText(shaderDir / "fs_mesh.sc");
    std::smatch match;
    const std::regex define(R"(#define (\w+) )");
    bool reserved = true;
    for (const char* file : {"fs_mesh.sc", "vs_mesh.sc", "vs_mesh_skinned.sc", "vs_mesh_instanced.sc", "mesh.sh"}) {
        const std::string text = readText(shaderDir / file);
        check(std::string_view(file) == "mesh.sh" || contains(text, "\n//@user"),
              std::format("{} has the //@user line", file).c_str());
        for (auto it = std::sregex_iterator(text.begin(), text.end(), define); it != std::sregex_iterator(); ++it)
            if (const std::string name = (*it)[1].str(); !name.starts_with("MAX_") && !name.starts_with("CSM_") &&
                                                          !name.starts_with("POINT_SHADOW_") &&
                                                          std::ranges::find(kast::shaderBuiltinNames(), name) ==
                                                              kast::shaderBuiltinNames().end()) {
                reserved = false;
                kast::log::error(std::format("{}'s #define {} is not in shaderBuiltinNames()", file, name));
            }
    }
    check(reserved, "every built-in the templates define is a reserved param name");
    check(kast::parseShader(valid, source, error), "parse again");
    const std::string generated = kast::generateShader(templateText, source, kast::ShaderStage::Fragment);
    check(contains(generated, "#define KAST_FRAGMENT\n") && contains(generated, "uniform vec4 u_kastParams[16];\n") &&
              contains(generated, "#define speed (u_kastParams[0].x)\n") &&
              contains(generated, "#define scroll (u_kastParams[2].xy)\n") &&
              contains(generated, "#define data (u_kastParams[4])\n") &&
              contains(generated, "SAMPLER2D(s_kastTexture0, 10);\n#define noise s_kastTexture0\n") &&
              contains(generated, "SAMPLER2D(s_kastTexture2, 12);\n") && contains(generated, "#undef speed\n") &&
              contains(generated, "#pragma kast_line 12\nvoid fragment(inout KastFragment kast_f)\n") &&
              contains(generated, "#pragma kast_line 14\n    ALBEDO *= tint * speed;\n") && !contains(generated, "\n//@user"),
          "generated fs_mesh: params as uniforms / samplers, markers before each .kshader line, the template signature");
    check(kast::generateShader("void main() {}\n", source, kast::ShaderStage::Fragment).empty(), "a template without //@user generates nothing");
    check(kast::parseShader("#define LONG(x) \\\n    (x)\nvoid fragment() {}\n", source, error) &&
              contains(kast::generateShader(templateText, source, kast::ShaderStage::Fragment), "#pragma kast_line 1\n#define LONG(x) \\\n    (x)\n#pragma kast_line 3\n"),
          "no marker inside a macro continued with a backslash");

    // shaderc's listing to .kshader lines: the marker nearest above the error's line.
    const std::string listing = "Code:\n---\n    370: #pragma   kast_line 4\n    371: c = 1.0;\n    372: #pragma   kast_line 5\n"
                                "\n>>> 373: c.x = undefinedThing;\n>>>   7:       ^\n\n    374: #pragma   kast_line 6\n---\n"
                                "Error: D3DCompile failed 0x80004005 (373,7-20): error X3004: undeclared identifier "
                                "'undefinedThing'\n(374,1): warning X3568: whatever\n(900,2): error X3000: syntax error\n"
                                "\nFailed to build shader.\n";
    check(kast::mapShaderErrors(listing) == "line 5: error X3004: undeclared identifier 'undefinedThing'\n"
                                            "generated line 900: error X3000: syntax error",
          std::format("shaderc errors map to .kshader lines: {}", kast::mapShaderErrors(listing)).c_str());
    check(kast::mapShaderErrors("cpp: \"(null)\", line 0: Error: Inside #ifdef block at end of input, depth = 1\n"
                                "Failed to build shader.\n") ==
              "cpp: \"(null)\", line 0: Error: Inside #ifdef block at end of input, depth = 1",
          "preprocessor errors pass through");

    // Real compiles with the shaderc next to this program.
    std::vector<uint8_t> bytecode;
    check(kast::parseShader(valid, source, error) && compileShader(source, kast::ShaderStage::Fragment, bytecode, error) &&
              !bytecode.empty(),
          std::format("a valid .kshader compiles with shaderc ({})", error).c_str());
    const std::string broken = "// line 1\n"
                               "param float strength = 1.0;\n"
                               "\n"
                               "/* a comment\n"
                               "   over lines */\n"
                               "void fragment()\n"
                               "{\n"
                               "    ALBEDO = vec3(strength,\n"
                               "                  0.5,\n"
                               "                  nothingHere);\n"
                               "}\n";
    check(kast::parseShader(broken, source, error) && !compileShader(source, kast::ShaderStage::Fragment, bytecode, error) &&
              contains(error, "line 10: error X3004: undeclared identifier 'nothingHere'"),
          std::format("a GLSL error is reported at its .kshader line: {}", error).c_str());
    check(kast::parseShader("void fragment()\n{\n    ALBEDO = 1.0\n    ALPHA = 0.5;\n}\n", source, error) &&
              !compileShader(source, kast::ShaderStage::Fragment, bytecode, error) && contains(error, "line 4: error X3000"),
          std::format("a missing ';' is reported where the compiler sees it (the next line): {}", error).c_str());

    // vertex(): each stage gets its own function with the template's signature and the other one blanked (lines kept);
    // the vertex templates define KAST_VERTEX only for a .kshader with a vertex(), the shadow stage KAST_SHADOW.
    const std::string withVertex = "param float lift = 1.0;\n"
                                   "param texture2D noise = black;\n"
                                   "float wave(float x) { return sin(x + TIME) * MATERIAL_ROUGHNESS; } // every stage\n"
                                   "void vertex()\n"
                                   "{\n"
                                   "    VERTEX.y += lift * wave(VERTEX.x) * texture2DLod(noise, UV, 0.0).r; // { a brace\n"
                                   "    NORMAL = normalize(NORMAL);\n"
                                   "}\n"
                                   "void fragment()\n"
                                   "{\n"
                                   "    if (UV.x > 0.5) { ALBEDO = vec3(1.0, 0.0, 0.0); }\n"
                                   "}\n";
    check(kast::parseShader(withVertex, source, error) && source.hasVertex && !source.customNormal,
          "vertex() is found; NORMAL inside it doesn't make the fragment stage apply the normal map");
    check(kast::shaderStages(source).size() == kast::kShaderStageCount, "a .kshader with vertex() compiles every stage");
    const std::string vertexTemplate = readText(shaderDir / "vs_mesh.sc");
    const std::string vs = kast::generateShader(vertexTemplate, source, kast::ShaderStage::Vertex);
    const std::string fs = kast::generateShader(templateText, source, kast::ShaderStage::Fragment);
    const std::string shadowFs = kast::generateShader(templateText, source, kast::ShaderStage::Shadow);
    check(contains(vs, "#define KAST_VERTEX\n") && !contains(vs, "KAST_FRAGMENT") &&
              contains(vs, "#pragma kast_line 4\nvoid vertex(inout KastVertex kast_v)\n") &&
              contains(vs, "#pragma kast_line 11\n" + std::string(53, ' ') + "\n") && !contains(vs, "ALBEDO = vec3"),
          "generated vs_mesh: vertex() with the template signature, fragment() blanked line for line");
    check(contains(fs, "#define KAST_FRAGMENT\n") && !contains(fs, "#define KAST_VERTEX") &&
              !contains(fs, "#define KAST_SHADOW") && !contains(fs, "VERTEX.y") &&
              contains(fs, "#pragma kast_line 9\nvoid fragment(inout KastFragment kast_f)\n"),
          "generated fs_mesh: fragment() with the template signature, vertex() blanked");
    check(contains(shadowFs, "#define KAST_FRAGMENT\n#define KAST_SHADOW\n"), "the shadow stage is fs_mesh with KAST_SHADOW");
    check(kast::parseShader("void fragment() {}\n", source, error) && !source.hasVertex &&
              kast::shaderStages(source) ==
                  std::vector<kast::ShaderStage>{kast::ShaderStage::Fragment, kast::ShaderStage::Shadow} &&
              !contains(kast::generateShader(vertexTemplate, source, kast::ShaderStage::Vertex), "#define KAST_VERTEX"),
          "without vertex(): fragment and shadow stages only");
    for (const kast::ShaderStage stage : {kast::ShaderStage::Fragment, kast::ShaderStage::Shadow, kast::ShaderStage::Vertex,
                                          kast::ShaderStage::SkinnedVertex, kast::ShaderStage::InstancedVertex})
        check(kast::parseShader(withVertex, source, error) && compileShader(source, stage, bytecode, error) &&
                  !bytecode.empty(),
              std::format("a .kshader with vertex() compiles for {} ({})", kast::shaderTemplate(stage), error).c_str());
    const std::string vertexBroken = "param float lift = 1.0;\n"
                                     "void vertex()\n"
                                     "{\n"
                                     "    VERTEX.y += lift * INSTANCE_ID;\n"
                                     "    VERTEX.x += ALBEDO.x;\n"
                                     "}\n"
                                     "void fragment() {}\n";
    check(kast::parseShader(vertexBroken, source, error) &&
              compileShader(source, kast::ShaderStage::Fragment, bytecode, error) &&
              !compileShader(source, kast::ShaderStage::InstancedVertex, bytecode, error) &&
              contains(error, "line 5: error X3004: undeclared identifier 'ALBEDO'"),
          std::format("an error in vertex() is reported at its .kshader line (and fragment() alone still compiles): {}",
                      error)
              .c_str());

    // light(), SCREEN_TEXTURE, DEPTH_TEXTURE (phase 5): light() takes the template's signature in the fragment stages
    // (KAST_LIGHT) wherever it sits, the vertex stages blank it; NORMAL inside it doesn't force the normal map.
    const std::string withLight = "param float bands = 3.0;\n"
                                  "void light()\n"
                                  "{\n"
                                  "    DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * ceil(saturate(dot(NORMAL, "
                                  "LIGHT_DIRECTION)) * bands) / bands;\n"
                                  "    SPECULAR_LIGHT += LIGHT_COLOR * step(0.9, dot(reflect(-LIGHT_DIRECTION, NORMAL), VIEW));\n"
                                  "}\n"
                                  "void vertex() { VERTEX.y += bands; }\n"
                                  "void fragment()\n"
                                  "{\n"
                                  "    vec3 behind = texture2D(SCREEN_TEXTURE, SCREEN_UV).rgb;\n"
                                  "    ALBEDO = mix(behind, ALBEDO, saturate(linearDepth(texture2D(DEPTH_TEXTURE, "
                                  "SCREEN_UV).r) + VIEW_POSITION.z));\n"
                                  "}\n";
    check(kast::parseShader(withLight, source, error) && source.hasLight && source.hasVertex && !source.customNormal &&
              source.screenTexture && source.depthTexture,
          "light(), SCREEN_TEXTURE and DEPTH_TEXTURE are found; NORMAL inside light() keeps the built-in normal map rule");
    const std::string lightFs = kast::generateShader(templateText, source, kast::ShaderStage::Fragment);
    const std::string lightVs = kast::generateShader(vertexTemplate, source, kast::ShaderStage::Vertex);
    check(contains(lightFs, "#define KAST_FRAGMENT\n#define KAST_LIGHT\n") &&
              contains(lightFs, "#pragma kast_line 2\nvoid light(inout KastLight kast_f)\n") &&
              contains(lightFs, "#pragma kast_line 8\nvoid fragment(inout KastFragment kast_f)\n") &&
              !contains(lightFs, "VERTEX.y"),
          "generated fs_mesh: light() and fragment() with the template signatures (light() first), vertex() blanked");
    check(!contains(lightVs, "KAST_LIGHT") && !contains(lightVs, "LIGHT_COLOR") && !contains(lightVs, "SCREEN_TEXTURE") &&
              contains(lightVs, "#pragma kast_line 7\nvoid vertex(inout KastVertex kast_v)"),
          "generated vs_mesh: light() and fragment() blanked");
    check(kast::parseShader("void fragment() {}\n", source, error) && !source.hasLight && !source.screenTexture &&
              !source.depthTexture && !contains(kast::generateShader(templateText, source, kast::ShaderStage::Fragment),
                                                "#define KAST_LIGHT"),
          "without light(): no KAST_LIGHT, the built-in Cook-Torrance");
    check(kast::parseShader(withLight, source, error), "parse the light() shader again");
    for (const kast::ShaderStage stage : {kast::ShaderStage::Fragment, kast::ShaderStage::Shadow, kast::ShaderStage::Vertex,
                                          kast::ShaderStage::SkinnedVertex, kast::ShaderStage::InstancedVertex})
        check(compileShader(source, stage, bytecode, error) && !bytecode.empty(),
              std::format("a .kshader with light(), vertex(), SCREEN_TEXTURE and DEPTH_TEXTURE compiles for {} ({})",
                          kast::shaderTemplate(stage), error)
                  .c_str());
    check(kast::parseShader("void fragment() {}\nvoid light()\n{\n    DIFFUSE_LIGHT += UV.x;\n}\n", source, error) &&
              !compileShader(source, kast::ShaderStage::Fragment, bytecode, error) && contains(error, "line 4: error"),
          std::format("an error in light() (UV is fragment()'s) is reported at its .kshader line: {}", error).c_str());
    // SCREEN_TEXTURE / DEPTH_TEXTURE copy the opaque scene: only blend and additive materials may sample them.
    kast::ShaderSource screenOnly, depthOnly, neither;
    check(kast::parseShader("void fragment() { ALBEDO = texture2D(SCREEN_TEXTURE, SCREEN_UV).rgb; }\n", screenOnly, error) &&
              kast::parseShader("void fragment() { ALPHA = texture2D(DEPTH_TEXTURE, SCREEN_UV).r; }\n", depthOnly, error) &&
              kast::parseShader("void fragment() { ALBEDO = vec3_splat(SCREEN_UV.x); }\n", neither, error) &&
              screenOnly.screenTexture && !screenOnly.depthTexture && depthOnly.depthTexture && !depthOnly.screenTexture &&
              !neither.screenTexture && !neither.depthTexture,
          "SCREEN_TEXTURE and DEPTH_TEXTURE are found separately; SCREEN_UV alone is neither");
    kast::Material modes;
    std::string why;
    modes.alphaMode = kast::AlphaMode::Opaque;
    check(!kast::checkShaderAlphaMode(modes, screenOnly, why) &&
              why == "SCREEN_TEXTURE is only for blend and additive materials (drawn after the opaque scene it copies); "
                     "this material's alphaMode is opaque",
          std::format("an opaque material sampling SCREEN_TEXTURE is rejected: {}", why).c_str());
    modes.alphaMode = kast::AlphaMode::Mask;
    check(!kast::checkShaderAlphaMode(modes, depthOnly, why) &&
              contains(why, "DEPTH_TEXTURE is only for blend and additive materials") && contains(why, "alphaMode is mask"),
          std::format("a mask material sampling DEPTH_TEXTURE is rejected: {}", why).c_str());
    bool allowed = kast::checkShaderAlphaMode(modes, neither, why);
    for (const kast::AlphaMode mode : {kast::AlphaMode::Blend, kast::AlphaMode::Additive}) {
        modes.alphaMode = mode;
        allowed = allowed && kast::checkShaderAlphaMode(modes, screenOnly, why) &&
                  kast::checkShaderAlphaMode(modes, depthOnly, why);
    }
    check(allowed, "blend and additive materials may sample them; any material may use a shader that doesn't");
    checkSceneCopies();

    // The library: compiles once per change, null (logged once, with the line) when broken or missing.
    const std::filesystem::path dir = testRoot() / "kast_shader_check";
    removeTree(dir);
    std::filesystem::create_directories(dir / "shaders");
    writeFile(dir / "shaders/good.kshader", "param color tint;\nvoid fragment() { ALBEDO *= tint.rgb; }\n");
    writeFile(dir / "shaders/bad.kshader", "void fragment()\n{\n    ALBEDO = missingName;\n}\n");
    kast::ShaderLibrary library;
    library.setRoot(dir);
    check(!library.get("shaders/good.kshader"), "no compiler and no game.pak bytecode: no shader");
    check(countLogged("Shader 'shaders/good.kshader': no compiled bytecode for it (the editor's Build compiles it into "
                      "game.pak") == 1,
          "missing precompiled bytecode is logged");
    library.setRoot(testRoot()); // forget
    library.setRoot(dir);
    library.setCompiler(compileShader);
    const auto good = library.get("shaders/good.kshader");
    check(good && !good->stage(kast::ShaderStage::Fragment).empty() && !good->stage(kast::ShaderStage::Shadow).empty() &&
              good->stage(kast::ShaderStage::Vertex).empty() && good->source.params.size() == 1 &&
              library.get("shaders/good.kshader") == good,
          "a .kshader compiles once (fragment and shadow stages; no vertex() takes the built-in vertex shaders) and is "
          "shared");
    check(!library.get("shaders/bad.kshader") && !library.get("shaders/none.kshader"), "broken and missing: null");
    for (int i = 0; i < 3; ++i)
        library.reloadChanged();
    check(countLogged("[error] Shader 'shaders/bad.kshader': line 3: error X3004: undeclared identifier 'missingName'; "
                      "rendering it magenta") == 1,
          "a broken .kshader logs one error naming the file and its line");
    check(countLogged("Shader 'shaders/none.kshader': cannot read the file; rendering it magenta") == 1,
          "a missing .kshader logs one error");
    rewrite(dir / "shaders/good.kshader", "void fragment() { ALBEDO = vec3(1.0, 0.0, 0.0); }\n");
    library.reloadChanged();
    const auto changed = library.get("shaders/good.kshader");
    check(changed && changed != good && changed->source.params.empty(), "a changed .kshader recompiles");
    rewrite(dir / "shaders/bad.kshader", "void fragment() {}\n");
    library.reloadChanged();
    check(library.get("shaders/bad.kshader") != nullptr, "a fixed .kshader compiles");
    removeTree(dir);
    checkShaderLibraryAsync();
    checkShaderBuild();
    checkShaderGraphs();
    checkShaderGraphNodes();
    checkShaderGraphEdits();

    // COLOR0: glTF COLOR_0 imports (RGBA8, R in the low byte); models without it and primitives are white.
    const auto model = importModel(KAST_TEST_PROJECT_DIR, "models/vertex_colors.gltf", error);
    std::vector<uint32_t> colors;
    for (const kast::ModelMesh& mesh : model ? model->meshes : std::vector<kast::ModelMesh>{})
        for (const kast::ModelVertex& v : mesh.vertices)
            colors.push_back(v.color);
    std::ranges::sort(colors);
    std::string read;
    for (uint32_t c : colors)
        read += std::format(" {:08x}", c);
    check(colors == std::vector<uint32_t>{0x80ffffff, 0xff0000ff, 0xff00ff00, 0xffff0000},
          std::format("vertex_colors.gltf's COLOR_0 reads back as red, green, blue, half-transparent white:{}", read).c_str());
    const auto plain = importModel(KAST_TEST_PROJECT_DIR, "models/cube.gltf", error);
    bool white = plain && !plain->meshes.empty();
    for (const kast::ModelMesh& mesh : plain ? plain->meshes : std::vector<kast::ModelMesh>{})
        for (const kast::ModelVertex& v : mesh.vertices)
            white = white && v.color == 0xffffffff;
    for (const kast::Primitive primitive : {kast::Primitive::Cube, kast::Primitive::Plane, kast::Primitive::Sphere})
        for (const kast::MeshVertex& v : kast::primitiveMesh(primitive).vertices)
            white = white && v.color == 0xffffffff;
    check(white, "models without vertex colors and the primitives are white");
}

// SCREEN_TEXTURE / DEPTH_TEXTURE: Renderer::render copies the opaque scene's color / depth only on frames a blended
// .kshader in view samples them (headless bgfx, real shaderc), and draws an opaque material that does magenta.
void checkSceneCopies()
{
    const std::filesystem::path dir = looseTestDir("kast_scene_copy_check");
    std::filesystem::create_directories(dir / "shaders");
    std::filesystem::create_directories(dir / "materials");
    writeFile(dir / "shaders/screen.kshader", "void fragment() { ALBEDO = texture2D(SCREEN_TEXTURE, SCREEN_UV).rgb; }\n");
    writeFile(dir / "shaders/depth.kshader",
              "void fragment() { ALPHA = saturate(linearDepth(texture2D(DEPTH_TEXTURE, SCREEN_UV).r)); }\n");
    writeFile(dir / "shaders/both.kshader", "void fragment() { ALBEDO = texture2D(SCREEN_TEXTURE, SCREEN_UV).rgb * "
                                            "texture2D(DEPTH_TEXTURE, SCREEN_UV).r; }\n");
    for (const char* name : {"screen", "depth", "both"})
        writeFile(dir / std::format("materials/{}.mat", name),
                  std::format(R"({{"version": 3, "alphaMode": "blend", "shader": "shaders/{}.kshader"}})", name));
    writeFile(dir / "materials/screen_opaque.mat", R"({"version": 3, "shader": "shaders/screen.kshader"})");
    writeFile(dir / "materials/glass.mat", R"({"version": 3, "alphaMode": "blend"})");
    kast::shaders().setRoot(dir);
    kast::shaders().setCompiler(compileShader);
    kast::shaders().setAsync(false);

    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    if (!bgfx::init(init)) {
        check(false, "scene copies: headless bgfx starts");
        return;
    }
    {
        kast::Renderer renderer;
        renderer.init();
        constexpr uint16_t kSize = 64;
        const bgfx::TextureHandle attachments[] = {
            bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT),
            bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT)};
        const bgfx::FrameBufferHandle target = bgfx::createFrameBuffer(2, attachments, true);
        kast::Scene scene;
        const entt::entity plane = scene.create("Water");
        auto& mesh = scene.registry().emplace<kast::MeshRenderer>(plane);
        mesh.primitive = kast::Primitive::Plane;
        float view[16], proj[16];
        bx::mtxLookAt(view, {0.0f, 3.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, bx::Handedness::Right);
        bx::mtxProj(proj, 60.0f, 1.0f, 0.1f, 100.0f, false, bx::Handedness::Right);
        // Copies made by one frame drawing plane with material at position, with or without a target.
        const auto frame = [&](const char* material, float x, bool withTarget) {
            mesh.material = std::format("materials/{}.mat", material);
            scene.registry().get<kast::Transform>(plane).position = {x, 0.0f, 0.0f};
            const kast::Renderer::SceneCopies before = renderer.sceneCopies();
            if (withTarget)
                renderer.setTarget(target, kSize, kSize);
            renderer.render(scene, dir, kast::Renderer::kShadowViews, view, proj, 0);
            bgfx::frame();
            const kast::Renderer::SceneCopies& after = renderer.sceneCopies();
            return std::pair{after.screen - before.screen, after.depth - before.depth};
        };
        using Copies = std::pair<uint64_t, uint64_t>;
        check(frame("glass", 0.0f, true) == Copies{0, 0}, "scene copies: none for a blended material without a .kshader");
        check(frame("screen", 0.0f, true) == Copies{1, 0} && frame("screen", 0.0f, true) == Copies{1, 0},
              "scene copies: SCREEN_TEXTURE in view copies the color once per frame, not the depth");
        check(frame("depth", 0.0f, true) == Copies{0, 1}, "scene copies: DEPTH_TEXTURE copies only the depth");
        check(frame("both", 0.0f, true) == Copies{1, 1}, "scene copies: both");
        check(frame("both", 500.0f, true) == Copies{0, 0}, "scene copies: none while the material is out of view");
        check(frame("both", 0.0f, false) == Copies{0, 0}, "scene copies: none without a target (black / far plane instead)");
        check(frame("both", 0.0f, true) == Copies{1, 1}, "scene copies: the target is set for one render() at a time");
        const auto warned = countLogged("SCREEN_TEXTURE is only for blend and additive materials");
        check(frame("screen_opaque", 0.0f, true) == Copies{0, 0} &&
                  countLogged("Shader 'shaders/screen.kshader': SCREEN_TEXTURE is only for blend and additive materials "
                              "(drawn after the opaque scene it copies); this material's alphaMode is opaque; rendering "
                              "the material magenta") == 1 &&
                  countLogged("SCREEN_TEXTURE is only for blend and additive materials") == warned + 1,
              "scene copies: an opaque material sampling SCREEN_TEXTURE draws magenta (one warning), copying nothing");
        kast::log::info(std::format("scene copies over the check: {} color, {} depth", renderer.sceneCopies().screen,
                                    renderer.sceneCopies().depth));
        bgfx::destroy(target);
        renderer.shutdown();
    }
    bgfx::shutdown();
    kast::shaders().setRoot(testRoot()); // forget
    removeTree(dir);
}

// Build (phase 4): every shipped .kshader compiles into game.pak at shaderPakPath; the player's library (no compiler)
// loads exactly that bytecode; a broken .kshader fails the Build naming the file and line, writing nothing.
void checkShaderBuild()
{
    const std::filesystem::path dir = looseTestDir("kast_shader_build_check");
    const std::filesystem::path root = dir / "game";
    const std::string sway = "param float amount = 0.5;\nvoid vertex() { VERTEX.x += amount; }\n"
                             "void fragment() { ALBEDO = vec3_splat(amount); }\n";
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::create_directories(root / ".wip");
    writeFile(root / "project.kast", "x");
    writeFile(root / "shaders/sway.kshader", sway);
    writeFile(root / "shaders/plain.kshader", "void fragment() {}\n");
    writeFile(root / ".wip/broken.kshader", "void fragment() { ALBEDO = nope; }\n"); // not shipped: not compiled
    saveTestScene(root / "a.scene", "Thing", {});
    kast::Project project{.root = root, .name = "Shaders", .startScene = "a.scene"};
    const std::filesystem::path playerDir = std::filesystem::path(SDL_GetBasePath());
    std::filesystem::path exe;
    std::string error;
    const bool built = buildGame(project, dir / "out", playerDir, exe, error);
    check(built, std::format("Build compiles shaders: {}", error).c_str());
    kast::Pak pak;
    check(pak.open(dir / "out" / kGamePakName, error), "the shader build's game.pak opens");
    kast::ShaderSource source;
    check(kast::parseShader(sway, source, error), "sway parses");
    bool same = true;
    size_t stages = 0;
    for (const kast::ShaderStage stage : kast::shaderStages(source)) {
        std::vector<uint8_t> bytecode;
        const std::optional<std::string_view> packed = pak.find(kast::shaderPakPath(sway, stage));
        same = same && packed && compileShader(source, stage, bytecode, error) &&
               *packed == std::string_view(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
        ++stages;
    }
    check(stages == kast::kShaderStageCount && same, "game.pak holds every stage's bytecode of a shader with vertex()");
    check(pak.list(".kshader_bytecode").size() == kast::kShaderStageCount + 2,
          "game.pak holds bytecode only for shipped shaders (sway: 5 stages, plain: 2)");
    check(pak.find("shaders/sway.kshader").has_value() && !pak.find(".wip/broken.kshader"),
          "the .kshader sources ship too (their params), dot-folders don't");

    // The player's path: no compiler, bytecode read from the pak's entries (here unpacked to disk).
    const std::filesystem::path unpacked = dir / "unpacked";
    for (const std::string& path : pak.list("")) {
        std::filesystem::create_directories((unpacked / path).parent_path());
        writeFile(unpacked / path, std::string(*pak.find(path)));
    }
    kast::ShaderLibrary player;
    player.setRoot(unpacked);
    const auto loaded = player.get("shaders/sway.kshader");
    same = loaded != nullptr;
    for (const kast::ShaderStage stage : kast::shaderStages(source))
        same = same && loaded->stage(stage) == std::vector<uint8_t>(pak.find(kast::shaderPakPath(sway, stage))->begin(),
                                                                    pak.find(kast::shaderPakPath(sway, stage))->end());
    check(same && loaded->source.params.size() == 1 && countLogged("Loaded shader 'shaders/sway.kshader' (precompiled)") == 1,
          "without a compiler the library loads game.pak's bytecode");
    rewrite(unpacked / "shaders/plain.kshader", "void fragment() { ALBEDO = vec3(0.0); }\n");
    check(!player.get("shaders/plain.kshader"), "a .kshader whose text differs from the built one has no bytecode");

    for (const auto& [file, text, line] :
         {std::tuple{"shaders/broken.kshader", "void fragment()\n{\n    ALBEDO = missingName;\n}\n",
                     "line 3: error X3004: undeclared identifier 'missingName'"},
          std::tuple{"shaders/broken.kshader", "param float a = 1.0;\nparam vec5 b;\nvoid fragment() {}\n",
                     "line 2: unknown param type 'vec5'"}}) {
        writeFile(root / file, text);
        const bool failed = !buildGame(project, dir / "bad", playerDir, exe, error);
        check(failed && contains(error, std::format("Build failed: shader '{}': {}", file, line)) &&
                  !std::filesystem::exists(dir / "bad"),
              std::format("a broken .kshader fails the Build with its file and line, writing nothing: {}", error).c_str());
    }
    std::filesystem::remove(root / "shaders/broken.kshader");
    // A .kshadergraph ships too, and Build compiles it keyed by its generated text, which the player (no compiler)
    // generates again from the shipped graph. A broken graph fails the Build naming the file and node.
    std::string graphText, generated;
    check(kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "shaders/graph_demo.kshadergraph", graphText),
          "the demo graph reads");
    writeFile(root / "shaders/demo.kshadergraph", graphText);
    generated = graphText;
    kast::Pak graphPak;
    check(kast::shaderFileText("shaders/demo.kshadergraph", generated, error) &&
              buildGame(project, dir / "graph", playerDir, exe, error) && graphPak.open(dir / "graph" / kGamePakName, error) &&
              graphPak.find(kast::shaderPakPath(generated, kast::ShaderStage::Fragment)) &&
              graphPak.find(kast::shaderPakPath(generated, kast::ShaderStage::Shadow)) && graphPak.find("shaders/demo.kshadergraph"),
          std::format("Build compiles a .kshadergraph into game.pak by its generated text ({})", error).c_str());
    const std::filesystem::path graphUnpacked = dir / "graph_unpacked";
    for (const std::string& path : graphPak.list("")) {
        std::filesystem::create_directories((graphUnpacked / path).parent_path());
        writeFile(graphUnpacked / path, std::string(*graphPak.find(path)));
    }
    kast::ShaderLibrary graphPlayer;
    graphPlayer.setRoot(graphUnpacked);
    const auto graphShader = graphPlayer.get("shaders/demo.kshadergraph");
    check(graphShader && graphShader->source.params.size() == 3 &&
              countLogged("Loaded shader 'shaders/demo.kshadergraph' (precompiled)") == 1,
          "without a compiler the library loads a graph's game.pak bytecode");
    writeFile(root / "shaders/broken.kshadergraph",
              R"({"version": 1, "nodes": [{"id": 1, "type": "output_light"}, {"id": 5, "type": "uv"}],
                  "links": [{"from": [5, "out"], "to": [1, "DIFFUSE_LIGHT"]}]})");
    check(!buildGame(project, dir / "bad", playerDir, exe, error) &&
              contains(error, "Build failed: shader 'shaders/broken.kshadergraph': node 5: UV is not available in light()") &&
              !std::filesystem::exists(dir / "bad"),
          std::format("a broken .kshadergraph fails the Build with its file and node: {}", error).c_str());
    std::filesystem::remove(root / "shaders/broken.kshadergraph");
    std::filesystem::create_directories(root / "materials");
    writeFile(root / "shaders/screen.kshader", "void fragment() { ALBEDO = texture2D(SCREEN_TEXTURE, SCREEN_UV).rgb; }\n");
    writeFile(root / "materials/water.mat", R"({"version": 3, "alphaMode": "mask", "shader": "shaders/screen.kshader"})");
    check(!buildGame(project, dir / "bad", playerDir, exe, error) &&
              contains(error, "Build failed: material 'materials/water.mat': SCREEN_TEXTURE is only for blend and additive "
                              "materials") &&
              !std::filesystem::exists(dir / "bad"),
          std::format("a mask material sampling SCREEN_TEXTURE fails the Build: {}", error).c_str());
    writeFile(root / "materials/water.mat", R"({"version": 3, "alphaMode": "blend", "shader": "shaders/screen.kshader"})");
    check(buildGame(project, dir / "good", playerDir, exe, error),
          std::format("as a blend material it builds: {}", error).c_str());
    removeTree(dir);
}

// Shader graphs (.kshadergraph): strict JSON with a byte-identical round trip, canLink's type rules, deterministic
// codegen, node ids on validation and shaderc errors, the library and material properties.
void checkShaderGraphs()
{
    std::string error;
    // shader_graphs/: the phase 5 samples, the hand-written shaders/*.kshader samples rebuilt without Custom Expression
    // nodes; their screenshots must match custom_shaders.scene's (cmake/shader_graph_tests.cmake).
    for (const char* name : {"shaders/graph_demo.kshadergraph", "shaders/empty.kshadergraph", "shader_graphs/pulse.kshadergraph",
                             "shader_graphs/wind.kshadergraph", "shader_graphs/dissolve.kshadergraph",
                             "shader_graphs/water.kshadergraph", "shader_graphs/toon.kshadergraph"}) {
        std::string text;
        kast::ShaderGraph graph;
        const bool read = kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / name, text);
        std::erase(text, '\r'); // a Windows git checkout may have made the line ends CRLF; Kast writes '\n'
        kast::ShaderGraph reloaded;
        check(read && kast::parseShaderGraph(text, graph, error) && kast::shaderGraphText(graph) == text &&
                  kast::parseShaderGraph(kast::shaderGraphText(graph), reloaded, error) &&
                  kast::shaderGraphText(reloaded) == text,
              std::format("{} round-trips byte for byte ({})", name, error).c_str());
        std::vector<kast::GraphIssue> issues;
        check(!kast::generateKShader(graph, issues).empty() && issues.empty() &&
                  std::ranges::none_of(graph.nodes, [](const kast::GraphNode& n) { return n.type == kast::kCustomExpression; }),
              std::format("{} generates with no error or warning and no Custom Expression", name).c_str());
    }
    const auto parse = [&](const std::string& json, kast::ShaderGraph& graph) {
        error.clear();
        return kast::parseShaderGraph(json, graph, error);
    };
    kast::ShaderGraph graph;
    check(parse(R"({"version": 1, "nodes": [{"id": 3, "type": "output_fragment"}]})", graph) && graph.params.empty() &&
              graph.nodes.size() == 1 && graph.nodes[0].position == std::array{0.0f, 0.0f} && graph.links.empty(),
          std::format("missing optional fields get their defaults ({})", error).c_str());
    for (const auto& [json, expected] : std::initializer_list<std::pair<const char*, const char*>>{
             {R"({"version": 1, "colour": 1})", "colour: unknown key"},
             {R"({"version": 2})", "version: must be 1"},
             {R"({"nodes": []})", "version: must be 1"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment", "size": 2}]})", "nodes[0].size: unknown key"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "blur"}]})", "nodes[0].type: unknown node type \"blur\""},
             {R"({"version": 1, "nodes": [{"id": 0, "type": "time"}]})", "nodes[0].id: must be a whole number > 0"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "time"}, {"id": 1, "type": "uv"}]})",
              "nodes[1].id: 1 is already another node's id"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "add", "inputs": {"a": [1, 2, 3, 4, 5]}}]})",
              "nodes[0].inputs.a: must be a finite number or an array of 2 to 4 finite numbers"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "add", "inputs": {"c": 1}}]})",
              "nodes[0].inputs.c: Add has no input 'c'"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment", "inputs": {"ALPHA": 1}}]})",
              "nodes[0].inputs.ALPHA: this input takes a link, not a constant"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "param", "settings": {"param": 2}}]})",
              "nodes[0].settings.param: must be a string"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "constant_vec3", "settings": {"value": [1, 2]}}]})",
              "nodes[0].settings.value: must be an array of 3 finite numbers"},
             {R"({"version": 1, "params": [{"name": "a", "type": "vec5"}]})", "params[0].type: must be"},
             {R"({"version": 1, "params": [{"name": "a", "type": "vec2", "range": [0, 1]}]})",
              "params[0].range: is only for float params"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "time"}], "links": [{"from": [9, "out"], "to": [1, "a"]}]})",
              "links[0].from: no node 9"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "time"}, {"id": 2, "type": "output_fragment"}],
                 "links": [{"from": [1, "out"], "to": [2, "COLOUR"]}]})",
              "links[0].to: Fragment Output (node 2) has no input 'COLOUR'"},
             {R"({"version": 1, "nodes": [{"id": 1, "type": "time"}, {"id": 2, "type": "add"}],
                 "links": [{"from": [1, "out"], "to": [2, "a"]}, {"from": [1, "out"], "to": [2, "a"]}]})",
              "links[1].to: that input already has a link"},
             {R"({"version": 1, "nodes": [{"id": 4, "type": "add"}, {"id": 7, "type": "multiply"}],
                 "links": [{"from": [4, "out"], "to": [7, "a"]}, {"from": [7, "out"], "to": [4, "b"]}]})",
              "node 4: the links form a cycle"},
             {"{", "invalid JSON"}}) {
        check(!parse(json, graph) && error.starts_with(expected),
              std::format("a bad .kshadergraph is rejected: {} (got: {})", expected, error).c_str());
    }

    // canLink: splat, truncate (warning), texture / number refused, cycles refused.
    check(parse(R"({"version": 1, "params": [{"name": "pattern", "type": "texture2D"}],
        "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 2, "type": "time"}, {"id": 3, "type": "constant_vec4"},
                  {"id": 4, "type": "param", "settings": {"param": "pattern"}}, {"id": 5, "type": "add"},
                  {"id": 6, "type": "multiply"}, {"id": 7, "type": "sample_texture"}],
        "links": [{"from": [5, "out"], "to": [6, "a"]}]})", graph),
          std::format("the canLink graph parses ({})", error).c_str());
    const auto link = [&](int from, const char* out, int to, const char* in) { return kast::canLink(graph, {from, out}, {to, in}); };
    kast::LinkCheck result = link(2, "out", 1, "ALBEDO");
    check(result.allowed && !result.warning, "canLink: float -> vec3 is allowed (splatted)");
    result = link(3, "out", 1, "ALPHA");
    check(result.allowed && result.warning && result.message == "vec4 truncated to float",
          std::format("canLink: vec4 -> float is allowed with a warning ({})", result.message).c_str());
    result = link(4, "out", 5, "a");
    check(!result.allowed && result.message == "a texture2D can't go into a number input",
          std::format("canLink: texture -> number is refused ({})", result.message).c_str());
    result = link(2, "out", 7, "texture");
    check(!result.allowed && result.message == "a float can't go into a texture2D input",
          std::format("canLink: number -> texture is refused ({})", result.message).c_str());
    check(link(4, "out", 7, "texture").allowed, "canLink: a texture param into Sample Texture");
    result = link(6, "out", 5, "b");
    check(!result.allowed && result.message == "the link would close a cycle",
          std::format("canLink: a link closing a cycle is refused ({})", result.message).c_str());
    check(!link(5, "out", 5, "a").allowed && !link(2, "nope", 1, "ALBEDO").allowed && !link(2, "out", 99, "a").allowed,
          "canLink: a node into itself and missing pins are refused");
    check(kast::outputPinType(graph, {6, "out"}) == kast::PinType::Float &&
              kast::outputPinType(graph, {4, "out"}) == kast::PinType::Texture2D,
          "output types: a math node with float inputs is a float, a texture param a texture2D");

    // Codegen: the demo graph's exact text, the same for any node order and run.
    std::string demoText;
    check(kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "shaders/graph_demo.kshadergraph", demoText) &&
              parse(demoText, graph),
          "the demo graph parses");
    std::vector<kast::GraphIssue> issues;
    const std::string demo = kast::generateKShader(graph, issues);
    const std::string expected = "param vec3 tint = vec3(1, 0.6, 0.3);\n"
                                 "param float speed = 0.5 range(0, 4);\n"
                                 "param texture2D pattern = white srgb;\n"
                                 "\n"
                                 "void fragment()\n"
                                 "{\n"
                                 "    // @node 2 uv\n"
                                 "    vec2 n2_out = UV;\n"
                                 "    // @node 3 time\n"
                                 "    float n3_out = TIME;\n"
                                 "    // @node 4 param\n"
                                 "    float n4_out = speed;\n"
                                 "    // @node 5 multiply\n"
                                 "    float n5_out = n3_out * n4_out;\n"
                                 "    // @node 6 add\n"
                                 "    vec2 n6_out = n2_out + vec2_splat(n5_out);\n"
                                 "    // @node 7 param\n"
                                 "    // @node 8 sample_texture\n"
                                 "    vec4 n8_rgba = texture2D(pattern, n6_out);\n"
                                 "    vec3 n8_rgb = n8_rgba.rgb;\n"
                                 "    float n8_r = n8_rgba.r;\n"
                                 "    float n8_g = n8_rgba.g;\n"
                                 "    float n8_b = n8_rgba.b;\n"
                                 "    float n8_a = n8_rgba.a;\n"
                                 "    // @node 9 param\n"
                                 "    vec3 n9_out = tint;\n"
                                 "    // @node 10 multiply\n"
                                 "    vec3 n10_out = n8_rgb * n9_out;\n"
                                 "    // @node 11 constant_float\n"
                                 "    float n11_out = 0.3;\n"
                                 "    // @node 1 output_fragment\n"
                                 "    ALBEDO = n10_out;\n"
                                 "    ROUGHNESS = n11_out;\n"
                                 "}\n";
    check(issues.empty() && demo == expected, std::format("the demo graph generates its .kshader:\n{}", demo).c_str());
    std::vector<kast::GraphIssue> again;
    kast::ShaderGraph reordered = graph;
    std::ranges::reverse(reordered.nodes);
    std::ranges::reverse(reordered.links);
    check(kast::generateKShader(graph, again) == demo && kast::generateKShader(reordered, again) == demo && again.empty(),
          "codegen is deterministic: twice, and with the nodes and links in reverse order, the same text");
    kast::ShaderSource source;
    check(parse(R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"}]})", graph) &&
              kast::generateKShader(graph, issues) == "void fragment()\n{\n}\n" && issues.empty(),
          "an unconnected Fragment Output is an empty fragment(): the built-in material's look");

    // Every stage, compiled by shaderc: vertex() and light() only when their output node has a link.
    check(parse(R"({"version": 1, "params": [{"name": "lift", "type": "float", "default": 0.25}],
        "nodes": [{"id": 1, "type": "output_vertex"}, {"id": 2, "type": "output_fragment"}, {"id": 3, "type": "output_light"},
                  {"id": 4, "type": "param", "settings": {"param": "lift"}}, {"id": 5, "type": "time"},
                  {"id": 6, "type": "add", "inputs": {"b": [0, 0.1, 0]}},
                  {"id": 7, "type": "constant_color", "settings": {"value": [1, 0.5, 0.25, 1]}}],
        "links": [{"from": [4, "out"], "to": [6, "a"]}, {"from": [6, "out"], "to": [1, "VERTEX"]},
                  {"from": [5, "out"], "to": [2, "EMISSION"]}, {"from": [7, "out"], "to": [3, "DIFFUSE_LIGHT"]},
                  {"from": [5, "out"], "to": [3, "SPECULAR_LIGHT"]}]})", graph),
          std::format("the three-stage graph parses ({})", error).c_str());
    std::string text = kast::generateKShader(graph, issues);
    check(issues.size() == 1 && issues[0] == kast::GraphIssue{3, "input 'DIFFUSE_LIGHT': vec4 truncated to vec3", true} &&
              contains(text, "void vertex()\n{\n    // @node 4 param\n    float n4_out = lift;\n    // @node 6 add\n"
                             "    vec3 n6_out = vec3_splat(n4_out) + vec3(0.0, 0.1, 0.0);\n    // @node 1 output_vertex\n"
                             "    VERTEX = n6_out;\n}\n") &&
              contains(text, "    DIFFUSE_LIGHT += (n7_out).xyz;\n    SPECULAR_LIGHT += vec3_splat(n5_out);\n") &&
              contains(text, "    // @node 5 time\n    float n5_out = TIME;\n    // @node 2 output_fragment\n    EMISSION = vec3_splat(n5_out);\n"),
          std::format("vertex(), fragment() and light(); a node feeding two stages is in both; a truncation warns on the "
                      "node:\n{}",
                      text)
              .c_str());
    bool compiled = kast::parseShader(text, source, error) && source.hasVertex && source.hasLight;
    for (const kast::ShaderStage stage : kast::shaderStages(source)) {
        std::vector<uint8_t> bytecode;
        compiled = compiled && compileShader(source, stage, bytecode, error) && !bytecode.empty();
    }
    check(compiled && kast::shaderStages(source).size() == kast::kShaderStageCount,
          std::format("a three-stage graph compiles for every stage ({})", error).c_str());
    std::vector<uint8_t> bytecode;
    check(kast::parseShader(demo, source, error) && compileShader(source, kast::ShaderStage::Fragment, bytecode, error),
          std::format("the demo graph compiles ({})", error).c_str());

    // shaderc errors map to the node whose line failed: a bad line injected into node 10's code (line 28).
    std::string bad = demo;
    bad.replace(bad.find("n8_rgb * n9_out"), 15, "n8_rgb * undefinedThing");
    check(kast::parseShader(bad, source, error) && !compileShader(source, kast::ShaderStage::Fragment, bytecode, error),
          "the injected bad line fails to compile");
    const std::string mapped = kast::mapGraphErrors(bad, error);
    check(mapped.starts_with("node 10: line 28: error X3004: undeclared identifier 'undefinedThing'"),
          std::format("a shaderc error names its node: {}", mapped).c_str());
    check(kast::graphNodeAtLine(demo, 1) == 0 && kast::graphNodeAtLine(demo, 5) == 0 && kast::graphNodeAtLine(demo, 8) == 2 &&
              kast::graphNodeAtLine(demo, 28) == 10 && kast::graphNodeAtLine(demo, 33) == 1 &&
              kast::graphNodeAtLine(demo, 34) == 0,
          "lines map to nodes: params and signatures to none, code to the marker above it");
    check(kast::mapGraphErrors(demo, "line 2: x\ncpp: other") == "line 2: x\ncpp: other", "unmapped lines are kept");

    // Validation errors, each on its node.
    const auto issuesOf = [&](const std::string& json) {
        issues.clear();
        text.clear();
        if (parse(json, graph))
            text = kast::generateKShader(graph, issues);
        return issues;
    };
    using Issues = std::vector<kast::GraphIssue>;
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_light"}, {"id": 5, "type": "uv"}],
                      "links": [{"from": [5, "out"], "to": [1, "DIFFUSE_LIGHT"]}]})") ==
                  Issues{{5, "UV is not available in light() (only in vertex(), fragment())"}} &&
              text.empty(),
          "a UV into the Light Output: an error on the UV node, no text");
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_vertex"}, {"id": 2, "type": "sample_texture"}],
                      "links": [{"from": [2, "rgb"], "to": [1, "VERTEX"]}]})") ==
              Issues{{2, "Sample Texture is not available in vertex() (only in fragment())"}, {2, "input 'texture' needs a link"}},
          "Sample Texture in vertex() without a texture: two errors on it");
    check(issuesOf(R"({"version": 1, "params": [{"name": "ALBEDO", "type": "float"}],
                      "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 8, "type": "param", "settings": {"param": "ALBEDO"}}],
                      "links": [{"from": [8, "out"], "to": [1, "ALPHA"]}]})") ==
              Issues{{8, "param 'ALBEDO' has the name of a built-in"}},
          "a param named like a built-in: the error is on its Param node");
    check(issuesOf(R"({"version": 1, "params": [{"name": "a", "type": "texture2D"}, {"name": "b", "type": "texture2D"},
                      {"name": "c", "type": "texture2D"}, {"name": "d", "type": "texture2D"}, {"name": "e", "type": "texture2D"}],
                      "nodes": [{"id": 1, "type": "output_fragment"}]})").size() == 1 &&
              issues[0].node == 0 && issues[0].message.starts_with("too many texture2D params: at most 4"),
          "five texture params: a graph-level error (no node uses the fifth)");
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 2, "type": "param", "settings": {"param": "zz"}},
                                {"id": 3, "type": "output_fragment"}],
                      "links": [{"from": [2, "out"], "to": [1, "AO"]}]})") ==
              Issues{{3, "a second Fragment Output: a graph has at most one"}, {2, "the graph has no param 'zz'"}},
          "a missing param and a second output node are errors on their nodes");

    // The library: a graph compiles like a .kshader; its errors name the file and node (or JSON path); edits reload.
    const std::filesystem::path dir = looseTestDir("kast_shader_graph_check");
    std::filesystem::create_directories(dir / "shaders");
    writeFile(dir / "shaders/demo.kshadergraph", demoText);
    writeFile(dir / "shaders/bad.kshadergraph", R"({"version": 1, "nodes": [{"id": 1, "type": "output_light"}, {"id": 5, "type": "uv"}],
        "links": [{"from": [5, "out"], "to": [1, "DIFFUSE_LIGHT"]}]})");
    writeFile(dir / "shaders/typo.kshadergraph", R"({"version": 1, "nodes": [], "links": [{"from": [1, "out"]}]})");
    kast::ShaderLibrary library;
    library.setRoot(dir);
    library.setCompiler(compileShader);
    const auto shader = library.get("shaders/demo.kshadergraph");
    check(shader && shader->source.params.size() == 3 && !shader->stage(kast::ShaderStage::Fragment).empty() &&
              countLogged("Compiled shader 'shaders/demo.kshadergraph' in ") == 1,
          "the library compiles a .kshadergraph");
    check(!library.get("shaders/bad.kshadergraph") && !library.get("shaders/typo.kshadergraph"), "broken graphs: null");
    check(countLogged("[error] Shader 'shaders/bad.kshadergraph': node 5: UV is not available in light() (only in vertex(), "
                      "fragment()); rendering it magenta") == 1 &&
              countLogged("[error] Shader 'shaders/typo.kshadergraph': links[0].to: must be [node id, \"pin\"]") == 1,
          "a broken graph logs its file and node (or JSON path)");
    std::string edited = demoText;
    edited.replace(edited.find("\"multiply\""), 10, "\"add\"");
    rewrite(dir / "shaders/demo.kshadergraph", edited);
    library.reloadChanged();
    const auto changed = library.get("shaders/demo.kshadergraph");
    check(changed && changed != shader && countLogged("Recompiled shader 'shaders/demo.kshadergraph'") == 1,
          "an edited graph recompiles");

    // Live update (the Shader Graph's unsaved edits): an override is read instead of the file until cleared; text() is
    // exactly what compiles; a broken override keeps the last good shader; clearing goes back to the file.
    const auto generated = [](const std::string& graphText) {
        kast::ShaderGraph graph;
        std::string why;
        std::vector<kast::GraphIssue> issues;
        return kast::parseShaderGraph(graphText, graph, why) ? kast::generateKShader(graph, issues) : std::string();
    };
    const std::string badGraph = R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 5, "type": "uv"},
        {"id": 6, "type": "light_direction"}], "links": [{"from": [6, "out"], "to": [1, "EMISSION"]}]})";
    library.setOverride("shaders/demo.kshadergraph", demoText);
    const auto live = library.get("shaders/demo.kshadergraph");
    std::string onDisk;
    check(live && live != changed && library.text("shaders/demo.kshadergraph") == generated(demoText) &&
              !generated(demoText).empty() && kast::readFile(dir / "shaders/demo.kshadergraph", onDisk) && onDisk == edited,
          "an override compiles instead of the file (text() is its generated code); the file is untouched");
    library.reloadChanged();
    check(library.get("shaders/demo.kshadergraph") == live, "while overridden, the unchanged file doesn't reload it");
    library.setOverride("shaders/demo.kshadergraph", badGraph);
    const kast::ShaderLibrary::Status broken = library.status("shaders/demo.kshadergraph");
    check(library.get("shaders/demo.kshadergraph") == live && broken.state == kast::ShaderLibrary::State::Failed &&
              broken.hasShader && contains(broken.error, "node 6: ") && library.text("shaders/demo.kshadergraph").empty(),
          std::format("a broken override fails on its node and keeps the last good shader ({})", broken.error).c_str());
    library.setOverride("shaders/demo.kshadergraph", std::nullopt);
    check(library.get("shaders/demo.kshadergraph") != live &&
              library.text("shaders/demo.kshadergraph") == generated(edited) &&
              library.status("shaders/demo.kshadergraph").state == kast::ShaderLibrary::State::Compiled,
          "clearing the override reloads the file on disk");
    // Async (the editor): the compile runs on the worker; an override set meanwhile waits for it, then loads.
    library.setAsync(true);
    library.setOverride("shaders/demo.kshadergraph", demoText);
    const bool compiling = library.status("shaders/demo.kshadergraph").state == kast::ShaderLibrary::State::Compiling;
    library.setOverride("shaders/demo.kshadergraph", badGraph); // must not wait for the running compile
    library.wait();
    check(compiling && library.status("shaders/demo.kshadergraph").state == kast::ShaderLibrary::State::Failed &&
              contains(library.status("shaders/demo.kshadergraph").error, "node 6: ") &&
              library.get("shaders/demo.kshadergraph") != nullptr,
          "async: an override compiles on the worker; one set while it runs loads after it");
    library.setAsync(false);

    // Materials name graphs; their params are material properties (the Inspector's and Lua's path).
    kast::shaders().setRoot(dir);
    kast::shaders().setCompiler(compileShader);
    kast::Material material;
    check(kast::materialFromJson(nlohmann::ordered_json::parse(R"({"version": 3, "shader": "shaders/demo.kshadergraph"})"),
                                 material, error),
          ".mat \"shader\" takes a .kshadergraph");
    const std::optional<nlohmann::ordered_json> speed = kast::materialProperty(material, "speed");
    check(speed && *speed == 0.5 && kast::setMaterialProperty(material, "speed", 2.0, error) &&
              *kast::materialProperty(material, "speed") == 2.0 && !kast::setMaterialProperty(material, "tint", 2.0, error),
          std::format("a graph's params are material properties, checked against their types ({})", error).c_str());
    kast::shaders().setRoot(testRoot()); // forget
    removeTree(dir);
}

// The shader graph node library: every node type, in a tiny graph wired into each stage it may run in, generates, parses
// and compiles with shaderc; stage, SCREEN/DEPTH and Custom Expression errors are on the right node.
void checkShaderGraphNodes()
{
    std::string error;
    std::vector<kast::GraphIssue> issues;
    kast::ShaderSource source;
    std::vector<uint8_t> bytecode;
    const auto hasError = [](const std::vector<kast::GraphIssue>& list) {
        return std::ranges::any_of(list, [](const kast::GraphIssue& i) { return !i.warning; });
    };
    int checked = 0;
    for (const kast::GraphNodeDef& def : kast::shaderGraphNodeDefs()) {
        if (def.output)
            continue;
        // Node 2 is the one under test. Texture inputs take the "tex" param (node 3), dynamic ones a vec3 constant;
        // the rest keep their fallbacks. Its outputs are summed (add nodes from 10) into each stage's output node;
        // a texture output goes through a Custom Expression (node 4) sampling it.
        kast::ShaderGraph graph;
        graph.params = {{.name = "amount", .type = kast::ShaderParamType::Float, .value = {0.5f}},
                        {.name = "tex", .type = kast::ShaderParamType::Texture2D}};
        kast::GraphNode node{.id = 2, .type = std::string(def.type)};
        for (const kast::GraphSettingDef& setting : def.settings)
            node.settings[std::string(setting.name)] =
                setting.count ? nlohmann::ordered_json(std::vector<float>(static_cast<size_t>(setting.count), 0.5f))
                              : nlohmann::ordered_json(std::string(setting.fallback));
        if (def.type == "param")
            node.settings["param"] = "amount";
        if (def.type == kast::kCustomExpression) {
            node.settings["inputs"] = "vec3 a, float b, texture2D t";
            node.settings["outputs"] = "vec3 result, float c";
            node.settings["code"] = "result = a * b;\nc = b + texture2DLod(t, vec2_splat(0.5), 0.0).r;";
        }
        graph.nodes.push_back(node);
        graph.nodes.push_back({.id = 3, .type = "param", .settings = {{"param", "tex"}}});
        const kast::GraphNodeDef& pins = *kast::graphNodeDef(graph.nodes[0]);
        for (const kast::GraphPinDef& pin : pins.inputs) {
            if (pin.type == kast::PinType::Texture2D)
                graph.links.push_back({{3, "out"}, {2, std::string(pin.name)}});
            else if (pin.type == kast::PinType::Dynamic)
                graph.nodes[0].inputs[std::string(pin.name)] = {0.5f, 0.25f, 0.75f};
            else if (pin.fallback.empty()) // a required number: a constant of its width
                graph.nodes[0].inputs[std::string(pin.name)].assign(static_cast<size_t>(pin.type) + 1, 0.5f);
        }
        std::optional<kast::GraphPin> sum;
        int next = 10;
        for (const auto& [pin, code] : pins.outputs) {
            kast::GraphPin from{2, std::string(pin.name)};
            if (kast::outputPinType(graph, from) == kast::PinType::Texture2D) {
                graph.nodes.push_back({.id = 4, .type = std::string(kast::kCustomExpression),
                                       .settings = {{"inputs", "texture2D t"}, {"outputs", "vec4 rgba"},
                                                    {"code", "rgba = texture2DLod(t, vec2_splat(0.5), 0.0);"}}});
                graph.links.push_back({from, {4, "t"}});
                from = {4, "rgba"};
            }
            if (sum) {
                graph.nodes.push_back({.id = next, .type = "add"});
                graph.links.push_back({*sum, {next, "a"}});
                graph.links.push_back({from, {next, "b"}});
                from = {next++, "out"};
            }
            sum = from;
        }
        for (const auto& [stage, type, pin] : {std::tuple{kast::kStageVertex, "output_vertex", "VERTEX"},
                                               {kast::kStageFragment, "output_fragment", "EMISSION"},
                                               {kast::kStageLight, "output_light", "DIFFUSE_LIGHT"}}) {
            if (!(def.stages & stage))
                continue;
            const int id = 100 + static_cast<int>(stage);
            graph.nodes.push_back({.id = id, .type = type});
            graph.links.push_back({*sum, {id, pin}});
        }
        kast::ShaderGraph parsed;
        const bool strict = kast::shaderGraphFromJson(kast::shaderGraphToJson(graph), parsed, error);
        const std::string text = strict ? kast::generateKShader(parsed, issues) : "";
        std::string why = !strict ? error : hasError(issues) ? issues[0].message : "";
        bool ok = strict && !hasError(issues) && kast::parseShader(text, source, why);
        for (const kast::ShaderStage stage : {kast::ShaderStage::Fragment, kast::ShaderStage::Vertex})
            if (ok && (stage == kast::ShaderStage::Vertex ? def.stages & kast::kStageVertex
                                                          : def.stages & (kast::kStageFragment | kast::kStageLight))) {
                ok = compileShader(source, stage, bytecode, why) && !bytecode.empty();
                why = kast::mapGraphErrors(text, why);
            }
        check(ok, std::format("node '{}' generates, parses and compiles in every stage it allows ({}):\n{}", def.type,
                              why, text)
                      .c_str());
        checked += ok;
    }
    check(checked > 60, std::format("{} node types checked", checked).c_str());

    const auto issuesOf = [&](const std::string& json, kast::ShaderGraph& graph) {
        issues.clear();
        if (!kast::parseShaderGraph(json, graph, error))
            return std::vector<kast::GraphIssue>{{-1, error}};
        kast::generateKShader(graph, issues);
        return issues;
    };
    using Issues = std::vector<kast::GraphIssue>;
    kast::ShaderGraph graph;
    // Stage availability: the error is on the node that can't run there.
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 7, "type": "light_direction"}],
                      "links": [{"from": [7, "out"], "to": [1, "ALBEDO"]}]})", graph) ==
              Issues{{7, "Light Direction is not available in fragment() (only in light())"}},
          "LIGHT_DIRECTION into the Fragment Output: an error on the Light Direction node");
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_vertex"}, {"id": 3, "type": "scene_color"}],
                      "links": [{"from": [3, "rgb"], "to": [1, "VERTEX"]}]})", graph) ==
              Issues{{3, "Scene Color is not available in vertex() (only in fragment())"}},
          "Scene Color into the Vertex Output: an error on the Scene Color node");
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 1, "type": "output_vertex"}, {"id": 2, "type": "time"},
                      {"id": 5, "type": "normal"}, {"id": 6, "type": "add"}],
                      "links": [{"from": [2, "out"], "to": [6, "a"]}, {"from": [5, "out"], "to": [6, "b"]},
                                {"from": [6, "out"], "to": [1, "VERTEX"]}]})", graph) ==
              Issues{{5, "Normal (World) is not available in vertex() (only in fragment(), light())"}},
          "a fragment-only input two links upstream of the Vertex Output: the error is on it, not on the add");

    // SCREEN/DEPTH: Scene Color and Depth Fade only on blend and additive materials, with the node in the message.
    const std::string screenGraph = R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"},
        {"id": 4, "type": "scene_color"}, {"id": 6, "type": "depth_fade"}],
        "links": [{"from": [4, "rgb"], "to": [1, "ALBEDO"]}, {"from": [6, "fade"], "to": [1, "ALPHA"]}]})";
    const std::string depthGraph = R"({"version": 1, "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 6, "type": "depth_fade"}],
        "links": [{"from": [6, "fade"], "to": [1, "ALPHA"]}]})";
    for (const auto& [json, expected] : {std::pair{screenGraph, "node 4 (Scene Color) samples SCREEN_TEXTURE, which is"},
                                         std::pair{depthGraph, "node 6 (Depth Fade) samples DEPTH_TEXTURE, which is"}}) {
        std::string text;
        const bool generated = kast::parseShaderGraph(json, graph, error) && !(text = kast::generateKShader(graph, issues)).empty() &&
                               kast::parseShader(text, source, error);
        kast::Material material;
        std::string opaque, mask, blend, additive;
        material.alphaMode = kast::AlphaMode::Opaque;
        const bool opaqueRejected = !kast::checkShaderAlphaMode(material, source, opaque);
        material.alphaMode = kast::AlphaMode::Mask;
        const bool maskRejected = !kast::checkShaderAlphaMode(material, source, mask);
        material.alphaMode = kast::AlphaMode::Blend;
        const bool blendOk = kast::checkShaderAlphaMode(material, source, blend);
        material.alphaMode = kast::AlphaMode::Additive;
        const bool additiveOk = kast::checkShaderAlphaMode(material, source, additive);
        check(generated && opaqueRejected && maskRejected && blendOk && additiveOk &&
                  opaque == std::string(expected) + " only for blend and additive materials (drawn after the opaque scene "
                                                    "it copies); this material's alphaMode is opaque" &&
                  mask.starts_with(expected) && mask.ends_with("alphaMode is mask"),
              std::format("a graph sampling the screen is rejected on opaque and mask materials, naming the node: {} / {}",
                          opaque, mask)
                  .c_str());
    }

    // Custom Expression: its pins come from its settings, checked strictly; its code compiles inside its node marker,
    // so a bad body is a shaderc error on that node.
    for (const auto& [settings, expected] : std::initializer_list<std::pair<const char*, const char*>>{
             {R"("inputs": "vec5 a")", "nodes[1].settings.inputs: 'vec5' is not a type"},
             {R"("inputs": "vec3")", "nodes[1].settings.inputs: 'vec3' is not '<type> <name>'"},
             {R"("inputs": "float a,")", "nodes[1].settings.inputs: '' is not '<type> <name>'"},
             {R"("inputs": "float UV")", "nodes[1].settings.inputs: 'UV' is a reserved name"},
             {R"("inputs": "float n4_x")", "nodes[1].settings.inputs: 'n4_x' is a reserved name"},
             {R"("inputs": "float a", "outputs": "vec3 a")", "nodes[1].settings.outputs: 'a' is already a pin of this node"},
             {R"("outputs": "texture2D t")", "nodes[1].settings.outputs: 'texture2D' is not a type"}}) {
        const auto got = issuesOf(std::format(R"({{"version": 1, "nodes": [{{"id": 1, "type": "output_fragment"}},
            {{"id": 4, "type": "custom_expression", "settings": {{{}}}}}]}})", settings), graph);
        check(got.size() == 1 && got[0].node == -1 && got[0].message.starts_with(expected),
              std::format("a bad Custom Expression signature is rejected: {} (got: {})", expected,
                          got.empty() ? "" : got[0].message)
                  .c_str());
    }
    const Issues noise = issuesOf(R"({"version": 1, "nodes": [{"id": 4, "type": "noise", "settings": {"kind": "perlin"}}]})", graph);
    check(issuesOf(R"({"version": 1, "nodes": [{"id": 4, "type": "swizzle", "settings": {"mask": "xq"}}]})", graph) ==
                  Issues{{-1, "nodes[0].settings.mask: must be 1 to 4 of x, y, z, w (or of r, g, b, a), e.g. \"zyx\""}} &&
              noise.size() == 1 && noise[0].message.starts_with("nodes[0].settings.kind: must be \"value_2d\""),
          "a bad swizzle mask or noise kind is rejected with its JSON path");
    graph.nodes = {{.id = 1, .type = "output_fragment"}, {.id = 4, .type = "swizzle", .settings = {{"mask", "rgbaa"}}}};
    graph.links = {{{4, "out"}, {1, "ALBEDO"}}};
    graph.params.clear();
    check(kast::generateKShader(graph, issues).empty() && !issues.empty() && issues[0].node == 4 &&
              issues[0].message.starts_with("mask must be 1 to 4"),
          "a graph built in memory with a bad mask: the error is on its node");
    check(issuesOf(R"({"version": 1, "params": [{"name": "amount", "type": "float"}],
                      "nodes": [{"id": 1, "type": "output_fragment"},
                                {"id": 4, "type": "custom_expression", "settings": {"outputs": "float amount"}}],
                      "links": [{"from": [4, "amount"], "to": [1, "ALPHA"]}]})", graph) ==
              Issues{{4, "pin 'amount' has the name of a param"}},
          "a Custom Expression pin named like a param (a macro) is an error on its node");

    const std::string custom = R"({"version": 1, "params": [{"name": "amount", "type": "float", "default": 0.5}],
        "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 2, "type": "param", "settings": {"param": "amount"}},
                  {"id": 7, "type": "custom_expression",
                   "settings": {"inputs": "float x", "outputs": "vec3 color", "code": "color = vec3(x, CODE, 0.0);"}}],
        "links": [{"from": [2, "out"], "to": [7, "x"]}, {"from": [7, "color"], "to": [1, "ALBEDO"]}]})";
    const auto withCode = [&](std::string_view code) {
        std::string json = custom;
        json.replace(json.find("CODE"), 4, code);
        return json;
    };
    std::string text;
    check(issuesOf(withCode("amount * 2.0"), graph).empty() && !(text = kast::generateKShader(graph, issues)).empty() &&
              contains(text, "    // @node 7 custom_expression\n    vec3 n7_color;\n    {\n        float x = n2_out;\n"
                             "        vec3 color = vec3_splat(0.0);\n        color = vec3(x, amount * 2.0, 0.0);\n"
                             "        n7_color = color;\n    }\n") &&
              kast::parseShader(text, source, error) && compileShader(source, kast::ShaderStage::Fragment, bytecode, error),
          std::format("a Custom Expression's code is emitted in its own block after its marker and compiles ({}):\n{}",
                      error, text)
              .c_str());
    check(issuesOf(withCode("undefinedThing"), graph).empty() && !(text = kast::generateKShader(graph, issues)).empty() &&
              kast::parseShader(text, source, error) && !compileShader(source, kast::ShaderStage::Fragment, bytecode, error),
          "a Custom Expression using an undeclared name fails to compile");
    const std::string mapped = kast::mapGraphErrors(text, error);
    check(mapped.starts_with("node 7: line ") && contains(mapped, "undeclared identifier 'undefinedThing'"),
          std::format("its shaderc error names the Custom Expression's node: {}", mapped).c_str());
}

// The Shader Graph panels' edit API (ShaderGraphDoc): every canvas and Graph Inspector edit is one step that undo turns
// back into the exact JSON before it (and redo into the JSON after); refused edits change nothing; the file follows
// Save and disk changes; "New Shader Graph" compiles.
void checkShaderGraphEdits()
{
    std::string error;
    const std::filesystem::path dir = testRoot() / "kast_graph_edit";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "edit.kshadergraph";
    writeFile(file, R"({"version": 1,
        "params": [{"name": "speed", "type": "float", "default": 2.0}, {"name": "pattern", "type": "texture2D"}],
        "nodes": [{"id": 1, "type": "output_fragment", "position": [600.0, 0.0]},
                  {"id": 2, "type": "uv", "position": [0.0, 0.0]},
                  {"id": 3, "type": "time", "position": [0.0, 100.0]},
                  {"id": 4, "type": "param", "position": [0.0, 200.0], "settings": {"param": "speed"}},
                  {"id": 5, "type": "multiply", "position": [200.0, 100.0]},
                  {"id": 6, "type": "param", "position": [0.0, 300.0], "settings": {"param": "pattern"}},
                  {"id": 7, "type": "constant_float", "position": [200.0, 300.0], "settings": {"value": 0.5}},
                  {"id": 8, "type": "sample_texture", "position": [400.0, 200.0]}],
        "links": [{"from": [3, "out"], "to": [5, "a"]}, {"from": [4, "out"], "to": [5, "b"]},
                  {"from": [5, "out"], "to": [1, "ROUGHNESS"]}, {"from": [6, "out"], "to": [8, "texture"]},
                  {"from": [8, "rgb"], "to": [1, "ALBEDO"]}]})");
    ShaderGraphDoc doc;
    check(doc.open(file, error) && !doc.dirty() && !doc.history().canUndo(), std::format("a graph opens ({})", error).c_str());
    const auto text = [&] { return kast::shaderGraphText(doc.graph()); };
    const auto node = [&](int id) { return std::ranges::find(doc.graph().nodes, id, &kast::GraphNode::id); };
    const auto linked = [&](kast::GraphPin from, kast::GraphPin to) {
        return std::ranges::any_of(doc.graph().links, [&](const kast::GraphLink& l) { return l.from == from && l.to == to; });
    };
    // Runs edit (true if it did something), then checks it was one step that undo and redo replay exactly.
    const auto step = [&](const char* what, const std::function<bool()>& edit) {
        const std::string before = text();
        const size_t steps = doc.history().size();
        const bool done = edit();
        const std::string after = text();
        kast::ShaderGraph reread;
        check(done && after != before && doc.history().size() == steps + 1 && doc.dirty() &&
                  kast::parseShaderGraph(after, reread, error) && kast::shaderGraphText(reread) == after,
              std::format("{}: one step, and the graph still parses ({})", what, error).c_str());
        doc.undo();
        check(text() == before, std::format("{}: undo restores the exact JSON before it", what).c_str());
        doc.redo();
        check(text() == after, std::format("{}: redo restores the exact JSON after it", what).c_str());
    };
    // A refused edit: false + its message, nothing changed, no step.
    const auto refused = [&](const char* what, const std::function<bool()>& edit, const std::string& message) {
        const std::string before = text();
        const size_t steps = doc.history().size();
        error.clear();
        check(!edit() && text() == before && doc.history().size() == steps && contains(error, message),
              std::format("{} is refused: '{}'", what, error).c_str());
    };

    int added = 0;
    step("add", [&] { return (added = doc.addNode("add", {300.0f, 400.0f})) == 9; });
    check(node(added)->position == std::array{300.0f, 400.0f} && node(added)->settings.empty(), "add: at its position");
    step("add a Param node with its setting", [&] { return doc.addNode("param", {0.0f, 400.0f}, {{"param", "speed"}}) == 10; });
    check(doc.addNode("blur", {0.0f, 0.0f}, {}, &error) == 0 && contains(error, "unknown node type"), "an unknown type adds nothing");
    step("link", [&] { return doc.link({2, "out"}, {added, "a"}, error); });
    check(linked({2, "out"}, {added, "a"}), "link: there");
    step("relink replaces the input's link", [&] { return doc.link({7, "out"}, {added, "a"}, error); });
    check(linked({7, "out"}, {added, "a"}) && !linked({2, "out"}, {added, "a"}), "relink: the old link went");
    refused("a texture into a number", [&] { return doc.link({6, "out"}, {added, "b"}, error); }, "texture2D can't go into");
    refused("a number into a texture", [&] { return doc.link({7, "out"}, {8, "texture"}, error); }, "can't go into a texture2D");
    refused("pins that don't exist", [&] { return doc.link({1, "ALBEDO"}, {5, "a"}, error); }, "no such pins");
    step("link into a later node", [&] { return doc.link({added, "out"}, {5, "a"}, error); });
    refused("a link closing a cycle", [&] { return doc.link({5, "out"}, {added, "b"}, error); }, "cycle");
    step("unlink", [&] { return doc.unlink({5, "a"}); });
    check(!doc.unlink({5, "a"}), "unlinking a free input does nothing");
    step("move", [&] { return doc.move({{added, {320.0f, 420.0f}}, {7, {210.0f, 310.0f}}}); });
    check(node(added)->position == std::array{320.0f, 420.0f} && node(7)->position == std::array{210.0f, 310.0f},
          "move: both nodes where they went");
    step("a drag is one step", [&] {
        doc.beginGesture("Move");
        for (float x = 0.0f; x < 5.0f; x += 1.0f)
            doc.move({{added, {320.0f + x, 420.0f}}});
        doc.endGesture();
        return node(added)->position[0] == 324.0f;
    });
    step("a setting edit from activation to release is one step", [&] {
        doc.beginGesture("Edit value");
        const bool ok = doc.setSetting(7, "value", 0.6, error) && doc.setSetting(7, "value", 0.75, error);
        doc.endGesture();
        return ok && node(7)->settings.at("value") == 0.75;
    });
    step("add a Swizzle", [&] { return doc.addNode("swizzle", {0.0f, 500.0f}) == 11; });
    step("a string setting", [&] { return doc.setSetting(11, "mask", "zyx", error); });
    refused("a bad swizzle mask", [&] { return doc.setSetting(11, "mask", "xq", error); }, "mask: must be 1 to 4");
    refused("an unknown setting", [&] { return doc.setSetting(7, "colour", 1.0, error); }, "no setting 'colour'");
    step("a constant input", [&] { return doc.setInput(added, "b", {0.25f, 0.5f, 1.0f}, error); });
    refused("a constant too wide", [&] { return doc.setInput(added, "b", {1, 2, 3, 4, 5}, error); }, "nodes[");
    step("add a Custom Expression", [&] {
        return doc.addNode(kast::kCustomExpression, {0.0f, 600.0f},
                           {{"inputs", "float x"}, {"outputs", "float y"}, {"code", "y = x * 2.0;"}}) == 12;
    });
    step("link it", [&] { return doc.link({7, "out"}, {12, "x"}, error); });
    step("a Custom Expression's pins: links to pins it lost go", [&] { return doc.setSetting(12, "inputs", "vec3 z", error); });
    check(!linked({7, "out"}, {12, "x"}), "its old input's link went");
    refused("a malformed Custom Expression pin list", [&] { return doc.setSetting(12, "inputs", "vec5 a", error); }, "inputs:");
    std::vector<int> pasted;
    const std::string clip = doc.copy({3, 4, 5});
    step("paste: fresh ids, the links between them kept, top-left at the cursor", [&] {
        pasted = doc.paste(clip, {1000.0f, 1000.0f}, error);
        return pasted == std::vector{13, 14, 15};
    });
    check(linked({14, "out"}, {15, "b"}) && !linked({15, "out"}, {1, "ROUGHNESS"}) &&
              node(13)->position == std::array{1000.0f, 1000.0f} && node(15)->position == std::array{1200.0f, 1000.0f} &&
              node(14)->settings.at("param") == "speed",
          "paste: internal links on the new ids, links out of the copy dropped, positions offset");
    check(doc.paste("not a graph", {0.0f, 0.0f}, error).empty() && contains(error, "no shader graph nodes"),
          "pasting text that isn't graph nodes adds nothing");
    step("delete: the node and its links", [&] { return doc.remove({5}); });
    check(node(5) == doc.graph().nodes.end() &&
              std::ranges::none_of(doc.graph().links, [](const kast::GraphLink& l) { return l.from.node == 5 || l.to.node == 5; }),
          "delete: no node 5 or links to it");
    step("add a param", [&] { return doc.addParam(kast::ShaderParamType::Color) == 2 && doc.graph().params[2].name == "param"; });
    kast::ShaderParam renamed = doc.graph().params[0];
    renamed.name = "rate";
    step("rename a param: its Param nodes follow", [&] { return doc.setParam(0, renamed, error); });
    check(node(4)->settings.at("param") == "rate" && node(14)->settings.at("param") == "rate", "rename: Param nodes renamed");
    renamed.name = "pattern";
    refused("a param named like another", [&] { return doc.setParam(0, renamed, error); }, "already is a param");
    renamed.name = "2fast";
    refused("a param name that isn't an identifier", [&] { return doc.setParam(0, renamed, error); }, "letters, digits");
    step("link the float Param", [&] { return doc.link({4, "out"}, {added, "a"}, error); });
    kast::ShaderParam texture = doc.graph().params[0];
    texture.type = kast::ShaderParamType::Texture2D;
    step("retype a param: links its type can't make go", [&] { return doc.setParam(0, texture, error); });
    check(!linked({4, "out"}, {added, "a"}), "retype: the float link from the now texture Param went");
    step("remove a param", [&] { return doc.removeParam(2); });
    step("add a comment", [&] { return doc.addComment({"Scroll", {-20.0f, -40.0f, 400.0f, 300.0f}}) == 0; });
    step("resize a comment", [&] { return doc.setComment(0, {"Scroll UV", {-20.0f, -40.0f, 500.0f, 320.0f}}); });
    step("move a comment with its nodes", [&] { return doc.move({{2, {10.0f, 0.0f}}}, {{0, {-10.0f, -40.0f, 500.0f, 320.0f}}}); });
    step("delete a comment", [&] { return doc.remove({}, {}, {0}); });

    // File: Save clears the dirty flag and writes the graph; disk changes reload when clean, else are a conflict.
    check(doc.dirty() && doc.save(error) && !doc.dirty(), std::format("Save ({})", error).c_str());
    std::string saved;
    check(kast::readFile(file, saved) && saved == text(), "Save writes the graph's JSON");
    doc.undo();
    check(doc.dirty(), "undo past the save point: dirty");
    doc.redo();
    check(!doc.dirty(), "redo back to it: clean");
    check(doc.poll() == ShaderGraphDoc::DiskChange::None, "an unchanged file: nothing to do");
    kast::ShaderGraph changed;
    kast::parseShaderGraph(saved, changed, error);
    changed.nodes.erase(changed.nodes.begin()); // node 1, removed by another program
    std::erase_if(changed.links, [](const kast::GraphLink& l) { return l.to.node == 1 || l.from.node == 1; });
    rewrite(file, kast::shaderGraphText(changed));
    check(doc.poll() == ShaderGraphDoc::DiskChange::Reloaded && node(1) == doc.graph().nodes.end() &&
              !doc.history().canUndo() && !doc.dirty(),
          "changed on disk while clean: reloaded, history cleared");
    doc.addNode("uv", {0.0f, 0.0f});
    rewrite(file, saved);
    check(doc.poll() == ShaderGraphDoc::DiskChange::Conflict && doc.dirty() && node(1) == doc.graph().nodes.end(),
          "changed on disk while dirty: a conflict, the edits kept");
    check(doc.poll() == ShaderGraphDoc::DiskChange::None, "the conflict is reported once per change");
    check(doc.open(file, error) && !doc.dirty() && node(1) != doc.graph().nodes.end() && !doc.history().canUndo(),
          "Reload: the file's graph, history cleared");

    // "New Shader Graph": the starter generates with no issue, parses and compiles.
    std::filesystem::path created;
    std::string kshader;
    std::vector<std::string> warnings;
    kast::ShaderSource source;
    std::vector<uint8_t> bytecode;
    const bool compiled = writeNewShaderGraph(dir, created, error) && created.filename() == "new_shader_graph.kshadergraph" &&
                          kast::readFile(created, kshader) &&
                          kast::shaderFileText(created.string(), kshader, error, &warnings) && warnings.empty() &&
                          kast::parseShader(kshader, source, error) && source.params.size() == 1 &&
                          source.params[0].name == "tint" &&
                          compileShader(source, kast::ShaderStage::Fragment, bytecode, error) && !bytecode.empty();
    check(compiled, std::format("New Shader Graph writes a graph that compiles, without warnings ({}{})", error,
                                warnings.empty() ? "" : warnings[0]).c_str());
    std::filesystem::path second;
    check(writeNewShaderGraph(dir, second, error) && second.filename() == "new_shader_graph_2.kshadergraph",
          "a second New Shader Graph gets the next free name");
    removeTree(dir);
}

// Built-in primitive and terrain tangents: unit length, perpendicular to the normal, handedness +-1, along texture u.
void checkPrimitiveTangents()
{
    for (const kast::Primitive primitive : {kast::Primitive::Cube, kast::Primitive::Plane, kast::Primitive::Sphere}) {
        const kast::MeshData mesh = kast::primitiveMesh(primitive);
        bool ok = !mesh.vertices.empty();
        for (const kast::MeshVertex& v : mesh.vertices) {
            const bx::Vec3 n{v.nx, v.ny, v.nz}, t{v.tx, v.ty, v.tz};
            ok = ok && near(bx::length(t), 1.0f, 1e-4f) && near(bx::dot(n, t), 0.0f, 1e-4f) && (v.tw == 1.0f || v.tw == -1.0f);
        }
        // Per triangle: the tangent points along +u, the bitangent cross(n, t) * w along -v (image up).
        for (size_t i = 0; ok && i + 2 < mesh.indices.size(); i += 3) {
            const kast::MeshVertex& a = mesh.vertices[mesh.indices[i]];
            const kast::MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
            const kast::MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
            const bx::Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
            const float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-8f || bx::length(bx::cross(e1, e2)) < 1e-8f)
                continue; // the sphere's pole triangles (two corners at the pole)
            const bx::Vec3 dpdu = bx::mul(bx::sub(bx::mul(e1, dv2), bx::mul(e2, dv1)), 1.0f / det);
            const bx::Vec3 dpdv = bx::mul(bx::sub(bx::mul(e2, du1), bx::mul(e1, du2)), 1.0f / det);
            const bx::Vec3 n{a.nx, a.ny, a.nz}, t{a.tx, a.ty, a.tz};
            ok = bx::dot(t, dpdu) > 0.0f && bx::dot(bx::mul(bx::cross(n, t), a.tw), dpdv) < 0.0f;
            // Front faces wind counter-clockwise: the geometric normal agrees with the vertex normal.
            ok = ok && bx::dot(bx::cross(e1, e2), n) > 0.0f;
        }
        check(ok, std::format("primitive {} tangents: unit, perpendicular, along u with image-up bitangent, CCW fronts",
                              static_cast<int>(primitive)).c_str());
    }
    bool terrain = true;
    for (const bx::Vec3 n : {bx::Vec3{0, 1, 0}, bx::normalize(bx::Vec3{0.5f, 1, 0}), bx::normalize(bx::Vec3{-0.3f, 1, 0.8f})}) {
        const bx::Vec3 t = kast::terrainTangent(n);
        terrain = terrain && near(bx::length(t), 1.0f) && near(bx::dot(t, n), 0.0f) && t.x > 0.0f &&
                  bx::cross(n, t).z < 0.0f; // bitangent (w 1) toward -Z, image up for v = +Z
    }
    check(terrain, "terrain tangents: unit, perpendicular, along +X with image up toward -Z");
}

// glTF material mapping (models/material_features.gltf): normal (with scale), occlusion (with strength) and emissive
// textures, doubleSided, KHR_materials_unlit; and the generated model tangents.
void checkMaterialFeatureImport()
{
    std::string error;
    const auto model = importModel(KAST_TEST_PROJECT_DIR, "models/material_features.gltf", error);
    check(model && model->materials.size() == 2, std::format("material_features.gltf loads with 2 materials ({})", error).c_str());
    if (!model || model->materials.size() != 2)
        return;
    const kast::Material& a = model->materials[0].material;
    const kast::Material& b = model->materials[1].material;
    check(model->materials[0].name == "Featured" && a.normalTexture == "textures/normal_bumps.png" && near(a.normalScale, 0.5f),
          "glTF normalTexture and its scale");
    check(a.occlusionTexture == "textures/ao_grid.png" && near(a.occlusionStrength, 0.25f), "glTF occlusionTexture and its strength");
    check(a.emissiveTexture == "textures/emissive_stripes.png" && a.emissiveEnabled, "glTF emissiveTexture");
    check(a.cullMode == kast::CullMode::None && b.cullMode == kast::CullMode::Back, "glTF doubleSided: cull none; absent: back");
    check(a.unlit && !b.unlit, "glTF KHR_materials_unlit: unlit");
    check(b.normalTexture.empty() && b.occlusionTexture.empty() && b.emissiveTexture.empty() && b.normalScale == 1.0f &&
              b.occlusionStrength == 1.0f,
          "a glTF material without them gets the defaults");
    bool tangents = !model->meshes.empty();
    for (const kast::ModelMesh& mesh : model->meshes)
        for (const kast::ModelVertex& v : mesh.vertices) // u along +X, v along +Z: image up is -Z
            tangents = tangents && near(v.tx, 1.0f) && near(v.ty, 0.0f) && near(v.tz, 0.0f) && v.tw == 1.0f &&
                       bx::cross(bx::Vec3{v.nx, v.ny, v.nz}, bx::Vec3{v.tx, v.ty, v.tz}).z * v.tw < 0.0f;
    check(tangents, "model tangents run along texture u with the bitangent toward image up");

    // Meshes without UVs still get unit tangents perpendicular to the normal.
    const auto obj = importModel(KAST_TEST_PROJECT_DIR, "models/cube.obj", error);
    bool perpendicular = obj != nullptr;
    for (const kast::ModelMesh& mesh : obj ? obj->meshes : std::vector<kast::ModelMesh>{})
        for (const kast::ModelVertex& v : mesh.vertices)
            perpendicular = perpendicular && near(bx::length(bx::Vec3{v.tx, v.ty, v.tz}), 1.0f, 1e-3f) &&
                            near(v.nx * v.tx + v.ny * v.ty + v.nz * v.tz, 0.0f, 1e-3f) && (v.tw == 1.0f || v.tw == -1.0f);
    check(perpendicular, "model tangents are unit and perpendicular to the normal (cube.obj)");
}

// u_time's clock: fixed steps add up exactly, paused Play holds it, it wraps at kShaderTimePeriod.
// The editor's library: compiles on worker threads (the frame loop never waits), keeps the last good shader through a
// recompile and a broken edit, and caches bytecode by source + version so a restart doesn't recompile.
void checkShaderLibraryAsync()
{
    using namespace std::chrono_literals;
    const std::filesystem::path dir = testRoot() / "kast_shader_async";
    removeTree(dir);
    std::filesystem::create_directories(dir / "shaders");
    const std::filesystem::path file = dir / "shaders/a.kshader";
    writeFile(file, "param float k = 1.0;\nvoid fragment() { ALBEDO *= k; }\n");
    std::atomic<int> compiles = 0;
    std::atomic<bool> release = false;
    // A stand-in for shaderc: holds each compile until released; "BROKEN" in the code fails like an undeclared name.
    const kast::ShaderCompiler slow = [&](const kast::ShaderSource& source, kast::ShaderStage stage,
                                          std::vector<uint8_t>& bytecode, std::string& error) {
        while (!release)
            std::this_thread::sleep_for(1ms);
        ++compiles;
        if (contains(source.code, "BROKEN")) {
            error = "line 3: error X3004: undeclared identifier 'BROKEN'";
            return false;
        }
        bytecode = {1, 2, static_cast<uint8_t>(stage)};
        return true;
    };
    kast::ShaderLibrary library;
    library.setRoot(dir);
    library.setCompiler(slow, "v1");
    library.setAsync(true);
    const auto start = std::chrono::steady_clock::now();
    const auto first = library.get("shaders/a.kshader");
    int frames = 0;
    for (; frames < 50; ++frames) // a frame loop: the library is polled once a frame
        library.reloadChanged();
    const auto status = library.status("shaders/a.kshader");
    check(!first && status.state == kast::ShaderLibrary::State::Compiling && !status.hasShader && status.declaration &&
              status.declaration->params.size() == 1 && compiles == 0 && std::chrono::steady_clock::now() - start < 1s,
          "async: 50 frames run while the first compile is held, get() is null and the params are already known");
    release = true;
    for (frames = 0; !library.get("shaders/a.kshader") && frames < 5000; ++frames) {
        library.reloadChanged();
        std::this_thread::sleep_for(1ms);
    }
    const auto good = library.get("shaders/a.kshader");
    check(good && compiles == 2 && good->stage(kast::ShaderStage::Shadow) == std::vector<uint8_t>{1, 2, 1} &&
              library.status("shaders/a.kshader").state == kast::ShaderLibrary::State::Compiled &&
              countLogged("Compiled shader 'shaders/a.kshader' in ") == 1,
          "async: the compile finishes on its own thread and a later frame takes it in");
    check(std::filesystem::exists(dir / ".kast/shader_cache" /
                                  (kast::shaderCacheKey("param float k = 1.0;\nvoid fragment() { ALBEDO *= k; }\n",
                                                        kast::ShaderStage::Fragment, "v1") +
                                   ".bin")),
          "the bytecode is cached in the project's .kast/shader_cache (a dot-folder: not shipped)");

    // A broken edit: the last good shader renders while it compiles and after it fails; the error has the line.
    release = false;
    rewrite(file, "void fragment()\n{\n    ALBEDO = BROKEN;\n}\n");
    library.reloadChanged();
    check(library.get("shaders/a.kshader") == good &&
              library.status("shaders/a.kshader").state == kast::ShaderLibrary::State::Compiling,
          "a recompile keeps handing out the last good shader");
    release = true;
    library.wait();
    const auto broken = library.status("shaders/a.kshader");
    check(library.get("shaders/a.kshader") == good && broken.state == kast::ShaderLibrary::State::Failed &&
              broken.hasShader && broken.error == "line 3: error X3004: undeclared identifier 'BROKEN'" &&
              countLogged("[error] Shader 'shaders/a.kshader': line 3: error X3004: undeclared identifier 'BROKEN'; the "
                          "last good version keeps rendering") == 1,
          "a broken edit keeps the last good shader and reports the error with its line");
    rewrite(file, "void fragment() { ALBEDO = vec3(0.0, 0.0, 1.0); }\n");
    library.reloadChanged();
    library.wait();
    const auto fixed = library.get("shaders/a.kshader");
    check(fixed && fixed != good && library.status("shaders/a.kshader").state == kast::ShaderLibrary::State::Compiled &&
              countLogged("Recompiled shader 'shaders/a.kshader' in ") == 1,
          "the fixed edit replaces it (logged as a recompile)");

    // A restart: same source and version, no compile.
    compiles = 0;
    kast::ShaderLibrary restarted;
    restarted.setRoot(dir);
    restarted.setCompiler(slow, "v1");
    const auto cached = restarted.get("shaders/a.kshader");
    check(cached && compiles == 0 && cached->stage(kast::ShaderStage::Fragment) == fixed->stage(kast::ShaderStage::Fragment) &&
              countLogged("Compiled shader 'shaders/a.kshader' (cache hit)") == 1,
          "a restart loads unchanged shaders from the cache (logged as a cache hit)");
    kast::ShaderLibrary newTools;
    newTools.setRoot(dir);
    newTools.setCompiler(slow, "v2");
    check(newTools.get("shaders/a.kshader") && compiles == 2, "another version (templates, shaderc) misses the cache");
    check(kast::shaderCacheKey("a", kast::ShaderStage::Fragment, "v") != kast::shaderCacheKey("a", kast::ShaderStage::Shadow, "v") &&
              kast::shaderCacheKey("a", kast::ShaderStage::Fragment, "v") != kast::shaderCacheKey("b", kast::ShaderStage::Fragment, "v") &&
              kast::shaderCacheKey("ab", kast::ShaderStage::Fragment, "") != kast::shaderCacheKey("a", kast::ShaderStage::Fragment, "b") &&
              kast::shaderCacheKey("a", kast::ShaderStage::Fragment, "v").size() == 16,
          "cache keys differ by text, stage and version");
    removeTree(dir);
}

void checkShaderClock()
{
    kast::ShaderClock clock;
    check(clock.seconds() == 0.0f, "u_time starts at 0");
    for (int i = 0; i < 60; ++i)
        clock.advance(0.25);
    check(clock.seconds() == 15.0f, "60 steps of 0.25 s: exactly 15 s");
    kast::ShaderClock wrap;
    wrap.advance(kast::kShaderTimePeriod - 1.0);
    wrap.advance(1.5);
    check(near(wrap.seconds(), 0.5f), "u_time wraps at kShaderTimePeriod");
    check(kast::kShaderTimePeriod == 3600.0, "the documented wrap period is 3600 s");

    // The editor: advances in edit mode and Play by the frame's dt, holds while paused.
    Document doc;
    updatePlay(doc, 0.125f);
    check(doc.shaderClock.seconds() == 0.125f, "u_time advances in edit mode");
    startPlay(doc);
    updatePlay(doc, 0.125f);
    check(doc.shaderClock.seconds() == 0.25f, "u_time advances in Play by exactly dt (--fixed-dt)");
    togglePause(doc);
    updatePlay(doc, 0.125f);
    updatePlay(doc, 0.125f);
    check(doc.play == PlayState::Paused && doc.shaderClock.seconds() == 0.25f, "u_time holds while Play is paused");
    togglePause(doc);
    updatePlay(doc, 0.125f);
    check(doc.shaderClock.seconds() == 0.375f, "u_time resumes with Play");
    stopPlay(doc);
}

void checkMaterialLibrary()
{
    const std::filesystem::path dir = testRoot() / "kast_material_check";
    removeTree(dir);
    std::filesystem::create_directories(dir / "materials");
    writeFile(dir / "materials/red.mat", R"({"version": 1, "baseColor": [1, 0, 0, 1]})");
    writeFile(dir / "materials/bad.mat", R"({"version": 1, "metallic": 9})");
    writeFile(dir / "materials/notjson.mat", "{ nope");

    kast::MaterialLibrary library;
    library.setRoot(dir);
    check(&library.get("") == &kast::defaultMaterial(), "empty material path: the default material");
    check(library.get("materials/red.mat").baseColor[0] == 1.0f && library.get("materials/red.mat").baseColor[1] == 0.0f,
          "a .mat loads by project-relative path");
    const auto isError = [](const kast::Material& m) { return kast::materialToJson(m) == kast::materialToJson(kast::errorMaterial()); };
    check(isError(library.get("materials/missing.mat")), "missing .mat: the error material");
    check(isError(library.get("materials/bad.mat")) && isError(library.get("materials/notjson.mat")),
          "invalid .mat (bad value, bad JSON): the error material");
    check(near(kast::emissiveRadiance(kast::errorMaterial()), {1, 0, 1}), "the error material is magenta");
    check(isError(library.get("../outside.mat")), "a path escaping the project: the error material");
    for (int i = 0; i < 3; ++i) {
        library.get("materials/missing.mat");
        library.reloadChanged();
    }
    kast::MaterialLibrary second; // another renderer: still one line per path
    second.setRoot(dir);
    second.get("materials/missing.mat");
    check(countLogged("Material 'materials/missing.mat': cannot read the file") == 1, "a missing .mat is logged once");
    check(countLogged("Material 'materials/bad.mat': \"metallic\" must be a number between 0 and 1") == 1,
          "an invalid .mat is logged once, with the reason");

    rewrite(dir / "materials/red.mat", R"({"version": 1, "baseColor": [0, 0, 1, 1]})");
    library.reloadChanged();
    check(library.get("materials/red.mat").baseColor[2] == 1.0f, "a changed .mat reloads (external edit)");
    writeFile(dir / "materials/missing.mat", R"({"version": 1, "roughness": 0.9})");
    library.reloadChanged();
    check(library.get("materials/missing.mat").roughness == 0.9f, "a .mat that appears later loads");

    kast::Material saved;
    saved.metallic = 0.7f;
    saved.baseColorTexture = "textures/a.png";
    std::string error;
    kast::Material loaded;
    check(saveMaterial(dir / "materials/saved.mat", saved, error) &&
              kast::loadMaterial(dir / "materials/saved.mat", loaded, error) && loaded.metallic == 0.7f,
          "saveMaterial writes a loadable .mat");
    std::filesystem::path created;
    check(writeNewMaterial(dir / "materials", created, error) && created.filename() == "new_material.mat" &&
              kast::loadMaterial(created, loaded, error) &&
              kast::materialToJson(loaded) == kast::materialToJson(kast::defaultMaterial()),
          "Create > Material writes the default material");
    check(writeNewMaterial(dir / "materials", created, error) && created.filename() == "new_material_2.mat",
          "Create > Material picks a free name");

    // Renaming / moving a texture rewrites the .mat files pointing at it.
    std::filesystem::create_directories(dir / "textures");
    const std::vector<std::string> changed = remapMaterialPaths(dir, "textures", "art");
    check(changed == std::vector<std::string>{"materials/saved.mat"} &&
              kast::loadMaterial(dir / "materials/saved.mat", loaded, error) && loaded.baseColorTexture == "art/a.png",
          "moving a texture folder remaps .mat texture paths");
    check(remapMaterialPaths(dir, "art/a.png", "art/b.png").size() == 1 &&
              kast::loadMaterial(dir / "materials/saved.mat", loaded, error) && loaded.baseColorTexture == "art/b.png",
          "renaming a texture remaps .mat texture paths");
    check(remapMaterialPaths(dir, "art/other.png", "art/c.png").empty(), "unrelated renames change no .mat");

    // Renaming / moving a .mat (or its folder) rewrites the open scene's MeshRenderers pointing at it.
    Document doc;
    entt::registry& registry = doc.scene.registry();
    const entt::entity a = doc.scene.create("A");
    const entt::entity b = doc.scene.create("B");
    const entt::entity c = doc.scene.create("C");
    registry.emplace<kast::MeshRenderer>(a, kast::Primitive::Cube, "materials/red.mat");
    registry.emplace<kast::MeshRenderer>(b, kast::Primitive::Cube, "materials/blue.mat");
    registry.emplace<kast::MeshRenderer>(c, kast::Primitive::Cube, "");
    remapScenePaths(doc, "materials/red.mat", "materials/crimson.mat");
    check(registry.get<kast::MeshRenderer>(a).material == "materials/crimson.mat" &&
              registry.get<kast::MeshRenderer>(b).material == "materials/blue.mat" && doc.dirty,
          "renaming a .mat remaps the MeshRenderers using it");
    remapScenePaths(doc, "materials", "art/mats");
    check(registry.get<kast::MeshRenderer>(a).material == "art/mats/crimson.mat" &&
              registry.get<kast::MeshRenderer>(b).material == "art/mats/blue.mat" &&
              registry.get<kast::MeshRenderer>(c).material.empty(),
          "moving a folder remaps every MeshRenderer material inside it; empty stays empty");
}

void checkTextureDecode()
{
    std::string png, jpg;
    check(kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "textures/checker.png", png) &&
              kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "textures/bricks.jpg", jpg),
          "test textures present");
    const std::optional<kast::MipImage> image = kast::decodeImage(png, true);
    check(image && image->width == 64 && image->height == 64 && image->levels == 7, "PNG decodes with a full mip chain");
    size_t bytes = 0;
    for (int level = 0, size = 64; level < 7; ++level, size /= 2)
        bytes += static_cast<size_t>(size) * size * 4;
    check(image && image->rgba.size() == bytes, "mip chain size: 64x64 down to 1x1");
    check(image && image->rgba[0] == 40 && image->rgba[1] == 80 && image->rgba[2] == 220 && image->rgba[3] == 255,
          "row 0 is the image's top (blue marker cell at the top-left)");
    check(kast::decodeImage(jpg, false).has_value(), "JPG decodes");
    check(!kast::decodeImage("not an image", true) && !kast::decodeImage("", false), "undecodable bytes: nullopt");
}

// Authored KTX / DDS (tests/project/textures/gpu, sky_test/cube.*, made with texturec): parsed as stored, broken or
// mismatched ones rejected with why. createGpuTexture's GPU format check is covered by the ktx_* screenshot tests.
void checkGpuImages()
{
    const std::filesystem::path dir = std::filesystem::path(KAST_TEST_PROJECT_DIR) / "textures";
    const auto parse = [&](const char* file, bool cube, std::string& error) {
        std::string bytes;
        check(kast::readFile(dir / file, bytes), std::format("{} present", file).c_str());
        check(kast::isGpuImage(bytes), std::format("{} is a KTX / DDS by its magic", file).c_str());
        return kast::parseGpuImage(bytes, cube, error);
    };
    using Format = bgfx::TextureFormat;
    const struct {
        const char* file;
        Format::Enum format;
    } kTwoD[] = {{"gpu/checker_bc1.ktx", Format::BC1}, {"gpu/checker_bc3.dds", Format::BC3},
                 {"gpu/checker_bc7.ktx", Format::BC7}, {"gpu/checker_rgba8.dds", Format::RGBA8},
                 {"gpu/checker_etc1.ktx", Format::ETC1}};
    for (const auto& [file, format] : kTwoD) {
        std::string error;
        const std::optional<kast::GpuImageInfo> info = parse(file, false, error);
        check(info && info->format == format && info->width == 64 && info->height == 64 && info->mips == 7 && !info->cube,
              std::format("{}: 64x64, 7 mips, stored format kept ({})", file, error).c_str());
        check(!parse(file, true, error) && error == "a 2D texture, not a cubemap", std::format("{}: not a cube", file).c_str());
    }
    for (const char* file : {"sky_test/cube.ktx", "sky_test/cube.dds"}) {
        std::string error;
        const std::optional<kast::GpuImageInfo> info = parse(file, true, error);
        check(info && info->cube && info->width == 16 && info->mips == 5, std::format("{}: 16x16 cube, 5 mips ({})", file, error).c_str());
        check(!parse(file, false, error) && error == "a cubemap, not a 2D texture", std::format("{}: not 2D", file).c_str());
    }
    std::string error;
    check(!parse("gpu/truncated.dds", false, error) && error == "the file is truncated", "truncated DDS rejected");
    check(!parse("gpu/corrupt.ktx", false, error) && error == "not a valid KTX or DDS file", "corrupt KTX rejected");
    check(!kast::parseGpuImage("", false, error) && !kast::parseGpuImage("DDS ", false, error), "empty / header-only rejected");
    std::string png;
    check(kast::readFile(dir / "checker.png", png) && !kast::isGpuImage(png, "textures/checker.png"),
          "a PNG is not a KTX / DDS");
    check(kast::isGpuImage("junk", "a/b.KTX") && kast::isGpuImage("junk", "b.dds"), "by extension too, any case");
}

// Texture compression: usage picks the format (per material slot), texturec cooks through the cache keyed by the image,
// usage and version, a sky's faces cook to one cube, and the editor's TextureCooks runs misses on a worker.
void checkTextureCooks()
{
    using Format = bgfx::TextureFormat;
    using kast::TextureUsage;
    check(kast::cookedFormat(TextureUsage::Color, 64, 32, false) == Format::BC1 &&
              kast::cookedFormat(TextureUsage::Linear, 64, 32, false) == Format::BC1 &&
              kast::cookedFormat(TextureUsage::Color, 64, 32, true) == Format::BC3 &&
              kast::cookedFormat(TextureUsage::Linear, 64, 32, true) == Format::BC3 &&
              kast::cookedFormat(TextureUsage::Normal, 64, 32, false) == Format::BC5 &&
              kast::cookedFormat(TextureUsage::Normal, 64, 32, true) == Format::BC5,
          "usage formats: color and linear BC1 opaque / BC3 with alpha, normal BC5");
    check(kast::cookedFormat(TextureUsage::Color, 6, 8, false) == Format::RGBA8 &&
              kast::cookedFormat(TextureUsage::Normal, 8, 30, false) == Format::RGBA8,
          "a side not a multiple of 4 stays RGBA8 (D3D11 takes BCn in whole blocks)");
    check(kast::isSrgb(TextureUsage::Color) && !kast::isSrgb(TextureUsage::Linear) && !kast::isSrgb(TextureUsage::Normal) &&
              kast::colorUsage(true) == TextureUsage::Color && kast::colorUsage(false) == TextureUsage::Linear,
          "only color is sampled as sRGB; params and particles pick by their sRGB flag");

    kast::Material material;
    material.baseColorTexture = "base.png";
    material.metallicRoughnessTexture = "mr.png";
    material.normalTexture = "normal.png";
    material.occlusionTexture = "ao.png";
    material.emissiveTexture = "glow.png";
    material.params["albedo"].texture = "param_srgb.png";
    material.params["mask"].texture = "param_linear.png";
    material.params["speed"].numbers = {1.0f};
    kast::ShaderSource shader;
    shader.params.push_back({.name = "albedo", .type = kast::ShaderParamType::Texture2D, .srgb = true});
    shader.params.push_back({.name = "mask", .type = kast::ShaderParamType::Texture2D, .srgb = false});
    using Uses = std::vector<std::pair<std::string, TextureUsage>>;
    check(kast::materialTextures(material, &shader) ==
              Uses{{"base.png", TextureUsage::Color}, {"mr.png", TextureUsage::Linear}, {"normal.png", TextureUsage::Normal},
                   {"ao.png", TextureUsage::Linear}, {"glow.png", TextureUsage::Color},
                   {"param_srgb.png", TextureUsage::Color}, {"param_linear.png", TextureUsage::Linear}},
          "material slots: base color and emissive color, metallic-roughness and occlusion linear, normal normal, "
          "params by their sRGB flag");
    check(kast::materialTextures(material, nullptr).back() == std::pair<std::string, TextureUsage>{"param_linear.png", TextureUsage::Color},
          "params of an unknown shader count as color");
    check(kast::materialTextures(kast::Material{}, nullptr).empty(), "no textures, no usages");
    check(kast::texturePakPath("textures/a.png", TextureUsage::Normal) == "textures/a.png.normal.ktx" &&
              kast::texturePakPath("models/m.glb#0", TextureUsage::Color) == "models/m.glb#0.color.ktx",
          "pak paths: <path>.<usage>.ktx");
    check(kast::skyPakPath({"a", "b", "c", "d", "e", "f"}).starts_with(".kast_sky/") &&
              kast::skyPakPath({"a", "b", "c", "d", "e", "f"}) != kast::skyPakPath({"a", "b", "c", "d", "f", "e"}),
          "sky pak path: by its faces, in a dot-folder");

    // texturec (next to this program, as next to the editor): each usage's format with its mips, through the cache.
    const std::filesystem::path textures = std::filesystem::path(KAST_TEST_PROJECT_DIR) / "textures";
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / std::format("kast_texture_cache_{}", _getpid());
    std::filesystem::remove_all(cache);
    std::string png;
    check(kast::readFile(textures / "checker.png", png), "checker.png present");
    check(kast::textureCacheKey(png, TextureUsage::Color, "v1") != kast::textureCacheKey(png, TextureUsage::Linear, "v1") &&
              kast::textureCacheKey(png, TextureUsage::Color, "v1") != kast::textureCacheKey(png, TextureUsage::Color, "v2") &&
              kast::textureCacheKey(png, TextureUsage::Color, "v1") != kast::textureCacheKey(png + " ", TextureUsage::Color, "v1"),
          "cache key: image, usage and version each change it");
    for (const auto [usage, format] : {std::pair{TextureUsage::Color, Format::BC1}, std::pair{TextureUsage::Linear, Format::BC1},
                                       std::pair{TextureUsage::Normal, Format::BC5}}) {
        std::string ktx, error;
        bool cached = true;
        check(kast::cookTexture(png, usage, cookTextureWithTexturec, "v1", cache, ktx, cached, error) && !cached,
              std::format("texturec cooks checker.png as {} ({})", kast::usageName(usage), error).c_str());
        const std::optional<kast::GpuImageInfo> info = kast::parseGpuImage(ktx, false, error);
        check(info && info->format == format && info->width == 64 && info->height == 64 && info->mips == 7,
              std::format("{}: format {}, 64x64 with 7 mips ({})", kast::usageName(usage), static_cast<int>(format), error)
                  .c_str());
        std::string again;
        check(kast::cookTexture(png, usage, cookTextureWithTexturec, "v1", cache, again, cached, error) && cached &&
                  again == ktx,
              std::format("{}: the second cook is a cache hit", kast::usageName(usage)).c_str());
        check(kast::cookTexture(png, usage, cookTextureWithTexturec, "v2", cache, again, cached, error) && !cached,
              std::format("{}: a new cooker version misses the cache", kast::usageName(usage)).c_str());
    }
    std::string ktx, error;
    bool cached = false;
    // An image with alpha below 255 (cutout.png, RGBA) keeps it: BC3; checker.png (RGB) above cooked to BC1.
    std::string cutout;
    check(kast::readFile(textures / "cutout.png", cutout), "cutout.png present");
    for (const TextureUsage usage : {TextureUsage::Color, TextureUsage::Linear}) {
        check(kast::cookTexture(cutout, usage, cookTextureWithTexturec, "v1", cache, ktx, cached, error),
              std::format("texturec cooks cutout.png as {} ({})", kast::usageName(usage), error).c_str());
        const std::optional<kast::GpuImageInfo> info = kast::parseGpuImage(ktx, false, error);
        check(info && info->format == Format::BC3,
              std::format("cutout.png ({}): alpha keeps BC3 ({})", kast::usageName(usage), error).c_str());
    }
    writeFile(cache / (kast::textureCacheKey(png, TextureUsage::Color, "v1") + ".ktx"), "half a KTX");
    check(kast::cookTexture(png, TextureUsage::Color, cookTextureWithTexturec, "v1", cache, ktx, cached, error) && !cached &&
              kast::parseGpuImage(ktx, false, error),
          "a corrupt cache entry (a cut-short write) is cooked again");
    check(!kast::cookTexture("not an image", TextureUsage::Color, cookTextureWithTexturec, "v1", cache, ktx, cached, error) &&
              error == "cannot read it as a PNG or JPG image",
          "a file that is no image fails before texturec");
    check(!kast::cookTexture(png.substr(0, 60), TextureUsage::Color, cookTextureWithTexturec, "v1", cache, ktx, cached, error) &&
              error.starts_with("texturec failed"),
          std::format("a truncated PNG: texturec rejects it ({})", error).c_str());

    // A sky's six faces: one BC1 cube with its mips (the sky ignores alpha).
    std::array<kast::MipImage, 6> faces;
    for (size_t i = 0; i < faces.size(); ++i) {
        std::string bytes;
        kast::readFile(textures / "sky_test" / std::format("{}.png", kast::kSkyFaceNames[i]), bytes);
        faces[i] = kast::decodeImage(bytes, true).value_or(kast::MipImage{});
    }
    check(kast::skyFacesError(faces).empty(), "sky_test faces make a cube");
    const std::string cubeIn = kast::skyCubeKtx(faces);
    std::optional<kast::GpuImageInfo> info = kast::parseGpuImage(cubeIn, true, error);
    check(info && info->format == Format::RGBA8 && info->width == 16 && info->mips == 1, "sky input: an RGBA8 cube KTX");
    check(kast::cookTexture(cubeIn, TextureUsage::Color, cookTextureWithTexturec, "v1", cache, ktx, cached, error),
          std::format("texturec cooks the sky cube ({})", error).c_str());
    info = kast::parseGpuImage(ktx, true, error);
    check(info && info->cube && info->format == Format::BC1 && info->width == 16 && info->mips == 5,
          std::format("sky cube: BC1, 16x16, 5 mips ({})", error).c_str());

    // TextureCooks: a miss cooks on a worker (one cook per key, however many ask), a hit or a non-async cook is ready
    // at once, and a failed cook comes back empty with the cooker's error.
    std::atomic<int> runs = 0;
    std::atomic<bool> release = false;
    std::string stored; // what the fake cooker "cooks": a valid KTX, so the cache takes it
    check(kast::readFile(textures / "gpu/checker_bc1.ktx", stored), "checker_bc1.ktx present");
    kast::TextureCooks& cooks = kast::textureCooks();
    cooks.setCooker([&](std::string_view, TextureUsage usage, Format::Enum, std::string& out, std::string& why) {
        ++runs;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (usage == TextureUsage::Normal) {
            why = "no normals today";
            return false;
        }
        out = stored;
        return true;
    }, "worker");
    cooks.setAsync(true);
    const std::filesystem::path root = cache / "project";
    auto first = cooks.cook(root, png, TextureUsage::Color);
    auto second = cooks.cook(root, png, TextureUsage::Color);
    check(first.wait_for(std::chrono::seconds(0)) != std::future_status::ready, "async miss: not ready at once (the caller draws RGBA8)");
    release = true;
    check(first.get().ktx == stored && !first.get().cached && second.get().ktx == stored && runs == 1,
          "async miss: cooked once on a worker for both callers");
    auto hit = cooks.cook(root, png, TextureUsage::Color);
    check(hit.wait_for(std::chrono::seconds(0)) == std::future_status::ready && hit.get().cached && runs == 1 &&
              std::filesystem::exists(root / ".kast/texture_cache" / (kast::textureCacheKey(png, TextureUsage::Color, "worker") + ".ktx")),
          "async hit: ready at once from <root>/.kast/texture_cache");
    auto failed = cooks.cook(root, png, TextureUsage::Normal);
    check(failed.get().ktx.empty() && failed.get().error == "no normals today", "a failed cook: empty, with the error");
    cooks.setAsync(false);
    auto sync = cooks.cook(root, png, TextureUsage::Linear);
    check(sync.wait_for(std::chrono::seconds(0)) == std::future_status::ready && sync.get().ktx == stored,
          "not async: the cook runs inside the call");
    cooks.setCooker({}, {});
    check(!cooks.enabled(), "no cooker: textures load uncompressed");
    std::filesystem::remove_all(cache);
}

void checkMaterialScenes()
{
    // A scene saved before materials: MeshRenderer "color" is dropped with a warning, the scene loads.
    kast::Scene loaded;
    std::string error;
    kast::SceneLoadReport report;
    const auto json = nlohmann::ordered_json::parse(R"({"version": 5, "entities": [{"id": 1, "name": "Old", "parent": null,
        "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]},
                       "MeshRenderer": {"primitive": "sphere", "color": [0.3, 0.45, 0.85]}}}]})");
    check(kast::sceneFromJson(json, loaded, error, &report), "a scene with MeshRenderer \"color\" still loads");
    const auto* mesh = loaded.registry().try_get<kast::MeshRenderer>(loaded.children(entt::null).at(0));
    check(mesh && mesh->primitive == kast::Primitive::Sphere && mesh->material.empty(), "old MeshRenderer: default material");
    check(report.warnings.size() == 1 && contains(report.warnings[0], "\"color\" is no longer supported"),
          "old MeshRenderer \"color\": one warning");
    check(!kast::sceneToJson(loaded)["entities"][0]["components"]["MeshRenderer"].contains("color"),
          "re-saving drops \"color\"");

    const std::vector<bx::Vec3> centers = {{0, 0, 1}, {0, 0, 5}, {0, 0, 3}, {0, 0, -5}};
    check(kast::backToFront(centers, {0, 0, 0}) == std::vector<size_t>{1, 3, 2, 0}, "blend order: far to near");
    check(kast::backToFront({{1, 0, 0}, {-1, 0, 0}}, {0, 0, 0}) == std::vector<size_t>{0, 1}, "blend order: ties keep list order");
    check(kast::backToFront({}, {0, 0, 0}).empty(), "blend order: none");
}

void checkLuaMaterials()
{
    const std::filesystem::path dir = luaTestDir();
    std::filesystem::create_directories(dir / "materials");
    writeFile(dir / "materials/shared.mat", R"({"version": 1, "baseColor": [0.5, 0.5, 0.5, 1], "roughness": 0.3})");
    writeFile(dir / "materials/other.mat", R"({"version": 1, "metallic": 1})");
    writeFile(dir / "scripts/mat.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
end
local done = false
local function find(name)
    for e in kast.query("MeshRenderer") do
        if kast.get(e, "MeshRenderer").primitive == name then return e end
    end
end
return { update = function(dt)
    if done then return end
    done = true
    local a, b, c = find("cube"), find("sphere"), find("plane")
    assert(kast.getMaterialProperty(a, "roughness") == 0.3, "get reads the shared .mat")
    assert(kast.getMaterialProperty(a, "alphaMode") == "opaque", "get a string property")
    local color = kast.getMaterialProperty(a, "baseColor")
    assert(#color == 4 and color[1] == 0.5 and color[4] == 1, "get a color as an array")
    kast.setMaterialProperty(a, "baseColor", {1, 0, 0, 1})
    kast.setMaterialProperty(a, "metallic", 0.75)
    kast.setMaterialProperty(a, "emissive", {x = 0, y = 1, z = 0})
    kast.setMaterialProperty(a, "emissiveEnabled", true)
    kast.setMaterialProperty(a, "emissiveIntensity", 3)
    assert(kast.getMaterialProperty(a, "emissiveEnabled") == true and kast.getMaterialProperty(a, "emissiveIntensity") == 3,
           "get/set emission toggle and intensity")
    fails('"emissiveEnabled" must be true or false', function() kast.setMaterialProperty(a, "emissiveEnabled", 1) end)
    kast.setMaterialProperty(a, "alphaMode", "blend")
    assert(kast.getMaterialProperty(a, "baseColor")[1] == 1 and kast.getMaterialProperty(a, "metallic") == 0.75,
           "set changes the entity's instance")
    assert(kast.getMaterialProperty(a, "roughness") == 0.3, "the instance starts as a copy of the .mat")
    assert(kast.getMaterialProperty(b, "baseColor")[1] == 0.5 and kast.getMaterialProperty(b, "metallic") == 0,
           "another entity with the same .mat is unaffected")
    assert(kast.getMaterialProperty(c, "baseColor")[1] == 0.8, "no material path: the default material")
    fails('"metallic" must be a number between 0 and 1', function() kast.setMaterialProperty(a, "metallic", 2) end)
    fails('"baseColor" must be an array of 4 numbers', function() kast.setMaterialProperty(a, "baseColor", {1, 0, 0}) end)
    fails('"baseColor" must be an array of 4 numbers', function() kast.setMaterialProperty(a, "baseColor", {1, 0, 0, 1, 1}) end)
    fails('"alphaMode" must be', function() kast.setMaterialProperty(a, "alphaMode", "glow") end)
    fails('"roughness" must be a number', function() kast.setMaterialProperty(a, "roughness", "smooth") end)
    fails('unknown field "shine"', function() kast.setMaterialProperty(a, "shine", 1) end)
    kast.setMaterialProperty(a, "normalTexture", "textures/n.png")
    kast.setMaterialProperty(a, "normalScale", 0.5)
    kast.setMaterialProperty(a, "occlusionTexture", "textures/ao.png")
    kast.setMaterialProperty(a, "occlusionStrength", 0.25)
    kast.setMaterialProperty(a, "emissiveTexture", "textures/e.png")
    kast.setMaterialProperty(a, "uvScale", {4, 4})
    kast.setMaterialProperty(a, "uvOffset", {0.5, -1})
    kast.setMaterialProperty(a, "cullMode", "none")
    kast.setMaterialProperty(a, "unlit", true)
    assert(kast.getMaterialProperty(a, "normalTexture") == "textures/n.png" and kast.getMaterialProperty(a, "normalScale") == 0.5,
           "get/set normal map")
    assert(kast.getMaterialProperty(a, "occlusionTexture") == "textures/ao.png" and
           kast.getMaterialProperty(a, "occlusionStrength") == 0.25, "get/set occlusion")
    assert(kast.getMaterialProperty(a, "emissiveTexture") == "textures/e.png", "get/set emissive texture")
    local uvScale, uvOffset = kast.getMaterialProperty(a, "uvScale"), kast.getMaterialProperty(a, "uvOffset")
    assert(#uvScale == 2 and uvScale[1] == 4 and uvScale[2] == 4 and uvOffset[1] == 0.5 and uvOffset[2] == -1, "get/set UV transform")
    assert(kast.getMaterialProperty(a, "cullMode") == "none" and kast.getMaterialProperty(a, "unlit") == true, "get/set cull and unlit")
    assert(kast.getMaterialProperty(b, "cullMode") == "back" and kast.getMaterialProperty(b, "unlit") == false, "defaults: back, lit")
    fails('"cullMode" must be "none", "back" or "front"', function() kast.setMaterialProperty(a, "cullMode", "both") end)
    fails('"occlusionStrength" must be a number between 0 and 1', function() kast.setMaterialProperty(a, "occlusionStrength", 2) end)
    fails('"uvScale" must be an array of 2 finite numbers', function() kast.setMaterialProperty(a, "uvScale", {1}) end)
    fails('"unlit" must be true or false', function() kast.setMaterialProperty(a, "unlit", 1) end)
    kast.setMaterialProperty(a, "unlit", false)
    fails('unknown material property "shine"', function() kast.getMaterialProperty(a, "shine") end)
    fails('has no "MeshRenderer"', function() kast.setMaterialProperty(kast.create("Bare"), "metallic", 1) end)
    assert(kast.getMaterialProperty(a, "metallic") == 0.75, "rejected sets leave the instance unchanged")

    kast.set(b, "MeshRenderer", {material = "materials/other.mat"})
    assert(kast.getMaterialProperty(b, "metallic") == 1, "kast.set swaps the material")
    kast.setMaterialProperty(c, "roughness", 0.9)
    kast.set(c, "MeshRenderer", {primitive = "plane"})
    assert(kast.getMaterialProperty(c, "roughness") == 0.9, "kast.set without a material change keeps the instance")
    kast.set(c, "MeshRenderer", {material = "materials/shared.mat"})
    assert(kast.getMaterialProperty(c, "roughness") == 0.3, "kast.set to another material drops the instance")
end })");

    Document doc;
    doc.path = (dir / "mat.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Materials"};
    entt::registry& registry = doc.scene.registry();
    registry.emplace<kast::MeshRenderer>(doc.scene.create("A"), kast::Primitive::Cube, "materials/shared.mat");
    registry.emplace<kast::MeshRenderer>(doc.scene.create("B"), kast::Primitive::Sphere, "materials/shared.mat");
    registry.emplace<kast::MeshRenderer>(doc.scene.create("C"), kast::Primitive::Plane, "");
    doc.scene.systems = {"scripts/mat.lua"};
    const std::string before = kast::materialToJson(kast::defaultMaterial()).dump();
    const auto fileTime = std::filesystem::last_write_time(dir / "materials/shared.mat");

    startPlay(doc);
    updatePlay(doc, 0.1f);
    checkOk(doc.lua, 0, "kast.get/setMaterialProperty asserts in Lua pass");
    const auto* instance = registry.try_get<kast::MaterialInstance>(named(doc.scene, "A"));
    check(instance && instance->material.baseColor[0] == 1.0f && instance->material.alphaMode == kast::AlphaMode::Blend &&
              instance->material.uvScale == std::array<float, 2>{4.0f, 4.0f} && instance->material.cullMode == kast::CullMode::None &&
              near(kast::emissiveRadiance(instance->material), {0, 3, 0}),
          "the instance holds the per-entity values");
    check(!registry.all_of<kast::MaterialInstance>(named(doc.scene, "B")), "B has no instance");
    check(registry.get<kast::MeshRenderer>(named(doc.scene, "B")).material == "materials/other.mat", "kast.set wrote the material");
    stopPlay(doc);
    check(registry.view<kast::MaterialInstance>().empty(), "Stop discards every material instance");
    check(registry.get<kast::MeshRenderer>(named(doc.scene, "B")).material == "materials/shared.mat",
          "Stop restores the swapped material path");
    std::string text;
    check(kast::readFile(dir / "materials/shared.mat", text) && !contains(text, "0.75") &&
              std::filesystem::last_write_time(dir / "materials/shared.mat") == fileTime,
          "material edits from Lua never reach the .mat file");
    check(before == kast::materialToJson(kast::defaultMaterial()).dump(), "the default material is never edited");
}

// kast.get/setMaterialProperty on .kshader params, the Inspector's undo steps, New Shader and asset renames.
void checkShaderParamEditing()
{
    const std::filesystem::path dir = luaTestDir();
    std::filesystem::create_directories(dir / "shaders");
    std::filesystem::create_directories(dir / "materials");
    std::string params;
    check(kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "shaders/params.kshader", params),
          "tests/project/shaders/params.kshader reads");
    writeFile(dir / "shaders/params.kshader", params);
    const std::string mat = R"({"version": 3, "roughness": 0.3, "shader": "shaders/params.kshader",
                                "params": {"amount": 0.75, "tint": [1, 0.5, 0, 1]}})";
    writeFile(dir / "materials/p.mat", mat);
    kast::shaders().setRoot(dir);
    kast::shaders().setCompiler(compileShader);

    // The GPU-free functions under it.
    kast::Material m;
    std::string error;
    check(kast::loadMaterial(dir / "materials/p.mat", m, error), "p.mat loads");
    check(kast::materialProperty(m, "amount") == nlohmann::ordered_json(0.75) &&
              kast::materialProperty(m, "scale") == nlohmann::ordered_json(2.0) &&
              kast::materialProperty(m, "offset") == nlohmann::ordered_json::parse("[0.25, -0.5]") &&
              kast::materialProperty(m, "tint") == nlohmann::ordered_json::parse("[1, 0.5, 0, 1]") &&
              kast::materialProperty(m, "mask") == nlohmann::ordered_json("") &&
              kast::materialProperty(m, "roughness") == nlohmann::ordered_json(0.3) && !kast::materialProperty(m, "nope"),
          "materialProperty: built-in fields, then the .kshader's params (the .mat's value, else the default)");
    check(kast::setMaterialProperty(m, "tint3", nlohmann::ordered_json::parse("[0, 1, 0]"), error) &&
              m.params["tint3"].numbers == std::vector<float>{0, 1, 0} &&
              kast::setMaterialProperty(m, "mask", "textures/noise.png", error) && m.params["mask"].texture == "textures/noise.png",
          "setMaterialProperty sets params");
    check(!kast::setMaterialProperty(m, "tint3", 1.0, error) &&
              error == "param 'tint3' is a vec3: its value must be an array of 3 numbers" &&
              m.params["tint3"].numbers == std::vector<float>{0, 1, 0},
          "a wrong-typed param value is an error and changes nothing");
    check(!kast::setMaterialProperty(m, "nope", 1.0, error) && error == "unknown field \"nope\"",
          "a name that is neither a field nor a param is an unknown field");

    // Lua in Play: params read and write through the MaterialInstance; Stop resets them.
    writeFile(dir / "scripts/params.lua", R"(local function fails(part, fn)
    local ok, err = pcall(fn)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
end
local done = false
return { update = function(dt)
    if done then return end
    done = true
    local e
    for id in kast.query("MeshRenderer") do e = id end
    assert(kast.getMaterialProperty(e, "amount") == 0.75, "get a float param from the .mat")
    assert(kast.getMaterialProperty(e, "scale") == 2, "get a param's default")
    local tint = kast.getMaterialProperty(e, "tint")
    assert(#tint == 4 and tint[1] == 1 and tint[2] == 0.5, "get a color param")
    assert(kast.getMaterialProperty(e, "roughness") == 0.3, "built-in fields still read")
    kast.setMaterialProperty(e, "amount", 0.25)
    kast.setMaterialProperty(e, "offset", {x = 1, y = 2})
    kast.setMaterialProperty(e, "tint", {0, 0, 1})
    kast.setMaterialProperty(e, "mask", "textures/noise.png")
    assert(kast.getMaterialProperty(e, "amount") == 0.25, "set a float param")
    local offset = kast.getMaterialProperty(e, "offset")
    assert(offset[1] == 1 and offset[2] == 2, "set a vec2 param")
    assert(kast.getMaterialProperty(e, "mask") == "textures/noise.png", "set a texture param")
    fails("param 'amount' is a float: its value must be a number", function() kast.setMaterialProperty(e, "amount", {1, 2}) end)
    fails('unknown material property "glow"', function() kast.getMaterialProperty(e, "glow") end)
    fails('unknown field "glow"', function() kast.setMaterialProperty(e, "glow", 1) end)
end })");
    Document doc;
    doc.path = (dir / "params.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Params"};
    entt::registry& registry = doc.scene.registry();
    registry.emplace<kast::MeshRenderer>(doc.scene.create("P"), kast::Primitive::Cube, "materials/p.mat");
    doc.scene.systems = {"scripts/params.lua"};
    kast::shaders().setRoot({}); // as on Play's first frame before any render: Lua gives the library its root
    startPlay(doc);
    updatePlay(doc, 0.1f);
    checkOk(doc.lua, 0, "kast.get/setMaterialProperty on shader params: Lua asserts pass");
    const auto* instance = registry.try_get<kast::MaterialInstance>(named(doc.scene, "P"));
    check(instance && instance->material.params.at("amount").numbers == std::vector<float>{0.25f} &&
              instance->material.params.at("tint").numbers == std::vector<float>{0, 0, 1},
          "the instance holds the Lua param values");
    stopPlay(doc);
    std::string text;
    check(registry.view<kast::MaterialInstance>().empty() && kast::readFile(dir / "materials/p.mat", text) && text == mat,
          "Stop resets the params; the .mat is untouched");

    // The same for a .kshadergraph's params.
    writeFile(dir / "shaders/g.kshadergraph", R"({"version": 1, "params": [{"name": "amount", "type": "float", "default": 0.5},
        {"name": "tint", "type": "color", "default": [1, 1, 1, 1]}],
        "nodes": [{"id": 1, "type": "output_fragment"}, {"id": 2, "type": "param", "settings": {"param": "amount"}},
                  {"id": 3, "type": "param", "settings": {"param": "tint"}}, {"id": 4, "type": "swizzle", "settings": {"mask": "rgb"}}],
        "links": [{"from": [2, "out"], "to": [1, "ROUGHNESS"]}, {"from": [3, "out"], "to": [4, "v"]},
                  {"from": [4, "out"], "to": [1, "ALBEDO"]}]})");
    const std::string graphMat = R"({"version": 3, "shader": "shaders/g.kshadergraph", "params": {"amount": 0.75}})";
    writeFile(dir / "materials/g.mat", graphMat);
    writeFile(dir / "scripts/graph_params.lua", R"(local done = false
return { update = function(dt)
    if done then return end
    done = true
    local e
    for id in kast.query("MeshRenderer") do e = id end
    assert(kast.getMaterialProperty(e, "amount") == 0.75, "get a graph's float param from the .mat")
    assert(kast.getMaterialProperty(e, "tint")[2] == 1, "get a graph param's default")
    kast.setMaterialProperty(e, "amount", 0.25)
    kast.setMaterialProperty(e, "tint", {0, 0, 1, 1})
    assert(kast.getMaterialProperty(e, "amount") == 0.25 and kast.getMaterialProperty(e, "tint")[3] == 1,
           "set a graph's params")
    local ok, err = pcall(function() kast.setMaterialProperty(e, "amount", {1, 2}) end)
    assert(not ok and string.find(err, "param 'amount' is a float", 1, true), "a graph param's type is checked")
end })");
    Document graphDoc;
    graphDoc.path = (dir / "graph_params.scene").string();
    graphDoc.project = kast::Project{.root = dir, .name = "Params"};
    entt::registry& graphRegistry = graphDoc.scene.registry();
    graphRegistry.emplace<kast::MeshRenderer>(graphDoc.scene.create("G"), kast::Primitive::Cube, "materials/g.mat");
    graphDoc.scene.systems = {"scripts/graph_params.lua"};
    kast::shaders().setRoot({});
    startPlay(graphDoc);
    updatePlay(graphDoc, 0.1f);
    checkOk(graphDoc.lua, 0, "kast.get/setMaterialProperty on .kshadergraph params: Lua asserts pass");
    const auto* graphInstance = graphRegistry.try_get<kast::MaterialInstance>(named(graphDoc.scene, "G"));
    check(graphInstance && graphInstance->material.params.at("amount").numbers == std::vector<float>{0.25f} &&
              graphInstance->material.params.at("tint").numbers == std::vector<float>{0, 0, 1, 1},
          "the instance holds the Lua graph param values");
    stopPlay(graphDoc);
    check(graphRegistry.view<kast::MaterialInstance>().empty() && kast::readFile(dir / "materials/g.mat", text) &&
              text == graphMat,
          "Stop resets the graph params; the .mat is untouched");

    // Inspector edits: each a step that saves the .mat; undo / redo save the version before / after.
    UndoStack history;
    kast::Material before, after;
    check(kast::loadMaterial(dir / "materials/p.mat", before, error), "p.mat loads again");
    after = before;
    check(kast::setMaterialProperty(after, "amount", 0.1, error) && saveMaterial(dir / "materials/p.mat", after, error),
          "an Inspector edit saves");
    pushMaterialStep(history, dir / "materials/p.mat", "Edit p.mat", before, after);
    pushMaterialStep(history, dir / "materials/p.mat", "Edit p.mat", after, after);
    kast::Material read;
    history.undo();
    check(history.size() == 1 && kast::loadMaterial(dir / "materials/p.mat", read, error) &&
              read.params.at("amount").numbers == std::vector<float>{0.75f},
          "undo saves the material as it was (an edit that changed nothing is no step)");
    history.redo();
    check(kast::loadMaterial(dir / "materials/p.mat", read, error) && read.params.at("amount").numbers == std::vector<float>{0.1f},
          "redo saves the edit again");

    // New Shader: a starter that parses and compiles, with a commented-out vertex().
    std::filesystem::path created;
    kast::ShaderSource source;
    check(writeNewShader(dir / "shaders", created, error) && created.filename() == "new_shader.kshader" &&
              kast::readFile(created, text) && kast::parseShader(text, source, error) && source.params.size() == 3 &&
              !source.hasVertex && contains(text, "// void vertex()"),
          "Create > Shader writes a starter with params, fragment() and a commented-out vertex()");
    std::vector<uint8_t> bytecode;
    check(compileShader(source, kast::ShaderStage::Fragment, bytecode, error) &&
              compileShader(source, kast::ShaderStage::Shadow, bytecode, error),
          std::format("the starter compiles: {}", error).c_str());
    check(writeNewShader(dir / "shaders", created, error) && created.filename() == "new_shader_2.kshader",
          "Create > Shader picks a free name");

    // Renames: a .mat's shader path and texture params follow, as its textures do.
    const std::vector<std::string> changed = remapMaterialPaths(dir, "shaders/params.kshader", "shaders/renamed.kshader");
    check(changed == std::vector<std::string>{"materials/p.mat"} && kast::loadMaterial(dir / "materials/p.mat", read, error) &&
              read.shader == "shaders/renamed.kshader",
          "renaming a .kshader remaps the .mat's shader");
    writeFile(dir / "materials/p.mat", R"({"version": 3, "shader": "shaders/params.kshader", "normalTexture": "textures/n.png",
                                           "params": {"mask": "textures/noise.png", "amount": 1}})");
    check(remapMaterialPaths(dir, "textures", "art").size() == 1 && kast::loadMaterial(dir / "materials/p.mat", read, error) &&
              read.params.at("mask").texture == "art/noise.png" && read.normalTexture == "art/n.png" &&
              read.params.at("amount").numbers == std::vector<float>{1.0f},
          "moving a texture folder remaps texture params and every texture field");
    kast::shaders().setRoot({});
    removeTree(dir);
}

// --- Models and animation ---

// A GLB in memory: a chain of joints nodes and one triangle-list mesh skinned to them, vertex i fully on joint i
// (unweightedLast: the last vertex has no weights; decoyJoint1: a joint-less node also named "Joint1" comes first in the
// scene, and clip "Move" animates the real Joint1, node 1; bodyInSkeleton: the mesh node is a child of the last joint,
// so the scene's one root node is Joint0).
std::string skinnedGlb(int joints, bool unweightedLast = false, bool decoyJoint1 = false, bool bodyInSkeleton = false)
{
    std::string bin;
    const auto put = [&bin](const auto& value) { bin.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    const int vertices = joints + (3 - joints % 3) % 3; // whole triangles
    for (int v = 0; v < vertices; ++v)
        for (float c : {static_cast<float>(v % 7), static_cast<float>(v / 7), 0.0f})
            put(c);
    const size_t jointsOffset = bin.size();
    for (int v = 0; v < vertices; ++v)
        for (uint16_t j : {static_cast<uint16_t>(std::min(v, joints - 1)), uint16_t{0}, uint16_t{0}, uint16_t{0}})
            put(j);
    const size_t weightsOffset = bin.size();
    for (int v = 0; v < vertices; ++v)
        for (float w : {unweightedLast && v == vertices - 1 ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f})
            put(w);
    const size_t bindsOffset = bin.size();
    for (int j = 0; j < joints; ++j)
        for (int i = 0; i < 16; ++i)
            put(i % 5 == 0 ? 1.0f : 0.0f); // identity
    const size_t timesOffset = bin.size();
    for (float t : {0.0f, 1.0f})
        put(t);
    const size_t valuesOffset = bin.size();
    for (float c : {0.0f, 0.1f, 0.0f, 1.0f, 0.1f, 0.0f})
        put(c);
    nlohmann::json nodes = nlohmann::json::array();
    for (int j = 0; j < joints; ++j) {
        nlohmann::json node = {{"name", std::format("Joint{}", j)}, {"translation", {0.0, 0.1, 0.0}}};
        if (j + 1 < joints || bodyInSkeleton)
            node["children"] = {j + 1};
        nodes.push_back(node);
    }
    nodes.push_back({{"name", "Body"}, {"mesh", 0}, {"skin", 0}});
    if (decoyJoint1)
        nodes.push_back({{"name", "Joint1"}});
    nlohmann::json skinJoints = nlohmann::json::array();
    for (int j = 0; j < joints; ++j)
        skinJoints.push_back(j);
    const auto count = static_cast<size_t>(vertices);
    nlohmann::json gltf;
    gltf["asset"] = {{"version", "2.0"}};
    gltf["scene"] = 0;
    gltf["scenes"] = nlohmann::json::array(
        {{{"nodes", decoyJoint1      ? nlohmann::json{joints + 1, 0, joints}
                    : bodyInSkeleton ? nlohmann::json{0}
                                     : nlohmann::json{0, joints}}}});
    if (decoyJoint1)
        gltf["animations"] = nlohmann::json::array(
            {{{"name", "Move"},
              {"channels", nlohmann::json::array({{{"sampler", 0}, {"target", {{"node", 1}, {"path", "translation"}}}}})},
              {"samplers", nlohmann::json::array({{{"input", 4}, {"output", 5}}})}}});
    gltf["nodes"] = nodes;
    gltf["skins"] = nlohmann::json::array({{{"joints", skinJoints}, {"inverseBindMatrices", 3}}});
    nlohmann::json primitive = {{"attributes", {{"POSITION", 0}, {"JOINTS_0", 1}, {"WEIGHTS_0", 2}}}};
    gltf["meshes"] = nlohmann::json::array({{{"primitives", nlohmann::json::array({primitive})}}});
    gltf["buffers"] = nlohmann::json::array({{{"byteLength", bin.size()}}});
    gltf["bufferViews"] = nlohmann::json::array(
        {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", jointsOffset}},
         {{"buffer", 0}, {"byteOffset", jointsOffset}, {"byteLength", weightsOffset - jointsOffset}},
         {{"buffer", 0}, {"byteOffset", weightsOffset}, {"byteLength", bindsOffset - weightsOffset}},
         {{"buffer", 0}, {"byteOffset", bindsOffset}, {"byteLength", timesOffset - bindsOffset}},
         {{"buffer", 0}, {"byteOffset", timesOffset}, {"byteLength", valuesOffset - timesOffset}},
         {{"buffer", 0}, {"byteOffset", valuesOffset}, {"byteLength", bin.size() - valuesOffset}}});
    gltf["accessors"] = nlohmann::json::array(
        {{{"bufferView", 0}, {"componentType", 5126}, {"count", count}, {"type", "VEC3"},
          {"min", {0.0, 0.0, 0.0}}, {"max", {6.0, static_cast<double>(vertices / 7), 0.0}}},
         {{"bufferView", 1}, {"componentType", 5123}, {"count", count}, {"type", "VEC4"}},
         {{"bufferView", 2}, {"componentType", 5126}, {"count", count}, {"type", "VEC4"}},
         {{"bufferView", 3}, {"componentType", 5126}, {"count", joints}, {"type", "MAT4"}},
         {{"bufferView", 4}, {"componentType", 5126}, {"count", 2}, {"type", "SCALAR"}, {"min", {0.0}}, {"max", {1.0}}},
         {{"bufferView", 5}, {"componentType", 5126}, {"count", 2}, {"type", "VEC3"}}});
    std::string json = gltf.dump();
    json.append((4 - json.size() % 4) % 4, ' ');
    bin.append((4 - bin.size() % 4) % 4, '\0');
    std::string glb;
    const auto put32 = [&glb](uint32_t value) { glb.append(reinterpret_cast<const char*>(&value), 4); };
    put32(0x46546C67); // "glTF"
    put32(2);
    put32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    put32(static_cast<uint32_t>(json.size()));
    put32(0x4E4F534A); // "JSON"
    glb += json;
    put32(static_cast<uint32_t>(bin.size()));
    put32(0x004E4942); // "BIN\0"
    glb += bin;
    return glb;
}

bx::Vec3 extent(const kast::Aabb& box) { return bx::sub(box.max, box.min); }

void logModel(const char* name, const kast::Model& model)
{
    size_t joints = 0;
    for (const kast::ModelMesh& mesh : model.meshes)
        joints = std::max(joints, mesh.joints.size());
    kast::log::info(std::format("{}: {} meshes, {} nodes, max {} joints, {} clips, {} materials, bounds ({:.3f} {:.3f} "
                                "{:.3f}) to ({:.3f} {:.3f} {:.3f})",
                                name, model.meshes.size(), model.nodes.size(), joints, model.clips.size(),
                                model.materials.size(), model.bounds.min.x, model.bounds.min.y, model.bounds.min.z,
                                model.bounds.max.x, model.bounds.max.y, model.bounds.max.z));
}

bool uprightHuman(const kast::Model& model)
{
    const bx::Vec3 size = extent(model.bounds);
    return size.y > 1.0f && size.y < 2.5f && size.y > size.x && size.y > size.z;
}

void checkModelLoading()
{
    const std::filesystem::path tests = KAST_TEST_PROJECT_DIR;
    std::string error;

    const auto character = importModel(tests, "models/female-128-anim.glb", error);
    check(character != nullptr, "female-128-anim.glb loads");
    if (character) {
        logModel("models/female-128-anim.glb", *character);
        check(character->meshes.size() == 2, "the character has 2 meshes");
        check(std::ranges::all_of(character->meshes, [](const kast::ModelMesh& m) { return m.joints.size() == 66; }),
              "both character meshes are skinned to 66 joints");
        check(character->clips.size() == 178, "the character has 178 clips");
        check(std::ranges::all_of(character->clips,
                                  [](const kast::Clip& c) { return !c.name.empty() && c.duration > 0.0f; }),
              "every clip is named and has a duration");
        check(kast::findClip(*character, "Idle_A") >= 0 && kast::findClip(*character, "Angry") >= 0,
              "clips found by name");
        check(kast::defaultClip(*character) == "Idle_A", "the default clip is Idle_A (no exact Idle)");
        check(!character->overBoneLimit, "66 joints are within the limit");
        check(character->materials.size() == 2, "the character has 2 embedded materials");
        if (character->materials.size() == 2) {
            const kast::Material& a = character->materials[0].material;
            const kast::Material& b = character->materials[1].material;
            check(near(a.baseColor[0], 0.5714685f) && near(a.baseColor[1], 0.1221148f) &&
                      near(a.baseColor[2], 0.7086533f) && near(a.baseColor[3], 1.0f),
                  "material 0 base color factor from the file");
            check(near(b.baseColor[0], 0.7990986f) && near(b.baseColor[1], 0.4019782f) &&
                      near(b.baseColor[2], 0.0423115f),
                  "material 1 base color factor from the file");
            check(near(a.metallic, 0.0f) && near(a.roughness, 0.5f) && near(b.metallic, 0.0f) && near(b.roughness, 0.5f),
                  "metallic 0 / roughness 0.5 from the file");
        }
        check(uprightHuman(*character), "the character stands upright, human sized");
        bool weightsOk = true;
        for (const kast::ModelMesh& mesh : character->meshes)
            for (const kast::ModelVertex& v : mesh.vertices)
                weightsOk = weightsOk && near(v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3], 1.0f, 1e-3f);
        check(weightsOk, "skinned vertex weights sum to 1");
    }

    const auto obj = importModel(tests, "models/cube.obj", error);
    check(obj != nullptr, "cube.obj loads");
    const auto gltf = importModel(tests, "models/cube.gltf", error);
    check(gltf != nullptr, "cube.gltf loads");
    if (obj && gltf) {
        logModel("cube.obj", *obj);
        logModel("cube.gltf", *gltf);
        const kast::Material& red = obj->materials.front().material;
        check(obj->materials.size() == 1 && obj->materials[0].name == "Red" && near(red.baseColor[0], 0.8f) && near(red.baseColor[1], 0.1f) && near(red.baseColor[2], 0.1f),
              "cube.obj gets its MTL Kd color");
        check(near(obj->bounds.min, gltf->bounds.min) && near(obj->bounds.max, gltf->bounds.max) &&
                  near(obj->bounds.max, {0.5f, 0.5f, 0.5f}),
              "the same cube has the same bounds as OBJ and glTF (meters, Y-up)");
        check(near(gltf->materials[0].material.metallic, 0.25f) && near(gltf->materials[0].material.roughness, 0.75f),
              "cube.gltf material factors");
    }

    // Kenney's character (CC0): skinned, Blender FBX export; its clips ship as separate animation-only files.
    const auto fbx = importModel(tests, "models/kenney/characterMedium.fbx", error);
    check(fbx != nullptr, "characterMedium.fbx loads");
    if (fbx) {
        logModel("characterMedium.fbx", *fbx);
        check(std::ranges::any_of(fbx->meshes, [](const kast::ModelMesh& m) { return !m.joints.empty(); }),
              "characterMedium.fbx is skinned");
        // Authored 376 units tall in a centimeter file (UnitScaleFactor 1): 3.76 m.
        const bx::Vec3 size = extent(fbx->bounds);
        check(near(size.y, 3.765f, 0.01f) && size.y > size.x && size.y > size.z,
              "the FBX character stands upright (Y-up), centimeters converted to meters");
        for (const kast::Clip& clip : fbx->clips)
            check(!clip.name.empty() && clip.duration > 0.0f, "FBX clips are named and have a duration");
    }

    check(importModel(tests, "models/does_not_exist.glb", error) == nullptr && !error.empty(),
          "a missing model is an error");
    error.clear();
    check(importModel(tests, "models/corrupt.glb", error) == nullptr && !error.empty(), "a corrupt model is an error");
    kast::log::info(std::format("corrupt.glb: {}", error));
    error.clear();
    check(importModelFromMemory("garbage", "glb", error) == nullptr && !error.empty(), "garbage bytes are an error");

    const auto ok = importModelFromMemory(skinnedGlb(128), "glb", error);
    check(ok && !ok->overBoneLimit && ok->meshes.size() == 1 && ok->meshes[0].joints.size() == 128,
          "128 joints are within the limit");
    const auto big = importModelFromMemory(skinnedGlb(130), "glb", error);
    check(big && big->overBoneLimit, "a model with more than 128 joints is over the limit");

    // A skinned vertex without weights stays where the mesh node puts it (not collapsed to the origin).
    const auto unweighted = importModelFromMemory(skinnedGlb(3, true), "glb", error);
    bool unweightedKept = unweighted && unweighted->meshes.size() == 1;
    if (unweightedKept) {
        const kast::ModelMesh& mesh = unweighted->meshes[0];
        std::vector<kast::Mat4> globals, skin;
        kast::nodeGlobals(*unweighted, kast::bindPose(*unweighted), globals);
        kast::skinMatrices(mesh, globals, skin);
        int found = 0;
        for (const kast::ModelVertex& v : mesh.vertices) {
            if (!near(v.x, 2.0f) || !near(v.y, 0.0f))
                continue; // vertex 2, the unweighted one, is at (2, 0, 0)
            ++found;
            bx::Vec3 p{0.0f, 0.0f, 0.0f};
            for (size_t i = 0; i < kast::kMaxBoneWeights; ++i)
                p = bx::add(p, bx::mul(bx::mul(bx::Vec3{v.x, v.y, v.z}, skin[static_cast<size_t>(v.joints[i])].data()),
                                       v.weights[i]));
            unweightedKept = unweightedKept &&
                             near(p, bx::mul(bx::Vec3{v.x, v.y, v.z}, globals[static_cast<size_t>(mesh.node)].data()));
        }
        unweightedKept = unweightedKept && found == 1 && unweighted->bounds.max.x >= 2.0f - 1e-4f;
    }
    check(unweightedKept, "an unweighted skinned vertex keeps its mesh-node position (and stays in the bounds)");

    // Two nodes named "Joint1": the bone and the clip bind to the one in the skeleton, not the decoy that comes first.
    const auto decoy = importModelFromMemory(skinnedGlb(3, false, true), "glb", error);
    const auto isRealJoint1 = [&decoy](int node) {
        return node > 0 && decoy->nodes[static_cast<size_t>(node)].name == "Joint1" &&
               decoy->nodes[static_cast<size_t>(decoy->nodes[static_cast<size_t>(node)].parent)].name == "Joint0";
    };
    check(decoy && decoy->meshes.size() == 1 && decoy->meshes[0].joints.size() >= 2 &&
              isRealJoint1(decoy->meshes[0].joints[1]),
          "a bone binds to the skeleton's node when another node has its name");
    check(decoy && decoy->clips.size() == 1 && decoy->clips[0].channels.size() == 1 &&
              isRealJoint1(decoy->clips[0].channels[0].node),
          "a clip channel binds to the skeleton's node when another node has its name");
    // Assimp's PopulateArmatureData asserts (aborts in a debug Assimp) when every ancestor of a bone is a bone.
    const auto rootJoint = importModelFromMemory(skinnedGlb(3, false, false, true), "glb", error);
    check(rootJoint && rootJoint->meshes.size() == 1 && rootJoint->meshes[0].joints.size() == 3,
          "a skinned glTF whose one root node is a joint loads");

    check(kast::isModelFile("a/B.GLB") && kast::isModelFile("x.fbx") && !kast::isModelFile("x.mat"), "model extensions");
    check(kast::embeddedTextureModel("models/a.glb#2") == "models/a.glb" &&
              kast::embeddedTextureModel("tex#1/m.glb#0") == "tex#1/m.glb" &&
              kast::embeddedTextureModel("textures/a#b.png").empty() && kast::embeddedTextureModel("t.png").empty(),
          "embedded texture references split on the last '#', and only after a model file");
}

// One node, one clip: translation 0 -> 2 on X and rotation identity -> 90 degrees about Y over 2 seconds.
kast::Model testAnimatedModel()
{
    kast::Model model;
    model.nodes.push_back({"Root", -1, {}});
    kast::Clip clip{"Move", 2.0f, {}};
    kast::Channel channel;
    channel.node = 0;
    channel.translation = {{0.0f, {0.0f, 0.0f, 0.0f}}, {2.0f, {2.0f, 0.0f, 0.0f}}};
    const float s = std::sin(bx::kPi / 4.0f), c = std::cos(bx::kPi / 4.0f);
    channel.rotation = {{0.0f, {0.0f, 0.0f, 0.0f, 1.0f}}, {2.0f, {0.0f, s, 0.0f, c}}};
    clip.channels.push_back(channel);
    model.clips.push_back(clip);
    model.clips.push_back({"Still", 1.0f, {}});
    return model;
}

void checkAnimationSampling()
{
    const kast::Model model = testAnimatedModel();
    kast::Pose pose = kast::bindPose(model);
    kast::sampleClip(model.clips[0], 1.0f, pose);
    check(near(pose[0].translation, {1.0f, 0.0f, 0.0f}), "translation lerps at the mid key");
    const float s = std::sin(bx::kPi / 8.0f), c = std::cos(bx::kPi / 8.0f);
    check(sameRotation(pose[0].rotation, {0.0f, s, 0.0f, c}), "rotation slerps to 45 degrees at the mid key");
    kast::sampleClip(model.clips[0], 5.0f, pose);
    check(near(pose[0].translation, {2.0f, 0.0f, 0.0f}), "past the last key the last key holds");

    check(near(kast::clipTime(2.5f, 2.0f, true), 0.5f), "looping wraps the time");
    check(near(kast::clipTime(4.0f, 2.0f, true), 0.0f), "looping wraps exactly at the end");
    check(near(kast::clipTime(2.5f, 2.0f, false), 2.0f), "not looping clamps at the end");
    check(near(kast::clipTime(-1.0f, 2.0f, false), 0.0f), "not looping clamps at the start");
    check(sameRotation(kast::slerp({0, 0, 0, 1}, {0, 0, 0, -1}, 0.5f), {0, 0, 0, 1}),
          "slerp takes the short way between q and -q");

    check(near(kast::fadeWeight(0.0f, 0.5f), 0.0f) && near(kast::fadeWeight(0.25f, 0.5f), 0.5f) &&
              near(kast::fadeWeight(0.5f, 0.5f), 1.0f) && near(kast::fadeWeight(9.0f, 0.5f), 1.0f),
          "crossfade weight goes 0 to 1 over the fade time");
    check(near(kast::fadeWeight(0.0f, 0.0f), 1.0f), "no fade: the new clip at once");

    // Crossfade in AnimatorState: play Move to its end (x = 2, not looping), then fade to Still (bind: x = 0) over 1 s.
    kast::AnimatorState state;
    state.playing = true;
    kast::playClip(state, 0, 0.0f);
    kast::advanceAnimator(state, model, false, 1.0f, 2.0f);
    check(near(state.pose[0].translation, {2.0f, 0.0f, 0.0f}) && !state.playing,
          "a clip that isn't looping stops at its end");
    state.playing = true;
    kast::playClip(state, 1, 1.0f);
    kast::advanceAnimator(state, model, true, 1.0f, 0.25f);
    check(near(state.pose[0].translation, {1.5f, 0.0f, 0.0f}), "a quarter into the fade: 75% of the old pose");
    kast::advanceAnimator(state, model, true, 1.0f, 0.5f);
    check(near(state.pose[0].translation, {0.5f, 0.0f, 0.0f}), "three quarters into the fade");
    kast::advanceAnimator(state, model, true, 1.0f, 0.5f);
    check(near(state.pose[0].translation, {0.0f, 0.0f, 0.0f}), "after the fade: only the new clip");
    check(state.globals.size() == 1 && near(state.globals[0][12], 0.0f), "globals follow the pose");

    // Skinning: a joint moved by +1 on Y moves vertices bound to it by +1 (identity inverse bind).
    kast::Model skinned;
    skinned.nodes.push_back({"Joint", -1, {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}}});
    kast::ModelMesh mesh;
    mesh.joints = {0};
    kast::Mat4 identity;
    bx::mtxIdentity(identity.data());
    mesh.inverseBinds = {identity};
    std::vector<kast::Mat4> globals, bones;
    kast::nodeGlobals(skinned, kast::bindPose(skinned), globals);
    kast::skinMatrices(mesh, globals, bones);
    check(near(bx::mul(bx::Vec3{2.0f, 0.0f, 0.0f}, bones[0].data()), {2.0f, 1.0f, 0.0f}),
          "skin matrix = inverse bind, then joint");
}

// Every field of two models, exactly (floats bit for bit).
bool sameModel(const kast::Model& a, const kast::Model& b)
{
    const auto same = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    const auto sameTrs = [](const kast::Trs& x, const kast::Trs& y) { return std::memcmp(&x, &y, sizeof x) == 0; };
    bool ok = a.nodes.size() == b.nodes.size() && a.meshes.size() == b.meshes.size() && a.clips.size() == b.clips.size() &&
              a.materials.size() == b.materials.size() && a.textures == b.textures &&
              std::memcmp(&a.bounds, &b.bounds, sizeof a.bounds) == 0 && a.overBoneLimit == b.overBoneLimit;
    for (size_t i = 0; ok && i < a.nodes.size(); ++i)
        ok = a.nodes[i].name == b.nodes[i].name && a.nodes[i].parent == b.nodes[i].parent &&
             sameTrs(a.nodes[i].bind, b.nodes[i].bind);
    for (size_t i = 0; ok && i < a.meshes.size(); ++i) {
        const kast::ModelMesh &x = a.meshes[i], &y = b.meshes[i];
        ok = same(x.vertices, y.vertices) && same(x.indices, y.indices) && x.material == y.material && x.node == y.node &&
             same(x.joints, y.joints) && same(x.inverseBinds, y.inverseBinds);
    }
    for (size_t i = 0; ok && i < a.clips.size(); ++i) {
        const kast::Clip &x = a.clips[i], &y = b.clips[i];
        ok = x.name == y.name && x.duration == y.duration && x.channels.size() == y.channels.size();
        for (size_t c = 0; ok && c < x.channels.size(); ++c)
            ok = x.channels[c].node == y.channels[c].node && same(x.channels[c].translation, y.channels[c].translation) &&
                 same(x.channels[c].rotation, y.channels[c].rotation) && same(x.channels[c].scale, y.channels[c].scale);
    }
    for (size_t i = 0; ok && i < a.materials.size(); ++i)
        ok = a.materials[i].name == b.materials[i].name &&
             kast::materialToJson(a.materials[i].material) == kast::materialToJson(b.materials[i].material);
    return ok;
}

// A one-triangle .gltf whose buffer is the external tri.bin (positions, then uint16 indices).
constexpr const char* kTriangleGltf = R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Tri", "mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
  "materials": [{"name": "Mat", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}}],
  "buffers": [{"uri": "tri.bin", "byteLength": 42}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";

std::string triangleBin(float x1)
{
    const float positions[9] = {0.0f, 0.0f, 0.0f, x1, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const uint16_t indices[3] = {0, 1, 2};
    std::string bytes(reinterpret_cast<const char*>(positions), sizeof positions);
    return bytes.append(reinterpret_cast<const char*>(indices), sizeof indices);
}

size_t logCount(const std::string& part)
{
    return static_cast<size_t>(
        std::ranges::count_if(kast::log::lines(), [&](const std::string& line) { return contains(line, part); }));
}

// .kmodel: the binary model format (round trip, corrupt files), the editor's model cache (hits never call the importer;
// edits to the file or a sidecar and new importer versions miss), the player's cooked-only library and Build's cook.
void checkModelCache()
{
    const std::filesystem::path tests = KAST_TEST_PROJECT_DIR;
    const std::filesystem::path dir = looseTestDir("kast_model_cache");
    std::filesystem::create_directories(dir / "models");
    writeFile(dir / "models/tri.gltf", kTriangleGltf);
    writeFile(dir / "models/tri.bin", triangleBin(1.0f));
    for (const char* file : {"models/cube.obj", "models/cube.mtl"})
        std::filesystem::copy_file(tests / file, dir / file);
    std::string error;

    // Round trip: Assimp's model and the same model read back from .kmodel are equal.
    for (const auto& [root, path] : {std::pair{tests, "models/female-128-anim.glb"}, std::pair{dir, "models/cube.obj"},
                                     std::pair{dir, "models/tri.gltf"}}) {
        const auto imported = importModel(root, path, error);
        const auto read = imported ? kast::readModel(kast::writeModel(*imported), error) : nullptr;
        check(imported && read && sameModel(*imported, *read), ".kmodel round trip keeps every field of the model");
        if (!read)
            kast::log::error(std::format("  {}: {}", path, error));
    }
    const auto overLimit = importModelFromMemory(skinnedGlb(130), "glb", error);
    const auto overLimitRead = overLimit ? kast::readModel(kast::writeModel(*overLimit), error) : nullptr;
    check(overLimitRead && overLimitRead->overBoneLimit && sameModel(*overLimit, *overLimitRead),
          ".kmodel keeps overBoneLimit");

    // Corrupt .kmodel files are errors, never crashes.
    const auto cube = importModel(dir, "models/cube.obj", error);
    const std::string bytes = cube ? kast::writeModel(*cube) : "";
    check(!kast::readModel("garbage", error) && contains(error, "not a .kmodel"), "garbage is not a .kmodel");
    check(!kast::readModel(bytes.substr(0, bytes.size() / 2), error) && contains(error, "cut short"),
          "a truncated .kmodel is an error");
    check(!kast::readModel(bytes + "x", error) && contains(error, "past the end"), "trailing bytes are an error");
    std::string newer = bytes;
    newer[4] = static_cast<char>(kast::kModelFormatVersion + 1);
    check(!kast::readModel(newer, error) && contains(error, "version"), "another .kmodel version is an error");
    if (cube) {
        kast::Model broken = *cube;
        broken.meshes[0].indices[0] = 100000;
        check(!kast::readModel(kast::writeModel(broken), error) && contains(error, "bad indices"),
              "an out-of-range index is an error");
    }

    // Sidecars: the files an import reads besides the model file.
    check(kast::modelSidecars("models/cube.obj", readText(dir / "models/cube.obj")) == std::vector<std::string>{"models/cube.mtl"},
          "an .obj's sidecar is its mtllib");
    check(kast::modelSidecars("models/tri.gltf", kTriangleGltf) == std::vector<std::string>{"models/tri.bin"},
          "a .gltf's sidecar is its external buffer");
    check(kast::modelSidecars("a/m.gltf", R"({"buffers": [{"uri": "../bufs/x%20y.bin"}, {"uri": "data:,"}]})") ==
              std::vector<std::string>{"bufs/x y.bin"},
          "glTF buffer URIs are decoded and relative to the model; data: URIs are no sidecar");
    check(kast::modelSidecars("models/female-128-anim.glb", readText(tests / "models/female-128-anim.glb")).empty(),
          "a .glb with its buffer inside has no sidecar");
    check(kast::modelSidecars("m.obj", "v 0 0 0\n  mtllib  my mats.mtl \r\nmtllibx no.mtl\n") ==
              std::vector<std::string>{"my mats.mtl"},
          "an mtllib name is the rest of its line");

    // The key: everything the cooked model depends on.
    const std::string key = kast::modelCacheKey("m.obj", "obj", {std::string("mtl")}, "v1");
    check(key.size() == 16 && key == kast::modelCacheKey("m.obj", "obj", {std::string("mtl")}, "v1"), "the key is stable");
    for (const std::string& other : {kast::modelCacheKey("m.obj", "obj", {std::string("mtl")}, "v2"),
                                     kast::modelCacheKey("n.obj", "obj", {std::string("mtl")}, "v1"),
                                     kast::modelCacheKey("m.obj", "obj!", {std::string("mtl")}, "v1"),
                                     kast::modelCacheKey("m.obj", "obj", {std::string("mtl!")}, "v1"),
                                     kast::modelCacheKey("m.obj", "obj", {std::nullopt}, "v1"),
                                     kast::modelCacheKey("m.obj", "obj", {}, "v1")})
        check(other != key, "the key changes with the version, path, file bytes and sidecar bytes");

    // The editor's library through the cache: a second load (a restart) is a hit and doesn't call the importer.
    int imports = 0;
    const kast::ModelImporter counting = [&imports](const std::filesystem::path& root, const std::string& path,
                                                    std::string& reason) {
        ++imports;
        return importModel(root, path, reason);
    };
    const auto cacheFiles = [&] { return kast::listFiles(dir / ".kast/model_cache").size(); };
    kast::ModelLibrary first;
    first.setRoot(dir);
    first.setImporter(counting, "v1");
    const auto tri = first.get("models/tri.gltf");
    check(tri && imports == 1 && cacheFiles() == 1, "a cache miss imports and writes <root>/.kast/model_cache/<key>.kmodel");
    kast::ModelLibrary restarted;
    restarted.setRoot(dir);
    restarted.setImporter(counting, "v1");
    const auto hit = restarted.get("models/tri.gltf");
    check(hit && imports == 1 && tri && sameModel(*tri, *hit) && logCount("Loaded model 'models/tri.gltf' (cache hit)") >= 1,
          "a cache hit reads the .kmodel without calling the importer");
    restarted.reloadChanged();
    check(imports == 1, "nothing changed: nothing reloads");
    rewrite(dir / "models/tri.bin", triangleBin(0.5f));
    restarted.reloadChanged();
    const auto edited = restarted.get("models/tri.gltf");
    check(imports == 2 && edited && near(edited->bounds.max.x, 0.5f) && cacheFiles() == 2,
          "editing a sidecar (.bin) reloads the model and misses the cache");
    rewrite(dir / "models/tri.gltf", std::string(kTriangleGltf) + "\n");
    restarted.reloadChanged();
    check(imports == 3 && restarted.get("models/tri.gltf") != edited, "editing the model file reloads it and misses the cache");
    check(restarted.get("models/cube.obj") && imports == 4, "the .obj imports once");
    rewrite(dir / "models/cube.mtl", readText(dir / "models/cube.mtl") + "\n");
    restarted.reloadChanged();
    check(imports == 5, "editing an .obj's .mtl reloads it");
    kast::ModelLibrary newImporter;
    newImporter.setRoot(dir);
    newImporter.setImporter(counting, "v2");
    check(newImporter.get("models/tri.gltf") && imports == 6, "a new importer version misses the cache");
    kast::ModelLibrary uncached;
    uncached.setRoot(dir);
    uncached.setImporter(counting);
    uncached.get("models/tri.gltf");
    uncached.get("models/tri.gltf");
    check(imports == 7, "without a version there is no cache: one import per load");
    for (const std::filesystem::path& file : kast::listFiles(dir / ".kast/model_cache"))
        writeFile(file, "corrupt");
    kast::ModelLibrary corruptCache;
    corruptCache.setRoot(dir);
    corruptCache.setImporter(counting, "v1");
    check(corruptCache.get("models/tri.gltf") && imports == 8, "a corrupt cache file is imported again");
    check(corruptCache.get("models/missing.glb") == nullptr && imports == 8, "a missing model never reaches the importer");

    // The player: no importer, only cooked models (modelPakPath), a clear error once for one that isn't.
    const std::filesystem::path game = looseTestDir("kast_model_player");
    std::filesystem::create_directories(game / "models");
    if (cube)
        writeFile(game / kast::modelPakPath("models/cube.obj"), kast::writeModel(*cube));
    std::filesystem::copy_file(dir / "models/tri.gltf", game / "models/raw.gltf");
    kast::ModelLibrary player;
    player.setRoot(game);
    const auto cooked = player.get("models/cube.obj");
    check(cooked && cube && sameModel(*cooked, *cube) && logCount("Loaded model 'models/cube.obj' (cooked)") == 1,
          "the player loads a cooked model by its source path");
    for (int i = 0; i < 3; ++i) {
        check(player.get("models/raw.gltf") == nullptr, "the player never imports a raw model");
        player.reloadChanged();
    }
    check(logCount("Model 'models/raw.gltf': game.pak has no cooked model for it") == 1,
          "a model with no cooked entry is one clear error");

    // Build: every model cooked in place of the model files and their sidecars; a corrupt one fails naming it.
    const std::filesystem::path source = looseTestDir("kast_model_build");
    for (const char* file : {"models/tri.gltf", "models/tri.bin", "models/cube.obj", "models/cube.mtl"}) {
        std::filesystem::create_directories((source / file).parent_path());
        std::filesystem::copy_file(dir / file, source / file);
    }
    saveTestScene(source / "a.scene", "Thing", {});
    kast::Project project{.root = source, .name = "Models", .startScene = "a.scene"};
    const std::filesystem::path playerDir = std::filesystem::path(SDL_GetBasePath());
    std::filesystem::path exe;
    kast::Pak pak;
    check(buildGame(project, source.parent_path() / "kast_model_build_out", playerDir, exe, error) &&
              pak.open(source.parent_path() / "kast_model_build_out" / kGamePakName, error) &&
              pak.list("") == std::vector<std::string>{"a.scene", "models/cube.obj.kmodel", "models/tri.gltf.kmodel"} &&
              logCount("Cooked model 'models/tri.gltf' for game.pak") >= 1,
          "Build ships cooked models in place of the model files and their sidecars");
    const size_t hits = logCount("Cooked model 'models/tri.gltf' for game.pak (cache hit)");
    check(buildGame(project, source.parent_path() / "kast_model_build_out", playerDir, exe, error) &&
              logCount("Cooked model 'models/tri.gltf' for game.pak (cache hit)") == hits + 1,
          "a second Build cooks from the cache");
    std::filesystem::copy_file(tests / "models/corrupt.glb", source / "models/corrupt.glb");
    check(!buildGame(project, source.parent_path() / "kast_model_build_bad", playerDir, exe, error) &&
              contains(error, "Build failed: model 'models/corrupt.glb'") &&
              !std::filesystem::exists(source.parent_path() / "kast_model_build_bad"),
          "Build fails naming a model that can't be imported, writing nothing");
}

void checkAnimatorSystem()
{
    // Models come from the global library: point it at the test project.
    kast::models().setRoot(KAST_TEST_PROJECT_DIR);
    kast::Scene scene;
    const entt::entity e = scene.create("Hero");
    scene.registry().emplace<kast::ModelRenderer>(e, "models/female-128-anim.glb");
    scene.registry().emplace<kast::Animator>(e, "Angry", true, 1.0f, true);
    const entt::entity lone = scene.create("No Model");
    scene.registry().emplace<kast::Animator>(lone);
    kast::animatorSystem(scene, 0.1f);
    const auto character = kast::models().get("models/female-128-anim.glb");
    const auto* state = scene.registry().try_get<kast::AnimatorState>(e);
    check(character && state && state->playing && state->clip == kast::findClip(*character, "Angry"),
          "playOnStart starts the Animator's clip");
    check(character && state && state->globals.size() == character->nodes.size(), "the system poses every node");
    check(!scene.registry().all_of<kast::AnimatorState>(lone), "an Animator without a ModelRenderer does nothing");
    if (state) {
        const std::vector<kast::Mat4> before = state->globals;
        kast::animatorSystem(scene, 0.3f);
        check(state->globals != before, "the pose changes as time advances");
        scene.registry().get<kast::Animator>(e).clip = "No_Such_Clip";
        kast::animatorSystem(scene, 0.1f);
        check(state->clip == -1, "an unknown clip shows the bind pose");
    }
    check(scene.registry().get<kast::Transform>(e).position.x == 0.0f, "animation never moves the entity Transform");

    // Registration: scene JSON round trip of both components, with the materials list.
    kast::Scene saved;
    const entt::entity m = saved.create("Model");
    saved.registry().emplace<kast::ModelRenderer>(m, "models/a.glb", std::vector<std::string>{"", "materials/x.mat"});
    saved.registry().emplace<kast::Animator>(m, "Run", false, 2.0f, false);
    const auto modelType = kast::findAnyComponentType("ModelRenderer");
    const auto animatorType = kast::findAnyComponentType("Animator");
    const nlohmann::ordered_json json = kast::componentToJson(saved.registry(), m, modelType);
    check(json["materials"] == nlohmann::ordered_json::array({"", "materials/x.mat"}), "materials saves as a string array");
    std::string error;
    kast::Scene loaded;
    const entt::entity l = loaded.create("Model");
    check(kast::setComponentJson(loaded.registry(), l, modelType, json, error) &&
              loaded.registry().get<kast::ModelRenderer>(l).materials.size() == 2,
          "materials loads back");
    nlohmann::ordered_json badMaterials = json;
    badMaterials["materials"] = nlohmann::ordered_json::array({1});
    check(!kast::setComponentJson(loaded.registry(), l, modelType, badMaterials, error), "a non-string material is rejected");
    nlohmann::ordered_json badSpeed = kast::componentToJson(saved.registry(), m, animatorType);
    badSpeed["speed"] = -1;
    check(!kast::setComponentJson(loaded.registry(), l, animatorType, badSpeed, error), "a negative Animator speed is rejected");
    kast::models().setRoot({});
}

void checkModelEditor()
{
    kast::models().setRoot(KAST_TEST_PROJECT_DIR);
    Document doc;
    entt::registry& registry = doc.scene.registry();

    // Dropping a model from the Assets panel (viewport or Hierarchy): an entity named after the file.
    const entt::entity hero = createModelEntity(doc, "models/female-128-anim.glb", entt::null);
    check(registry.get<kast::Name>(hero).value == "female-128-anim" &&
              registry.get<kast::ModelRenderer>(hero).model == "models/female-128-anim.glb" && doc.selected == hero &&
              doc.dirty,
          "dropping a model creates a selected ModelRenderer entity named after the file");
    check(registry.all_of<kast::Animator>(hero) && registry.get<kast::Animator>(hero).clip == "Idle_A",
          "an animated model gets an Animator playing the default clip");
    const entt::entity cube = createModelEntity(doc, "models/cube.obj", hero);
    check(!registry.all_of<kast::Animator>(cube) && doc.scene.parent(cube) == hero,
          "a static model gets no Animator; a Hierarchy drop parents it");

    // Create > Model: an empty ModelRenderer to fill in the Inspector.
    const entt::entity empty = createObject(doc, ObjectKind::Model, entt::null);
    check(registry.all_of<kast::ModelRenderer>(empty) && registry.get<kast::ModelRenderer>(empty).model.empty() &&
              registry.get<kast::Name>(empty).value == "Model",
          "Create > Model adds an empty ModelRenderer");

    // Renaming / moving a model or a .mat remaps the open scene's ModelRenderers.
    registry.get<kast::ModelRenderer>(hero).materials = {"", "materials/gold_r05.mat"};
    doc.dirty = false;
    remapScenePaths(doc, "models/female-128-anim.glb", "models/heroine.glb");
    check(registry.get<kast::ModelRenderer>(hero).model == "models/heroine.glb" &&
              registry.get<kast::ModelRenderer>(cube).model == "models/cube.obj" && doc.dirty,
          "renaming a model remaps ModelRenderer.model");
    remapScenePaths(doc, "materials", "art/mats");
    check(registry.get<kast::ModelRenderer>(hero).materials ==
              std::vector<std::string>{"", "art/mats/gold_r05.mat"},
          "moving a .mat folder remaps ModelRenderer material overrides; embedded slots stay empty");
    remapScenePaths(doc, "models", "art/models");
    check(registry.get<kast::ModelRenderer>(cube).model == "art/models/cube.obj", "moving a model folder remaps it");

    // Edit-mode preview: Animators pose their model without dirtying the scene, and the pose is never saved.
    Document edit;
    const entt::entity animated = createModelEntity(edit, "models/female-128-anim.glb", entt::null);
    edit.dirty = false;
    updatePlay(edit, 0.25f);
    const auto* state = edit.scene.registry().try_get<kast::AnimatorState>(animated);
    check(state && state->playing && !state->globals.empty(), "Edit mode plays playOnStart clips");
    check(!edit.dirty, "the Edit-mode preview doesn't dirty the scene");
    const std::filesystem::path file = testRoot() / "kast_model_preview.scene";
    std::string error;
    check(kast::saveScene(edit.scene, file.string(), error), "a scene with a previewed Animator saves");
    std::string text;
    kast::readFile(file, text);
    check(text.find("AnimatorState") == std::string::npos && text.find("\"time\"") == std::string::npos,
          "preview state is not in the scene file");
    kast::Scene reloaded;
    check(kast::loadScene(file.string(), reloaded, error), "the saved scene loads");
    check(reloaded.registry().view<kast::AnimatorState>().empty() && !reloaded.registry().view<kast::Animator>().empty(),
          "after reload the Animator is back without any preview state");

    // Play starts clips from 0 (not where the Edit preview was); Stop drops the playback state.
    updatePlay(edit, 1.0f);
    startPlay(edit);
    kast::models().setRoot(KAST_TEST_PROJECT_DIR); // this Document has no project; the renderer sets it per frame
    updatePlay(edit, 0.1f);
    state = edit.scene.registry().try_get<kast::AnimatorState>(animated);
    check(state && std::abs(state->time - 0.1f) < 1e-4f, "Play starts the clip from 0");
    stopPlay(edit);
    check(edit.scene.registry().view<kast::AnimatorState>().empty(), "Stop drops the playback state");
    kast::models().setRoot({});
}

// --- physics ---

// A named entity with a Collider and, unless motion is nullopt (a static collider), a Rigidbody.
entt::entity physicsBody(kast::Scene& scene, const std::string& name, bx::Vec3 position, kast::Collider collider,
                         std::optional<kast::MotionType> motion = kast::MotionType::Dynamic,
                         entt::entity parent = entt::null)
{
    const entt::entity e = scene.create(name, parent);
    scene.registry().get<kast::Transform>(e).position = position;
    scene.registry().emplace<kast::Collider>(e, collider);
    if (motion)
        scene.registry().emplace<kast::Rigidbody>(e, kast::Rigidbody{.motionType = *motion});
    return e;
}

kast::Collider boxCollider(bx::Vec3 size)
{
    kast::Collider c;
    c.size = size;
    return c;
}

kast::Collider sphereCollider(float radius)
{
    kast::Collider c;
    c.shape = kast::ColliderShape::Sphere;
    c.radius = radius;
    return c;
}

// A 40 x 1 x 40 static floor whose top is at y = 0.
entt::entity physicsFloor(kast::Scene& scene)
{
    return physicsBody(scene, "Floor", {0.0f, -0.5f, 0.0f}, boxCollider({40.0f, 1.0f, 40.0f}), std::nullopt);
}

entt::entity physicsCharacter(kast::Scene& scene, bx::Vec3 feet)
{
    const entt::entity e = scene.create("Hero");
    scene.registry().get<kast::Transform>(e).position = feet;
    scene.registry().emplace<kast::CharacterController>(e);
    return e;
}

void stepPhysics(kast::Physics& physics, int steps)
{
    for (int i = 0; i < steps; ++i)
        physics.update(kast::kPhysicsStep);
}

size_t loggedCount(const std::string& part)
{
    return static_cast<size_t>(
        std::ranges::count_if(kast::log::lines(), [&](const std::string& line) { return contains(line, part); }));
}

void checkPhysicsComponents()
{
    const auto valid = [](const char* type, const char* json) {
        std::string error;
        return static_cast<bool>(
            kast::componentFromJson(kast::findComponentType(type), nlohmann::ordered_json::parse(json), error));
    };
    check(kast::findComponentType("Collider") && kast::findComponentType("Rigidbody") &&
              kast::findComponentType("CharacterController"),
          "physics: Collider, Rigidbody and CharacterController are registered (Add Component, Inspector)");
    const auto collider = [](const char* shape, const char* size, double radius, double height, double friction,
                             double restitution) {
        return std::format(R"({{"shape": "{}", "size": {}, "radius": {}, "height": {}, "center": [0, 1, 0],
                                "isTrigger": false, "friction": {}, "restitution": {}}})",
                           shape, size, radius, height, friction, restitution);
    };
    check(valid("Collider", collider("capsule", "[1, 2, 3]", 0.5, 2, 0.4, 0.2).c_str()), "physics: a valid Collider loads");
    check(!valid("Collider", collider("box", "[1, 0, 1]", 0.5, 2, 0.5, 0).c_str()), "physics: Collider size must be > 0");
    check(!valid("Collider", collider("sphere", "[1, 1, 1]", -1, 2, 0.5, 0).c_str()), "physics: Collider radius must be > 0");
    check(!valid("Collider", collider("capsule", "[1, 1, 1]", 1, 1, 0.5, 0).c_str()),
          "physics: Collider height must be >= 2 * radius");
    check(!valid("Collider", collider("box", "[1, 1, 1]", 0.5, 2, -1, 0).c_str()), "physics: Collider friction must be >= 0");
    check(!valid("Collider", collider("box", "[1, 1, 1]", 0.5, 2, 0.5, 1.5).c_str()),
          "physics: Collider restitution must be 0..1");
    check(!valid("Collider", collider("cone", "[1, 1, 1]", 0.5, 2, 0.5, 0).c_str()), "physics: unknown Collider shape rejected");
    check(valid("Rigidbody", R"({"motionType": "kinematic", "mass": 2, "gravityScale": 0, "linearDamping": 0, "angularDamping": 1})"),
          "physics: a valid Rigidbody loads");
    check(!valid("Rigidbody", R"({"motionType": "dynamic", "mass": 0, "gravityScale": 1, "linearDamping": 0, "angularDamping": 0})"),
          "physics: Rigidbody mass must be > 0");
    check(!valid("Rigidbody", R"({"motionType": "dynamic", "mass": 1, "gravityScale": 1, "linearDamping": -1, "angularDamping": 0})"),
          "physics: Rigidbody damping must be >= 0");
    check(valid("CharacterController", R"({"radius": 0.4, "height": 1.8, "maxSlopeDegrees": 45, "stepHeight": 0.3})"),
          "physics: a valid CharacterController loads");
    check(!valid("CharacterController", R"({"radius": 0.4, "height": 1.8, "maxSlopeDegrees": 95, "stepHeight": 0.3})"),
          "physics: CharacterController maxSlopeDegrees must be 0..90");
    check(!valid("CharacterController", R"({"radius": 1, "height": 1, "maxSlopeDegrees": 45, "stepHeight": 0.3})"),
          "physics: CharacterController height must be >= 2 * radius");

    // Round trip: components and gravity.
    kast::Scene scene;
    kast::Collider capsule = sphereCollider(0.3f);
    capsule.shape = kast::ColliderShape::Capsule;
    capsule.height = 1.5f;
    capsule.center = {0.0f, 0.1f, 0.0f};
    capsule.isTrigger = true;
    capsule.friction = 0.7f;
    capsule.restitution = 0.25f;
    const entt::entity e = physicsBody(scene, "Body", {1.0f, 2.0f, 3.0f}, capsule, kast::MotionType::Kinematic);
    scene.registry().get<kast::Rigidbody>(e).mass = 3.0f;
    scene.registry().emplace<kast::CharacterController>(scene.create("Hero"), kast::CharacterController{.stepHeight = 0.4f});
    scene.physics.gravity = {0.0f, -3.5f, 1.0f};
    const nlohmann::ordered_json json = kast::sceneToJson(scene);
    kast::Scene loaded;
    std::string error;
    check(kast::sceneFromJson(json, loaded, error), "physics: a scene with physics loads");
    check(kast::sceneToJson(loaded) == json, "physics: components and gravity round-trip through the scene file");
    check(loaded.physics.gravity.z == 1.0f && loaded.physics.gravity.y == -3.5f, "physics: gravity loads");

    nlohmann::ordered_json old = json;
    old.erase("physics");
    kast::Scene oldScene;
    check(kast::sceneFromJson(old, oldScene, error) && oldScene.physics.gravity.y == -9.81f &&
              oldScene.physics.gravity.x == 0.0f && oldScene.physics.gravity.z == 0.0f,
          "physics: a scene without \"physics\" loads with the default gravity 0, -9.81, 0");
    nlohmann::ordered_json bad = json;
    bad["physics"]["gravity"] = {0, "down", 0};
    check(!kast::sceneFromJson(bad, oldScene, error) && contains(error, "gravity"), "physics: bad gravity is rejected");
    bad["physics"] = {{"gravity", {0, -9.81, 0}}, {"drag", 1}};
    check(!kast::sceneFromJson(bad, oldScene, error) && contains(error, "unknown field"),
          "physics: an unknown physics field is rejected");
}

void checkPhysicsFall()
{
    // A dynamic box above a static floor falls and comes to rest on it (within Jolt's 2 cm penetration slop).
    kast::Scene scene;
    physicsFloor(scene);
    const entt::entity cube = physicsBody(scene, "Cube", {0.0f, 5.0f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    kast::EventBus bus;
    kast::Physics physics;
    physics.start(scene, bus);
    stepPhysics(physics, 30);
    const float falling = scene.registry().get<kast::Transform>(cube).position.y;
    check(falling < 5.0f && falling > 0.5f, "physics: the box falls");
    stepPhysics(physics, 300);
    const float rest = scene.registry().get<kast::Transform>(cube).position.y;
    check(near(rest, 0.5f, 0.025f), "physics: the box rests on the floor (y = 0.5)");
    if (!near(rest, 0.5f, 0.025f))
        kast::log::error(std::format("  rest y = {}", rest));

    // Under a moved, rotated parent (two levels): the result lands in the local Transform.
    kast::Scene parented;
    physicsFloor(parented);
    const entt::entity grand = parented.create("Grand");
    parented.registry().get<kast::Transform>(grand).position = {4.0f, 1.0f, 0.0f};
    const entt::entity parent = parented.create("Parent", grand);
    parented.registry().get<kast::Transform>(parent).position = {0.0f, 1.0f, 2.0f};
    parented.registry().get<kast::Transform>(parent).rotation = kast::quatFromEulerDeg({0.0f, 90.0f, 0.0f});
    const entt::entity child = physicsBody(parented, "Child", {1.0f, 3.0f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}),
                                           kast::MotionType::Dynamic, parent);
    const bx::Vec3 startWorld = worldPosition(parented, child);
    kast::Physics physics2;
    physics2.start(parented, bus);
    stepPhysics(physics2, 300);
    const bx::Vec3 endWorld = worldPosition(parented, child);
    const kast::Transform& local = parented.registry().get<kast::Transform>(child);
    check(near(endWorld, {startWorld.x, 0.5f, startWorld.z}, 0.025f),
          "physics: a child body falls straight down in world space and rests on the floor");
    // The parent's world position is (4, 2, 2): local y = 0.5 - 2; the yaw keeps y; x / z keep their local values.
    check(near(local.position, {1.0f, -1.5f, 0.0f}, 0.025f),
          "physics: the child's local Transform is relative to its rotated, moved parent");
    if (!near(local.position, {1.0f, -1.5f, 0.0f}, 0.025f))
        kast::log::error(std::format("  local = {} {} {}", local.position.x, local.position.y, local.position.z));

    // World scale: box per axis, sphere by the largest axis.
    kast::Scene scaled;
    physicsFloor(scaled);
    const entt::entity tall = physicsBody(scaled, "Tall", {0.0f, 4.0f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    scaled.registry().get<kast::Transform>(tall).scale = {1.0f, 3.0f, 1.0f};
    const entt::entity ball = physicsBody(scaled, "Ball", {5.0f, 4.0f, 0.0f}, sphereCollider(0.5f));
    scaled.registry().get<kast::Transform>(ball).scale = {1.0f, 2.0f, 1.0f};
    kast::Physics physics3;
    physics3.start(scaled, bus);
    stepPhysics(physics3, 300);
    const float tallY = scaled.registry().get<kast::Transform>(tall).position.y;
    const float ballY = scaled.registry().get<kast::Transform>(ball).position.y;
    check(near(tallY, 1.5f, 0.03f), "physics: a box scaled 3x in y rests at y = 1.5");
    check(near(ballY, 1.0f, 0.03f), "physics: a radius 0.5 sphere scaled (1, 2, 1) has radius 1 and rests at y = 1");
    check(scaled.registry().get<kast::Transform>(tall).scale.y == 3.0f, "physics: scale is untouched");
    if (!near(tallY, 1.5f, 0.03f) || !near(ballY, 1.0f, 0.03f))
        kast::log::error(std::format("  tall y = {}, ball y = {}", tallY, ballY));
}

void checkPhysicsPlayStop()
{
    Document doc;
    resetToDefault(doc);
    const entt::entity cube = doc.scene.children(entt::null)[2];
    doc.scene.registry().get<kast::Transform>(cube).position = {0.0f, 6.0f, 0.0f};
    doc.scene.registry().emplace<kast::Collider>(cube);
    doc.scene.registry().emplace<kast::Rigidbody>(cube);
    const kast::EntityId id = idOf(doc.scene, cube);
    const auto cubeY = [&] { // by id: Stop rebuilds the registry
        for (auto [entity, persistent, transform] : doc.scene.registry().view<kast::PersistentId, kast::Transform>().each())
            if (persistent.value == id)
                return transform.position.y;
        return -1000.0f;
    };
    updatePlay(doc, 1.0f);
    check(cubeY() == 6.0f, "physics: nothing simulates in edit mode");
    startPlay(doc);
    for (int i = 0; i < 30; ++i)
        updatePlay(doc, 1.0f / 60.0f);
    check(cubeY() < 6.0f, "physics: Play simulates (the cube falls)");
    togglePause(doc);
    const float paused = cubeY();
    updatePlay(doc, 1.0f / 60.0f);
    check(cubeY() == paused, "physics: Pause stops the simulation");
    togglePause(doc);
    stopPlay(doc);
    check(cubeY() == 6.0f, "physics: Stop restores the original transforms");
}

void checkPhysicsTimestep()
{
    const auto run = [](float frameDt, int frames) {
        kast::Scene scene;
        physicsFloor(scene);
        const entt::entity cube = physicsBody(scene, "Cube", {0.3f, 3.0f, 0.1f}, boxCollider({1.0f, 1.0f, 1.0f}));
        scene.registry().get<kast::Transform>(cube).rotation = kast::quatFromEulerDeg({20.0f, 30.0f, 10.0f});
        kast::EventBus bus;
        kast::Physics physics;
        physics.start(scene, bus);
        int steps = 0;
        for (int i = 0; i < frames; ++i)
            steps += physics.update(frameDt);
        return std::pair{steps, scene.registry().get<kast::Transform>(cube)};
    };
    const auto [stepsA, a] = run(1.0f / 30.0f, 45);
    const auto [stepsB, b] = run(1.0f / 60.0f, 90);
    check(stepsA == 90 && stepsB == 90, "physics: 1/30 s frames run 2 fixed steps each, 1/60 s frames 1");
    check(near(a.position, b.position, 1e-5f) && near(a.rotation.w, b.rotation.w, 1e-5f) && near(a.rotation.x, b.rotation.x, 1e-5f),
          "physics: 1 x (1/30) and 2 x (1/60) frame dt give the same result (fixed 60 Hz step)");

    kast::Scene scene;
    kast::EventBus bus;
    kast::Physics physics;
    check(physics.update(1.0f) == 0, "physics: update before start does nothing");
    physics.start(scene, bus);
    check(physics.update(0.25f) == kast::kMaxPhysicsSteps && physics.update(10.0f) == kast::kMaxPhysicsSteps,
          "physics: a huge frame dt is capped at kMaxPhysicsSteps steps");
    check(physics.update(0.001f) <= 1, "physics: the backlog past the cap is dropped");
    check(physics.update(0.0f) == 0 && physics.update(-1.0f) == 0, "physics: zero / negative dt takes no step");
}

void checkPhysicsEvents()
{
    kast::Scene scene;
    const entt::entity floor = physicsFloor(scene);
    const entt::entity cube = physicsBody(scene, "Cube", {0.0f, 8.0f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    kast::Collider triggerShape = boxCollider({3.0f, 2.0f, 3.0f});
    triggerShape.isTrigger = true;
    const entt::entity trigger = physicsBody(scene, "Trigger", {0.0f, 4.0f, 0.0f}, triggerShape, std::nullopt);
    kast::EventBus bus;
    std::vector<std::pair<std::string, nlohmann::json>> events;
    std::vector<kast::EventBus::Subscription> subscriptions;
    for (const char* name : {"collisionEnter", "collisionExit", "triggerEnter", "triggerExit"})
        subscriptions.push_back(bus.subscribe(name, [&events, name](const nlohmann::json& p) { events.emplace_back(name, p); }));
    const auto count = [&](const std::string& name) {
        return std::ranges::count_if(events, [&](const auto& e) { return e.first == name; });
    };
    std::map<entt::entity, kast::EntityId> ids; // kept: the cube is destroyed below
    const auto pairIs = [&](const nlohmann::json& p, entt::entity x, entt::entity y) {
        const kast::EntityId a = p["a"], b = p["b"];
        return (a == ids[x] && b == ids[y]) || (a == ids[y] && b == ids[x]);
    };
    for (entt::entity e : {floor, cube, trigger})
        ids[e] = idOf(scene, e);
    kast::Physics physics;
    physics.start(scene, bus);
    for (int i = 0; i < 300; ++i) {
        physics.update(kast::kPhysicsStep);
        bus.drain();
    }
    check(count("triggerEnter") == 1 && count("triggerExit") == 1, "physics: the box enters and leaves the trigger once");
    check(near(scene.registry().get<kast::Transform>(cube).position.y, 0.5f, 0.025f),
          "physics: triggers don't block (the box falls through to the floor)");
    check(count("collisionEnter") == 1 && count("collisionExit") == 0,
          "physics: one collisionEnter when the box lands, no exit while it rests (no stay events)");
    if (count("collisionEnter") != 1 || count("collisionExit") != 0)
        for (const auto& [name, payload] : events)
            kast::log::error(std::format("  {} {}", name, payload.dump()));
    const auto enter = std::ranges::find(events, std::string("collisionEnter"), &std::pair<std::string, nlohmann::json>::first);
    const auto triggered = std::ranges::find(events, std::string("triggerEnter"), &std::pair<std::string, nlohmann::json>::first);
    if (enter != events.end() && triggered != events.end()) {
        check(pairIs(triggered->second, cube, trigger), "physics: triggerEnter names the box and the trigger");
        const nlohmann::json& p = enter->second;
        check(pairIs(p, cube, floor), "physics: collisionEnter names the box and the floor");
        // normal points from a toward b: up if a is the floor.
        const float sign = p["a"] == ids[floor] ? 1.0f : -1.0f;
        check(near(p["normal"]["y"].get<float>(), sign, 0.01f) && near(p["point"]["y"].get<float>(), 0.0f, 0.05f) &&
                  std::abs(p["point"]["x"].get<float>()) <= 0.51f,
              "physics: collisionEnter has the contact point (on the floor top) and the normal from a toward b");
    }

    // A body going away ends its contacts.
    scene.destroy(cube);
    physics.update(kast::kPhysicsStep);
    bus.drain();
    check(count("collisionExit") == 1 && pairIs(std::ranges::find(events, std::string("collisionExit"),
                                                                  &std::pair<std::string, nlohmann::json>::first)->second,
                                                cube, floor),
          "physics: destroying a resting body sends collisionExit with its id");
    // Bounce: restitution, enter and exit.
    kast::Collider bouncy = sphereCollider(0.5f);
    bouncy.restitution = 0.9f;
    const entt::entity ball = physicsBody(scene, "Ball", {5.0f, 3.0f, 0.0f}, bouncy);
    ids[ball] = idOf(scene, ball);
    events.clear();
    for (int i = 0; i < 60; ++i) {
        physics.update(kast::kPhysicsStep);
        bus.drain();
    }
    check(count("collisionEnter") >= 1 && count("collisionExit") >= 1 && pairIs(events.front().second, ball, floor),
          "physics: a bouncing ball sends collisionEnter then collisionExit");

    // A body asleep on the floor keeps its contact (Jolt drops it on sleep), and ends it when teleported away.
    const entt::entity sleeper = physicsBody(scene, "Sleeper", {-6.0f, 0.5f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    ids[sleeper] = idOf(scene, sleeper);
    for (int i = 0; i < 300; ++i) {
        physics.update(kast::kPhysicsStep);
        bus.drain();
    }
    const auto sleeperEvents = [&](const std::string& name) {
        return std::ranges::count_if(events, [&](const auto& e) { return e.first == name && pairIs(e.second, sleeper, floor); });
    };
    check(sleeperEvents("collisionEnter") == 1 && sleeperEvents("collisionExit") == 0,
          "physics: falling asleep on the floor sends no collisionExit");
    scene.registry().get<kast::Transform>(sleeper).position = {-6.0f, 10.0f, 0.0f};
    for (int i = 0; i < 2; ++i) {
        physics.update(kast::kPhysicsStep);
        bus.drain();
    }
    check(sleeperEvents("collisionExit") == 1, "physics: teleporting a sleeping body off the floor sends collisionExit");
}

void checkPhysicsBodyControl()
{
    kast::Scene scene;
    scene.physics.gravity = {0.0f, 0.0f, 0.0f};
    const entt::entity ball = physicsBody(scene, "Ball", {0.0f, 0.0f, 0.0f}, sphereCollider(0.5f));
    kast::Rigidbody& rb = scene.registry().get<kast::Rigidbody>(ball);
    rb.mass = 2.0f;
    rb.linearDamping = 0.0f;
    rb.angularDamping = 0.0f;
    const entt::entity wall = physicsBody(scene, "Wall", {50.0f, 0.0f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}), std::nullopt);
    const entt::entity platform = physicsBody(scene, "Platform", {0.0f, -20.0f, 0.0f}, boxCollider({2.0f, 0.5f, 2.0f}),
                                              kast::MotionType::Kinematic);
    kast::EventBus bus;
    kast::Physics physics;
    physics.start(scene, bus);

    physics.setVelocity(ball, {1.0f, 0.0f, 0.0f});
    stepPhysics(physics, 60);
    check(near(scene.registry().get<kast::Transform>(ball).position.x, 1.0f, 0.01f) &&
              near(physics.velocity(ball), {1.0f, 0.0f, 0.0f}, 1e-4f),
          "physics: setVelocity / getVelocity: 1 m/s for 1 s moves 1 m");
    physics.addImpulse(ball, {0.0f, 2.0f, 0.0f}); // mass 2: +1 m/s
    check(near(physics.velocity(ball), {1.0f, 1.0f, 0.0f}, 1e-4f), "physics: addImpulse changes velocity by impulse / mass");
    physics.addForce(ball, {0.0f, 0.0f, 120.0f}); // over one step: dv = 120 / 2 / 60 = 1
    stepPhysics(physics, 1);
    check(near(physics.velocity(ball), {1.0f, 1.0f, 1.0f}, 1e-3f), "physics: addForce acts over the next step (F / m * dt)");
    stepPhysics(physics, 1);
    check(near(physics.velocity(ball).z, 1.0f, 1e-3f), "physics: the force is gone after the step");
    physics.setAngularVelocity(ball, {0.0f, 2.0f, 0.0f});
    stepPhysics(physics, 1);
    check(near(physics.angularVelocity(ball), {0.0f, 2.0f, 0.0f}, 1e-3f), "physics: setAngularVelocity / getAngularVelocity");
    const bx::Quaternion before = scene.registry().get<kast::Transform>(ball).rotation;
    stepPhysics(physics, 10);
    const bx::Quaternion after = scene.registry().get<kast::Transform>(ball).rotation;
    check(!sameRotation(after, before), "physics: angular velocity rotates it");
    if (sameRotation(after, before))
        kast::log::error(std::format("  rotation {} {} {} {}", after.x, after.y, after.z, after.w));

    // Kinematic: velocity moves it, forces are refused.
    physics.setVelocity(platform, {0.0f, 3.0f, 0.0f});
    stepPhysics(physics, 60);
    check(near(scene.registry().get<kast::Transform>(platform).position.y, -17.0f, 0.01f),
          "physics: a kinematic body moves by its velocity");

    const auto throws = [](auto&& f) {
        try {
            f();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    check(throws([&] { physics.addForce(platform, {1.0f, 0.0f, 0.0f}); }) &&
              throws([&] { physics.addImpulse(wall, {1.0f, 0.0f, 0.0f}); }) &&
              throws([&] { physics.velocity(wall); }) && throws([&] { physics.setAngularVelocity(wall, {0, 1, 0}); }) &&
              throws([&] { physics.moveCharacter(ball, {1.0f, 0.0f, 0.0f}); }) && throws([&] { physics.isGrounded(ball); }) &&
              throws([&] { physics.addForce(scene.create("Empty"), {1.0f, 0.0f, 0.0f}); }),
          "physics: body functions on the wrong kind of body (or none) throw");
}

void checkPhysicsRuntimeSync()
{
    kast::Scene scene;
    scene.physics.gravity = {0.0f, 0.0f, 0.0f};
    const entt::entity a = scene.create("A");
    scene.registry().get<kast::Transform>(a).position = {0.0f, 0.0f, 0.0f};
    kast::EventBus bus;
    kast::Physics physics;
    physics.start(scene, bus);
    const auto hitAt = [&](float x) { // what a ray down at x hits
        const auto hit = physics.raycast({x, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
        return hit ? hit->entity : entt::null;
    };
    check(hitAt(0.0f) == entt::null, "physics: no Collider, no body");
    scene.registry().emplace<kast::Collider>(a);
    stepPhysics(physics, 1);
    check(hitAt(0.0f) == a, "physics: an added Collider has a body after the next step");
    scene.registry().get<kast::Transform>(a).position = {7.0f, 0.0f, 0.0f};
    stepPhysics(physics, 1);
    check(hitAt(7.0f) == a && hitAt(0.0f) == entt::null, "physics: setting a static body's Transform teleports it");
    scene.registry().get<kast::Collider>(a).size = {5.0f, 1.0f, 1.0f};
    stepPhysics(physics, 1);
    check(hitAt(9.2f) == a, "physics: changed Collider values rebuild the body");
    scene.registry().remove<kast::Collider>(a);
    stepPhysics(physics, 1);
    check(hitAt(7.0f) == entt::null, "physics: removing the Collider removes the body");

    // Dynamic teleport keeps its velocity; kinematic moves over a step and stops.
    const entt::entity d = physicsBody(scene, "D", {0.0f, 0.0f, 20.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    stepPhysics(physics, 1);
    physics.setVelocity(d, {0.0f, 0.0f, 1.0f});
    scene.registry().get<kast::Transform>(d).position = {30.0f, 0.0f, 20.0f};
    stepPhysics(physics, 1);
    check(near(scene.registry().get<kast::Transform>(d).position, {30.0f, 0.0f, 20.0f + kast::kPhysicsStep}, 1e-3f),
          "physics: setting a dynamic body's Transform teleports it");
    const entt::entity k = physicsBody(scene, "K", {0.0f, 0.0f, -20.0f}, boxCollider({1.0f, 1.0f, 1.0f}),
                                       kast::MotionType::Kinematic);
    stepPhysics(physics, 1);
    scene.registry().get<kast::Transform>(k).position = {1.0f, 0.0f, -20.0f};
    stepPhysics(physics, 1);
    check(near(physics.velocity(k), {0.0f, 0.0f, 0.0f}, 1e-4f) &&
              near(scene.registry().get<kast::Transform>(k).position, {1.0f, 0.0f, -20.0f}, 1e-3f),
          "physics: a kinematic body follows its Transform (moved over one step, then still)");
    const auto hitK = physics.raycast({1.0f, 10.0f, -20.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    check(hitK && hitK->entity == k, "physics: the kinematic body is at its new Transform");
    scene.destroy(k);
    stepPhysics(physics, 1);
    check(!physics.raycast({1.0f, 10.0f, -20.0f}, {0.0f, -1.0f, 0.0f}, 100.0f), "physics: a destroyed entity's body goes");

    // Setup problems: reported once, nothing created.
    const entt::entity lonely = scene.create("Lonely");
    scene.registry().emplace<kast::Rigidbody>(lonely);
    const entt::entity both = scene.create("Both");
    scene.registry().get<kast::Transform>(both).position = {-30.0f, 0.0f, 0.0f};
    scene.registry().emplace<kast::Collider>(both);
    scene.registry().emplace<kast::CharacterController>(both);
    stepPhysics(physics, 5);
    check(loggedCount("Physics: entity 'Lonely'") == 1 && logged("has a Rigidbody but no Collider; it is ignored"),
          "physics: a Rigidbody without a Collider warns once, naming the entity");
    check(loggedCount("Physics: entity 'Both'") == 1 && logged("can't be combined with a Collider or Rigidbody"),
          "physics: CharacterController + Collider is reported once, naming the entity");
    check(kast::physicsConflict(scene.registry(), both) && !kast::physicsConflict(scene.registry(), lonely),
          "physics: physicsConflict (shown in the Inspector) flags CharacterController + Collider");
    check(!physics.raycast({-30.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f),
          "physics: the conflicting entity has no body");
}

void checkPhysicsCharacter()
{
    // Floor, a 0.25 step at x 3..5, a 0.6 block at x -3..-5... see each part.
    const auto world = [](kast::Scene& scene) { physicsFloor(scene); };
    kast::EventBus bus;

    {
        kast::Scene scene;
        world(scene);
        const entt::entity floor = scene.children(entt::null)[0];
        const entt::entity hero = physicsCharacter(scene, {0.0f, 0.05f, 0.0f});
        std::vector<nlohmann::json> enters;
        auto sub = bus.subscribe("collisionEnter", [&](const nlohmann::json& p) { enters.push_back(p); });
        kast::Physics physics;
        physics.start(scene, bus);
        stepPhysics(physics, 30);
        bus.drain();
        check(physics.isGrounded(hero), "physics: a character on the floor is grounded");
        const auto ground = physics.ground(hero);
        check(ground && ground->entity == floor && near(ground->normal, {0.0f, 1.0f, 0.0f}, 1e-3f),
              "physics: getGround returns the floor and its normal");
        check(near(scene.registry().get<kast::Transform>(hero).position.y, 0.0f, 0.05f),
              "physics: the character's feet (Transform position) are on the floor");
        const auto heroFloor = std::ranges::find_if(enters, [&](const nlohmann::json& p) {
            return (p["a"] == idOf(scene, hero) && p["b"] == idOf(scene, floor)) ||
                   (p["b"] == idOf(scene, hero) && p["a"] == idOf(scene, floor));
        });
        check(heroFloor != enters.end(), "physics: characters send collisionEnter like any body");
        if (heroFloor != enters.end()) {
            const float sign = (*heroFloor)["a"] == idOf(scene, floor) ? 1.0f : -1.0f;
            check(near((*heroFloor)["normal"]["y"].get<float>(), sign, 0.05f),
                  "physics: a character contact's normal points from a toward b");
        }
        physics.setVelocity(hero, {0.0f, 5.0f, 0.0f});
        stepPhysics(physics, 10);
        check(!physics.isGrounded(hero) && scene.registry().get<kast::Transform>(hero).position.y > 0.4f,
              "physics: setVelocity upward makes the character jump (mid-air: not grounded)");
        check(!physics.ground(hero), "physics: getGround is nil mid-air");
        stepPhysics(physics, 120);
        check(physics.isGrounded(hero) && near(scene.registry().get<kast::Transform>(hero).position.y, 0.0f, 0.05f),
              "physics: gravity brings the character back down");
        physics.moveCharacter(hero, {2.0f, 0.0f, 0.0f});
        stepPhysics(physics, 60);
        const bx::Vec3 p = scene.registry().get<kast::Transform>(hero).position;
        check(near(p.x, 2.0f, 0.1f) && near(p.y, 0.0f, 0.05f), "physics: moveCharacter walks it (2 m/s for 1 s)");
        check(near(physics.velocity(hero).x, 2.0f, 0.05f), "physics: getVelocity works on a character");
    }
    {
        // Steps: 0.25 (<= stepHeight 0.3) is climbed; a 0.6 block isn't.
        kast::Scene scene;
        world(scene);
        physicsBody(scene, "Step", {4.0f, 0.125f, 0.0f}, boxCollider({2.0f, 0.25f, 2.0f}), std::nullopt);
        physicsBody(scene, "Block", {-4.0f, 0.3f, 0.0f}, boxCollider({2.0f, 0.6f, 2.0f}), std::nullopt);
        const entt::entity up = physicsCharacter(scene, {0.0f, 0.0f, 0.0f});
        kast::Physics physics;
        physics.start(scene, bus);
        stepPhysics(physics, 10);
        physics.moveCharacter(up, {2.0f, 0.0f, 0.0f});
        stepPhysics(physics, 120);
        const bx::Vec3 p = scene.registry().get<kast::Transform>(up).position;
        check(p.x > 3.5f && near(p.y, 0.25f, 0.05f), "physics: the character climbs a step <= stepHeight");
        if (!(p.x > 3.5f && near(p.y, 0.25f, 0.05f)))
            kast::log::error(std::format("  step: {} {} {}", p.x, p.y, p.z));
        scene.registry().get<kast::Transform>(up).position = {0.0f, 0.0f, 0.0f}; // teleport back
        stepPhysics(physics, 2);
        physics.moveCharacter(up, {-2.0f, 0.0f, 0.0f});
        stepPhysics(physics, 120);
        const bx::Vec3 q = scene.registry().get<kast::Transform>(up).position;
        check(q.x > -2.7f && near(q.y, 0.0f, 0.05f), "physics: a static block taller than stepHeight blocks the character");
        if (!(q.x > -2.7f && near(q.y, 0.0f, 0.05f)))
            kast::log::error(std::format("  block: {} {} {}", q.x, q.y, q.z));
    }
    {
        // Slopes: a 30 degree ramp is walked up, a 60 degree one isn't (maxSlopeDegrees 45).
        kast::Scene scene;
        world(scene);
        const auto ramp = [&](float degrees, float z) {
            const entt::entity r = physicsBody(scene, "Ramp", {3.0f, 0.0f, z}, boxCollider({8.0f, 0.2f, 3.0f}), std::nullopt);
            scene.registry().get<kast::Transform>(r).rotation = kast::quatFromEulerDeg({0.0f, 0.0f, degrees});
        };
        ramp(30.0f, 0.0f);
        ramp(60.0f, 10.0f);
        const entt::entity gentle = physicsCharacter(scene, {0.0f, 0.0f, 0.0f});
        const entt::entity steep = physicsCharacter(scene, {0.0f, 0.0f, 10.0f});
        kast::Physics physics;
        physics.start(scene, bus);
        stepPhysics(physics, 10);
        physics.moveCharacter(gentle, {2.0f, 0.0f, 0.0f});
        physics.moveCharacter(steep, {2.0f, 0.0f, 0.0f});
        stepPhysics(physics, 150);
        const float gentleY = scene.registry().get<kast::Transform>(gentle).position.y;
        const float steepY = scene.registry().get<kast::Transform>(steep).position.y;
        check(gentleY > 0.7f, "physics: the character walks up a slope <= maxSlopeDegrees");
        check(steepY < 0.5f, "physics: the character can't walk up a slope > maxSlopeDegrees");
        if (!(gentleY > 0.7f && steepY < 0.5f))
            kast::log::error(std::format("  slopes: gentle y {} steep y {}", gentleY, steepY));
    }
    {
        // A wall (static) and a kinematic body block; a dynamic box is pushed.
        kast::Scene scene;
        world(scene);
        physicsBody(scene, "Wall", {0.0f, 1.5f, -4.0f}, boxCollider({10.0f, 3.0f, 2.0f}), std::nullopt);
        physicsBody(scene, "Gate", {0.0f, 1.5f, 4.0f}, boxCollider({10.0f, 3.0f, 2.0f}), kast::MotionType::Kinematic);
        const entt::entity crate = physicsBody(scene, "Crate", {-2.0f, 0.5f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
        const entt::entity hero = physicsCharacter(scene, {0.0f, 0.0f, 0.0f});
        kast::Physics physics;
        physics.start(scene, bus);
        stepPhysics(physics, 10);
        physics.moveCharacter(hero, {0.0f, 0.0f, -2.0f});
        stepPhysics(physics, 150);
        check(scene.registry().get<kast::Transform>(hero).position.z > -2.65f,
              "physics: a static wall blocks the character (feet stop at the wall face + radius)");
        physics.moveCharacter(hero, {0.0f, 0.0f, 2.0f});
        stepPhysics(physics, 240);
        check(scene.registry().get<kast::Transform>(hero).position.z < 2.65f, "physics: a kinematic body blocks the character");
        scene.registry().get<kast::Transform>(hero).position = {0.0f, 0.0f, 0.0f};
        physics.moveCharacter(hero, {-2.0f, 0.0f, 0.0f});
        stepPhysics(physics, 90);
        const float crateX = scene.registry().get<kast::Transform>(crate).position.x;
        check(crateX < -2.5f, "physics: the character pushes a dynamic box");
        if (!(crateX < -2.5f))
            kast::log::error(std::format("  crate x {}", crateX));
    }
}

void checkPhysicsLua()
{
    const std::filesystem::path dir = luaTestDir();
    kast::Scene scene;
    const entt::entity floor = physicsFloor(scene);
    const entt::entity crate = physicsBody(scene, "Crate", {0.0f, 0.5f, 0.0f}, boxCollider({1.0f, 1.0f, 1.0f}));
    const entt::entity hero = physicsCharacter(scene, {10.0f, 0.0f, 0.0f});
    const entt::entity ghost = scene.create("Ghost");
    scene.registry().get<kast::Transform>(ghost).position = {-10.0f, 0.5f, 0.0f};
    writeFile(dir / "p.lua", std::format(R"(local FLOOR, CRATE, HERO, GHOST = {}, {}, {}, {}
local frame = 0
local function fails(pattern, f, ...)
    local ok, err = pcall(f, ...)
    assert(not ok, "expected an error matching " .. pattern)
    assert(string.find(err, pattern, 1, true), "error was: " .. tostring(err))
end
return {{
    update = function(dt)
        frame = frame + 1
        if frame == 2 then
            local hit = kast.physics.raycast({{x = 5, y = 10, z = 0}}, {{x = 0, y = -2, z = 0}}, 100)
            assert(hit and hit.entity == FLOOR, "ray hits the floor")
            assert(math.abs(hit.point.y) < 1e-3 and math.abs(hit.point.x - 5) < 1e-3, "hit point")
            assert(math.abs(hit.normal.y - 1) < 1e-3 and math.abs(hit.distance - 10) < 1e-3, "hit normal / distance")
            hit = kast.physics.raycast({{0, 10, 0}}, {{0, -1, 0}}, 100)
            assert(hit and hit.entity == CRATE and math.abs(hit.point.y - 1) < 1e-3, "ray hits the crate first")
            assert(kast.physics.raycast({{0, 10, 0}}, {{0, 1, 0}}, 100) == nil, "a miss is nil")
            assert(kast.physics.raycast({{5, 10, 0}}, {{0, -1, 0}}, 5) == nil, "maxDistance limits the ray")
            local ids = kast.physics.overlapSphere({{x = 0, y = 0.5, z = 0}}, 0.2)
            assert(#ids == 1 and ids[1] == CRATE, "overlapSphere finds the crate only")
            ids = kast.physics.overlapSphere({{x = 10, y = 1, z = 0}}, 0.1)
            assert(#ids == 1 and ids[1] == HERO, "overlapSphere finds the character")
            ids = kast.physics.overlapBox({{x = 0, y = 0.5, z = 0}}, {{x = 1, y = 1, z = 1}})
            local found = {{}}
            for _, id in ipairs(ids) do found[id] = true end
            assert(#ids == 2 and found[CRATE] and found[FLOOR], "overlapBox finds the crate and the floor")
            ids = kast.physics.overlapBox({{x = 1.2, y = 0.5, z = 0}}, {{x = 1, y = 0.1, z = 0.1}})
            assert(#ids == 1 and ids[1] == CRATE, "a flat box beside the crate touches it only")
            local s = math.sin(math.rad(45))
            ids = kast.physics.overlapBox({{x = 1.2, y = 0.5, z = 0}}, {{x = 1, y = 0.1, z = 0.1}}, {{0, 0, s, s}})
            assert(#ids == 1 and ids[1] == FLOOR, "overlapBox takes a rotation: stood up 90 degrees it misses the crate")
            kast.physics.setVelocity(CRATE, {{x = 0, y = 0, z = 0}})
            local v = kast.physics.getVelocity(CRATE)
            assert(v.x == 0 and v.y == 0 and v.z == 0, "getVelocity returns a vector")
            kast.physics.addImpulse(CRATE, {{x = 0, y = 5, z = 0}})
            assert(math.abs(kast.physics.getVelocity(CRATE).y - 5) < 1e-3, "addImpulse")
            kast.physics.addForce(CRATE, {{x = 0, y = 0, z = 0}})
            kast.physics.setAngularVelocity(CRATE, {{x = 0, y = 1, z = 0}})
            assert(math.abs(kast.physics.getAngularVelocity(CRATE).y - 1) < 1e-3, "angular velocity")
            kast.physics.moveCharacter(HERO, {{x = 1, y = 0, z = 0}})
            assert(type(kast.physics.isGrounded(HERO)) == "boolean", "isGrounded")
            fails("has no dynamic Rigidbody", kast.physics.addForce, FLOOR, {{x = 0, y = 1, z = 0}})
            fails("has no dynamic Rigidbody", kast.physics.addImpulse, GHOST, {{x = 0, y = 1, z = 0}})
            fails("has no dynamic or kinematic Rigidbody or CharacterController", kast.physics.getVelocity, FLOOR)
            fails("has no CharacterController", kast.physics.moveCharacter, CRATE, {{x = 1, y = 0, z = 0}})
            fails("has no CharacterController", kast.physics.getGround, CRATE)
            fails("kast.physics.raycast: maxDistance must be > 0", kast.physics.raycast, {{0, 0, 0}}, {{0, 1, 0}}, 0)
            fails("kast.physics.raycast: origin must be a table", kast.physics.raycast, "up", {{0, 1, 0}}, 1)
            fails("kast.physics.overlapSphere: radius must be > 0", kast.physics.overlapSphere, {{0, 0, 0}}, -1)
            fails("kast.physics.overlapBox: halfExtents must be > 0", kast.physics.overlapBox, {{0, 0, 0}}, {{0, 1, 1}})
            fails("invalid entity id", kast.physics.getVelocity, 999999)
            -- Runtime sync through the component API: applied by the next step (next frame).
            kast.add(GHOST, "Collider")
            kast.log("physics lua frame 2 ok")
        elseif frame == 3 then
            local hit = kast.physics.raycast({{x = -10, y = 10, z = 0}}, {{0, -1, 0}}, 100)
            assert(hit and hit.entity == GHOST, "kast.add Collider: body by the next step")
            kast.set(GHOST, "Transform", {{position = {{x = -20, y = 0.5, z = 0}}}})
        elseif frame == 4 then
            assert(kast.physics.raycast({{x = -20, y = 10, z = 0}}, {{0, -1, 0}}, 100).entity == GHOST, "kast.set Transform teleports")
            kast.remove(GHOST, "Collider")
        elseif frame == 5 then
            assert(kast.physics.raycast({{x = -20, y = 10, z = 0}}, {{0, -1, 0}}, 100).entity == FLOOR, "kast.remove Collider")
            kast.destroy(CRATE)
            -- Until the next step its body is still in the physics world: queries leave the gone entity out.
            assert(kast.physics.raycast({{0, 10, 0}}, {{0, -1, 0}}, 100) == nil, "a destroyed entity's body is no hit")
            assert(#kast.physics.overlapSphere({{x = 0, y = 0.5, z = 0}}, 0.2) == 0, "nor an overlap")
        elseif frame == 6 then
            local hit = kast.physics.raycast({{0, 10, 0}}, {{0, -1, 0}}, 100)
            assert(hit.entity == FLOOR, "kast.destroy removes the body")
            kast.log("physics lua done")
        end
    end,
}}
)",
                                         idOf(scene, floor), idOf(scene, crate), idOf(scene, hero), idOf(scene, ghost)));
    scene.systems = {"p.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    check(lua.physics().running(), "physics: the Lua session starts physics");
    for (int i = 0; i < 6; ++i)
        kast::tickPlay(scene, lua, kast::kPhysicsStep);
    checkOk(lua, 0, "physics: kast.physics queries, body control, errors and runtime sync from Lua");
    check(logged("physics lua frame 2 ok") && logged("physics lua done"), "physics: the Lua checks ran");
    lua.stop();
    check(!lua.physics().running(), "physics: stopping the session stops physics");
}

// --- Terrain (docs/engine/terrain.html): heightmap decoding, the shared ground, Jolt parity, streaming ---

std::optional<kast::map::MapInfo> hillsMap()
{
    std::string text, err;
    if (!kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "maps/hills/map.json", text))
        return std::nullopt;
    return kast::map::parse_manifest(nlohmann::json::parse(text), err);
}

std::optional<kast::map::Heights> hillsHeights(int tx, int tz)
{
    std::string bytes, err;
    if (!kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / std::format("maps/hills/heights/{}_{}.png", tx, tz),
                        bytes))
        return std::nullopt;
    return kast::map::decode_heights(bytes, ".png", err);
}

void checkTerrainHeights()
{
    namespace m = kast::map;
    constexpr size_t n = m::kTileSamples;
    std::string err;
    std::string r16(n * n * 2, '\0');
    r16[0] = '\x34';
    r16[1] = '\x12';                   // sample (0, 0) = 0x1234, little-endian
    r16[(n * 1 + 2) * 2 + 1] = '\xff'; // sample u=2, v=1 = 0xff00
    const auto decoded = m::decode_heights(r16, ".r16", err);
    check(decoded && decoded->size() == n * n && (*decoded)[0] == 0x1234 && (*decoded)[n + 2] == 0xff00,
          "terrain: .r16 decodes 257 x 257 little-endian samples, row-major [v][u]");
    err.clear();
    check(!m::decode_heights(r16.substr(2), ".r16", err) && err.find("need 132098") != std::string::npos,
          "terrain: a wrong-size .r16 is refused, naming the size it needs");
    const auto png = hillsHeights(0, 0);
    check(png && png->size() == n * n && *std::ranges::max_element(*png) > *std::ranges::min_element(*png),
          "terrain: the hills' 16-bit grayscale PNG decodes (and has slopes)");
    std::string bytes;
    check(kast::readFile(std::filesystem::path(KAST_SOURCE_DIR) / "tests/heights_8bit.png", bytes) &&
              !m::decode_heights(bytes, ".png", err) && err.find("8-bit") != std::string::npos &&
              err.find("need 16-bit grayscale") != std::string::npos,
          "terrain: an 8-bit PNG (right size) is refused: need 16-bit grayscale");
    check(kast::readFile(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "textures/checker.png", bytes) &&
              !m::decode_heights(bytes, ".png", err) && err.find("need 257 x 257") != std::string::npos,
          "terrain: a PNG of the wrong size is refused");
    check(!m::decode_heights("junk", ".png", err) && err.starts_with("png:"), "terrain: a broken PNG is refused");
    check(!m::decode_heights(r16, ".raw", err) && err.find("need .r16 or .png") != std::string::npos,
          "terrain: an unknown heights format is refused");

    // ground(): the triangle under the point, split along (u, v)-(u+1, v+1) like Jolt; not bilinear.
    m::MapInfo info;
    info.heightRange = {{0.0f, 65535.0f}}; // sample s = s metres
    m::Heights h = m::flat_heights();
    h[n * 1 + 0] = 10; // (u 0, v 1)
    check(near(m::ground(h, info, 0.0f, 1.0f), 10.0f) && near(m::ground(h, info, 0.5f, 0.5f), 0.0f) &&
              near(m::ground(h, info, 0.25f, 0.75f), 5.0f) && near(m::ground(h, info, 0.75f, 0.25f), 0.0f),
          "terrain: ground uses the (u,v)-(u+1,v+1) diagonal: the far corner only lifts its own triangle");
    check(near(m::sample_height(65535, *hillsMap()), 80.0f) && near(m::sample_height(0, *hillsMap()), 0.0f),
          "terrain: samples map onto the manifest's heightRange");
    size_t triangles = 0;
    bool inRange = true, ccw = true;
    m::for_each_triangle([&](uint32_t a, uint32_t b, uint32_t c) {
        ++triangles;
        inRange = inRange && a < n * n && b < n * n && c < n * n;
        // Seen from +y (x = u, z = v): (b - a) x (c - a) has +y.
        const float ux = float(b % n) - float(a % n), uz = float(b / n) - float(a / n);
        const float vx = float(c % n) - float(a % n), vz = float(c / n) - float(a / n);
        ccw = ccw && uz * vx - ux * vz > 0.0f;
    });
    check(triangles == 256 * 256 * 2 && inRange && ccw,
          "terrain: for_each_triangle gives 2 counter-clockwise (from above) triangles per quad");

    // Seams: column 256 of tile tx is column 0 of tile tx+1, row 256 of tz is row 0 of tz+1.
    const auto t00 = hillsHeights(0, 0), t10 = hillsHeights(1, 0), t01 = hillsHeights(0, 1);
    bool seamX = t00 && t10, seamZ = t00 && t01;
    for (size_t i = 0; i < n && seamX && seamZ; ++i) {
        seamX = (*t00)[i * n + 256] == (*t10)[i * n + 0];
        seamZ = (*t00)[256 * n + i] == (*t01)[0 * n + i];
    }
    check(seamX && seamZ, "terrain: hills tiles share their edge samples (x and z seams)");
}

void checkTilesAround()
{
    namespace m = kast::map;
    m::MapInfo start;
    start.tiles = {{0, 0, 7, 7}};
    const auto keys = [](const std::vector<m::TileKey>& tiles) {
        std::string out;
        for (const m::TileKey t : tiles)
            out += std::format("{}_{} ", t.x, t.z);
        return out;
    };
    check(keys(kast::tiles_around(start, {1024.0f, 1024.0f}, 1)) == "3_3 4_3 5_3 3_4 4_4 5_4 3_5 4_5 5_5 ",
          "tiles_around: radius 1 is the 3 x 3 tiles around the centre's");
    check(keys(kast::tiles_around(start, {1300.0f, 1024.0f}, 1)) == "4_3 5_3 6_3 4_4 5_4 6_4 4_5 5_5 6_5 ",
          "tiles_around: crossing into tile 5 shifts the set");
    check(keys(kast::tiles_around(start, {10.0f, 2040.0f}, 1)) == "0_6 1_6 0_7 1_7 ",
          "tiles_around: clipped to the declared tiles at a corner");
    check(keys(kast::tiles_around(start, {10.0f, 10.0f}, 0)) == "0_0 ", "tiles_around: radius 0 is the centre tile");
    check(kast::tiles_around(start, {-600.0f, -600.0f}, 1).empty(), "tiles_around: none far off the map");
    m::MapInfo ell; // an L of two rects
    ell.tiles = {{0, 0, 1, 0}, {0, 1, 0, 1}};
    check(keys(kast::tiles_around(ell, {300.0f, 300.0f}, 1)) == "0_0 1_0 0_1 ",
          "tiles_around: only tiles some rect declares");
}

// A TerrainTile entity for tile (tx, tz) of the hills, as the streamer makes it.
entt::entity hillsTile(kast::Scene& scene, const std::shared_ptr<const kast::map::MapInfo>& info, int tx, int tz)
{
    auto data = std::make_shared<kast::TerrainTileData>();
    data->map = info;
    data->tile = {tx, tz};
    data->heights = *hillsHeights(tx, tz);
    const entt::entity e = scene.create(std::format("Tile {}_{}", tx, tz));
    scene.registry().emplace<kast::TerrainTile>(e, 3, tx, tz, std::shared_ptr<const kast::TerrainTileData>(data));
    return e;
}

void checkTerrainPhysics()
{
    const auto info = std::make_shared<const kast::map::MapInfo>(*hillsMap());
    kast::Scene scene;
    std::map<std::pair<int, int>, entt::entity> tiles;
    for (int tz = 0; tz < 2; ++tz)
        for (int tx = 0; tx < 2; ++tx)
            tiles[{tx, tz}] = hillsTile(scene, info, tx, tz);
    kast::EventBus bus;
    kast::Physics physics;
    physics.start(scene, bus);
    stepPhysics(physics, 1);

    // The shared ground() against a Jolt raycast down onto the height fields, at 1000 random points.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> coord(0.01f, 511.99f);
    float worst = 0.0f;
    int misses = 0, wrongTile = 0;
    for (int i = 0; i < 1000; ++i) {
        const float x = coord(rng), z = coord(rng);
        const int tx = int(x / 256.0f), tz = int(z / 256.0f);
        const auto hit = physics.raycast({x, 200.0f, z}, {0.0f, -1.0f, 0.0f}, 400.0f);
        if (!hit) {
            ++misses;
            continue;
        }
        if (hit->entity != tiles[{tx, tz}])
            ++wrongTile;
        const kast::TerrainTileData& data = *scene.registry().get<kast::TerrainTile>(tiles[{tx, tz}]).data;
        worst = std::max(worst, std::abs(hit->point.y - kast::map::ground(data.heights, *info, x - float(tx) * 256.0f,
                                                                          z - float(tz) * 256.0f)));
    }
    check(misses == 0 && wrongTile == 0, "terrain: a raycast down hits the height field of the tile under it");
    check(worst < 0.05f,
          std::format("terrain: ground() matches Jolt within 5 cm at 1000 points (worst {:.4f} m)", worst).c_str());
    kast::log::info(std::format("terrain: ground() vs Jolt height field: worst {:.4f} m over 1000 points", worst));

    // Smooth hills barely show the diagonal; random 0..2 m samples make the wrong triangle off by up to a metre.
    {
        auto rough = std::make_shared<kast::TerrainTileData>();
        rough->map = info;
        rough->tile = {5, 5}; // off the hills tiles
        std::uniform_int_distribution<int> sample(0, 1638); // 0..2 m in [0, 80]
        rough->heights = kast::map::flat_heights();
        for (uint16_t& s : rough->heights)
            s = static_cast<uint16_t>(sample(rng));
        kast::Scene roughScene;
        const entt::entity e = roughScene.create("Rough");
        roughScene.registry().emplace<kast::TerrainTile>(e, 3, 5, 5, std::shared_ptr<const kast::TerrainTileData>(rough));
        kast::Physics roughPhysics;
        roughPhysics.start(roughScene, bus);
        stepPhysics(roughPhysics, 1);
        float roughWorst = 0.0f;
        for (int i = 0; i < 1000; ++i) {
            const float u = coord(rng) / 2.0f, v = coord(rng) / 2.0f;
            const auto hit = roughPhysics.raycast({1280.0f + u, 50.0f, 1280.0f + v}, {0.0f, -1.0f, 0.0f}, 100.0f);
            roughWorst = std::max(roughWorst, hit ? std::abs(hit->point.y - kast::map::ground(rough->heights, *info, u, v))
                                                  : 1000.0f);
        }
        check(roughWorst < 0.05f,
              std::format("terrain: ground() matches Jolt on a rough tile too (same diagonal; worst {:.4f} m)",
                          roughWorst).c_str());
        kast::log::info(std::format("terrain: rough tile ground() vs Jolt: worst {:.4f} m over 1000 points", roughWorst));
    }

    // Streamed out: its body goes.
    scene.destroy(tiles[{1, 1}]);
    stepPhysics(physics, 1);
    check(!physics.raycast({400.0f, 200.0f, 400.0f}, {0.0f, -1.0f, 0.0f}, 400.0f) &&
              physics.raycast({100.0f, 200.0f, 100.0f}, {0.0f, -1.0f, 0.0f}, 400.0f),
          "terrain: a destroyed TerrainTile's height field is removed, the others stay");
}

void checkTerrainStreaming()
{
    kast::TerrainStreamer streamer;
    streamer.start(std::filesystem::path(KAST_TEST_PROJECT_DIR));
    check(streamer.map(1) && streamer.map(2) && streamer.map(3) && streamer.map(3)->name == "Hills" &&
              streamer.map(3)->areas.list.empty() && streamer.map(1)->areas.list.size() == 4,
          "terrain: the project's maps (and areas) load from maps/");
    kast::Scene scene;
    const entt::entity terrain = scene.create("Terrain");
    scene.registry().emplace<kast::Terrain>(terrain, 3, 1);
    const entt::entity player = scene.create("Player");
    scene.registry().emplace<kast::NetworkLocalPlayer>(player);
    scene.registry().get<kast::Transform>(player).position = {100.0f, 30.0f, 100.0f};
    kast::EventBus bus;
    std::vector<std::string> areaEvents;
    auto entered = bus.subscribe("area.entered", [&](const nlohmann::json& p) {
        areaEvents.push_back(std::format("enter {} {}", p["map"].get<int>(), p["id"].get<std::string>()));
    });
    auto left = bus.subscribe("area.left", [&](const nlohmann::json& p) {
        areaEvents.push_back(std::format("leave {} {}", p["map"].get<int>(), p["id"].get<std::string>()));
    });
    const auto tileKeys = [&] {
        std::vector<std::string> out;
        for (auto [e, tile] : scene.registry().view<kast::TerrainTile>().each())
            out.push_back(std::format("{}:{}_{}{}", tile.map, tile.tx, tile.tz,
                                      tile.data && scene.parent(e) == terrain ? "" : "!"));
        std::ranges::sort(out);
        return out;
    };
    streamer.update(scene, bus);
    check(tileKeys() == std::vector<std::string>{"3:0_0", "3:0_1", "3:1_0", "3:1_1"},
          "terrain: map 3, radius 1 around the player: the 4 declared tiles, children of Terrain, with data");
    const auto first = scene.registry().view<kast::TerrainTile>().front();
    streamer.update(scene, bus);
    check(scene.valid(first) && tileKeys().size() == 4, "terrain: a frame without change keeps the tile entities");

    scene.registry().get<kast::Terrain>(terrain).map = 1; // world.mapChanged, as Replication sets it
    scene.registry().get<kast::Transform>(player).position = {1024.0f, 0.0f, 1024.0f};
    streamer.update(scene, bus);
    bus.drain();
    check(tileKeys() == std::vector<std::string>{"1:3_3", "1:3_4", "1:3_5", "1:4_3", "1:4_4", "1:4_5", "1:5_3",
                                                 "1:5_4", "1:5_5"},
          "terrain: another map swaps every tile for the new map's 3 x 3");
    const auto flat = scene.registry().view<kast::TerrainTile>().front();
    const auto& flatData = *scene.registry().get<kast::TerrainTile>(flat).data;
    check(near(kast::map::ground(flatData.heights, *flatData.map, 10.0f, 10.0f), 0.0f),
          "terrain: a tile without a heights file is flat at heightMin");
    check(areaEvents == std::vector<std::string>{"enter 1 goldshire"},
          "terrain: area.entered for the centre's area (no area on the hills)");
    areaEvents.clear();
    scene.registry().get<kast::Transform>(player).position = {1065.0f, 0.0f, 1025.0f};
    streamer.update(scene, bus);
    bus.drain();
    check(areaEvents == std::vector<std::string>{"leave 1 goldshire", "enter 1 hills_gate"},
          "terrain: walking into another area: area.left, then area.entered");
    scene.registry().get<kast::Terrain>(terrain).radius = 0;
    streamer.update(scene, bus);
    check(tileKeys() == std::vector<std::string>{"1:4_4"}, "terrain: radius 0 keeps the centre tile only");
    scene.destroy(terrain);
    streamer.update(scene, bus);
    check(scene.registry().view<kast::TerrainTile>().empty(), "terrain: no Terrain, no tiles");
}

// tests/project/terrain.scene in editor Play: the tiles stream in, and the Hero (dropped ~6.5 m above the hills) is on
// the ground after 2 s.
void checkTerrainPlay()
{
    Document doc;
    std::string error;
    const std::filesystem::path root = KAST_TEST_PROJECT_DIR;
    check(openProject(doc, root / kast::kProjectFileName, error) && openFile(doc, (root / "terrain.scene").string(), error),
          ("terrain: opens terrain.scene: " + error).c_str());
    startPlay(doc);
    for (int i = 0; i < 120; ++i)
        updatePlay(doc, 1.0f / 60.0f);
    check(doc.scene.registry().view<kast::TerrainTile>().size() == 4,
          "terrain Play: the hills' 4 tiles stream in around the camera");
    const entt::entity hero = named(doc.scene, "Hero");
    const auto info = hillsMap();
    const bx::Vec3 p = hero != entt::null ? doc.scene.registry().get<kast::Transform>(hero).position : bx::Vec3{0, 0, 0};
    const kast::map::TileKey tile = kast::map::tile_of(p.x, p.z);
    const auto heights = hillsHeights(tile.x, tile.z);
    check(hero != entt::null && info && heights, "terrain Play: Hero over a hills tile");
    if (hero != entt::null && info && heights) {
        const float ground = kast::map::ground(*heights, *info, p.x - float(tile.x) * 256.0f, p.z - float(tile.z) * 256.0f);
        check(near(p.y, ground, 0.1f) && doc.lua.physics().isGrounded(hero),
              std::format("terrain Play: the dropped CharacterController is grounded at ground +- 0.1 after 2 s "
                          "(y {:.3f}, ground {:.3f})", p.y, ground).c_str());
        kast::log::info(std::format("terrain Play: hero y {:.3f}, ground {:.3f}", p.y, ground));
    }
    stopPlay(doc);
    check(doc.scene.registry().view<kast::TerrainTile>().empty(), "terrain Play: Stop leaves no tiles in the scene");
}

// Zone objects (docs/engine/terrain.html#objects) on the hills' fixtures (maps/hills/objects): each streams in with its
// tile as the tile's child, an object entity at its pos and yaw holding the prefab instance (scaled), and for an obstacle
// or interactable a static Collider from its shape standing on pos; Jolt collides with the shape. nearestInteractable
// measures to the shape's edge. A bad entry and a missing prefab are logged with the file and skipped, the rest kept.
void checkZoneObjects()
{
    kast::TerrainStreamer streamer;
    streamer.start(std::filesystem::path(KAST_TEST_PROJECT_DIR));
    kast::Scene scene;
    const entt::entity terrain = scene.create("Terrain");
    scene.registry().emplace<kast::Terrain>(terrain, 3, 1);
    kast::EventBus bus;
    streamer.update(scene, bus); // centre: the origin, tile 0_0; radius 1 takes all 4
    std::map<std::string, entt::entity> objects;
    for (auto [tileEntity, tile] : scene.registry().view<kast::TerrainTile>().each())
        for (const entt::entity child : scene.children(tileEntity))
            objects[std::format("{}_{} {}", tile.tx, tile.tz, scene.registry().get<kast::Name>(child).value)] = child;
    check(objects.size() == 4 && objects.contains("0_0 visual Crate") && objects.contains("1_0 obstacle Crate") &&
              objects.contains("0_1 interactable Crate") && objects.contains("0_1 obstacle Crate"),
          "zone objects: hills' 4 fixtures are their tiles' children");
    if (objects.size() != 4)
        return;
    const auto& reg = scene.registry();
    const entt::entity visual = objects["0_0 visual Crate"], wall = objects["1_0 obstacle Crate"];
    const entt::entity shrine = objects["0_1 interactable Crate"];
    const auto worldPos = [&](entt::entity e) {
        float m[16];
        scene.worldMatrix(e, m);
        return bx::Vec3{m[12], m[13], m[14]};
    };
    check(!reg.all_of<kast::Collider>(visual) && scene.children(visual).size() == 1 &&
              near(reg.get<kast::Transform>(scene.children(visual)[0]).scale, {0.4f, 0.4f, 0.4f}, 1e-5f) &&
              near(worldPos(visual).x, 248.0f) && near(worldPos(visual).z, 244.0f),
          "zone objects: a visual has no Collider, its prefab instance is scaled 0.4, at its map position");
    const kast::Collider* box = reg.try_get<kast::Collider>(wall);
    check(box && box->shape == kast::ColliderShape::Box && near(box->size, {2.0f, 2.0f, 2.0f}, 1e-5f) &&
              near(box->center, {0.0f, 1.0f, 0.0f}, 1e-5f) && !reg.all_of<kast::Rigidbody>(wall) &&
              near(worldPos(wall).x, 266.0f) && near(reg.get<kast::Transform>(wall).position.x, 10.0f),
          "zone objects: an obstacle's box shape is a static Collider standing on its pos (local to tile 1_0)");
    const kast::Collider* post = reg.try_get<kast::Collider>(shrine);
    check(post && post->shape == kast::ColliderShape::Cylinder && near(post->radius, 0.6f) && near(post->height, 1.0f) &&
              near(post->center, {0.0f, 0.5f, 0.0f}, 1e-5f),
          "zone objects: an interactable's cylinder shape is a Collider too");

    kast::Physics physics;
    physics.start(scene, bus);
    stepPhysics(physics, 1);
    const float base = worldPos(shrine).y;
    const auto hit = physics.raycast({250.0f, base + 20.0f, 266.0f}, {0.0f, -1.0f, 0.0f}, 40.0f);
    check(hit && hit->entity == shrine && near(hit->point.y, base + 1.0f, 0.02f),
          "zone objects: Jolt hits the cylinder's top, 1 m above its pos (the shape, not the 1 m crate mesh)");
    const auto through = physics.raycast({248.0f, worldPos(visual).y + 20.0f, 244.0f}, {0.0f, -1.0f, 0.0f}, 40.0f);
    check(through && reg.all_of<kast::TerrainTile>(through->entity), "zone objects: a raycast passes through the visual");

    const auto nearest = kast::nearestInteractable(scene, 250.0f, 268.5f, 3.0f);
    check(nearest && nearest->object->id == 1 && nearest->object->name == "Test Shrine" && near(nearest->distance, 1.9f, 1e-3f),
          "zone objects: nearestInteractable finds Test Shrine 1.9 m from its edge");
    check(!kast::nearestInteractable(scene, 250.0f, 280.0f, 3.0f), "zone objects: none beyond maxDistance");

    scene.registry().get<kast::Terrain>(terrain).radius = 0; // the centre tile 0_0 only: 0_1 and 1_0 stream out
    streamer.update(scene, bus);
    check(!scene.valid(wall) && !scene.valid(shrine) && scene.valid(visual) && !kast::nearestInteractable(scene, 250.0f, 268.5f, 3.0f),
          "zone objects: they go with their tile");

    // A copy of the hills with a bad entry and a missing prefab (twice): logged, skipped, the rest kept.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "kast_zone_objects_check";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "maps");
    std::filesystem::copy(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "maps" / "hills", root / "maps" / "hills",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "prefabs", root / "prefabs");
    writeFile(root / "maps" / "hills" / "objects" / "0_0.json", R"({"version": 1, "objects": [
        {"type": "visual", "prefab": "prefabs/Crate.prefab", "pos": [10, 0, 10]},
        {"type": "tree", "prefab": "prefabs/Crate.prefab", "pos": [20, 0, 20]},
        {"type": "visual", "prefab": "prefabs/NopeZoneObject.prefab", "pos": [30, 0, 30]},
        {"type": "visual", "prefab": "prefabs/NopeZoneObject.prefab", "pos": [40, 0, 40]},
        {"type": "obstacle", "prefab": "prefabs/Crate.prefab", "pos": [50, 0, 50], "shape": {"cylinder": {"radius": 1, "height": 1}}}]})");
    kast::TerrainStreamer broken;
    broken.start(root);
    kast::Scene brokenScene;
    brokenScene.registry().emplace<kast::Terrain>(brokenScene.create("Terrain"), 3, 0);
    broken.update(brokenScene, bus);
    const entt::entity tile = brokenScene.registry().view<kast::TerrainTile>().front();
    const auto file = (root / "maps" / "hills" / "objects" / "0_0.json").string();
    const auto count = std::ranges::count_if(kast::log::lines(), [](const std::string& l) { return contains(l, "NopeZoneObject.prefab"); });
    check(brokenScene.children(tile).size() == 2 &&
              logged(std::format("Terrain: {}: objects[1]: \"type\" must be \"visual\", \"obstacle\" or \"interactable\"; "
                                 "that object is skipped", file)) &&
              logged(std::format("Terrain: {}: prefab \"prefabs/NopeZoneObject.prefab\":", file)) && count == 1,
          std::format("zone objects: a bad entry and a missing prefab (logged once) are skipped, the other 2 kept ({})",
                      brokenScene.children(tile).size()).c_str());
    std::filesystem::remove_all(root);
}

// tests/project/zone_objects.scene in editor Play (scripts/zone_objects.lua): the Hero walking +x stops at the
// obstacle crate's side (x 265 - its 0.4 m radius); the Walker walks through the visual crate; the Lua API answers.
void checkZoneObjectsPlay()
{
    Document doc;
    std::string error;
    const std::filesystem::path root = KAST_TEST_PROJECT_DIR;
    check(openProject(doc, root / kast::kProjectFileName, error) && openFile(doc, (root / "zone_objects.scene").string(), error),
          ("zone objects Play: opens zone_objects.scene: " + error).c_str());
    startPlay(doc);
    for (int i = 0; i < 330; ++i)
        updatePlay(doc, 1.0f / 60.0f);
    const entt::entity hero = named(doc.scene, "Hero"), walker = named(doc.scene, "Walker");
    const float heroX = hero != entt::null ? doc.scene.registry().get<kast::Transform>(hero).position.x : 0.0f;
    const float walkerX = walker != entt::null ? doc.scene.registry().get<kast::Transform>(walker).position.x : 0.0f;
    check(heroX > 263.5f && heroX < 264.7f,
          std::format("zone objects Play: the Hero is stopped by the obstacle crate (x {:.2f}, its side at 264.6)", heroX).c_str());
    check(walkerX > 249.5f, std::format("zone objects Play: the Walker walked through the visual crate at 248 (x {:.2f})", walkerX).c_str());
    check(logged("zone objects: nearest 1 Test Shrine 1.90 m at 250, 266") && logged("none within 3 m of (250, 280): true") &&
              logged("zone objects: interact offline false") &&
              logged("interact(0) false") && logged("kast.interact: id must be a whole number from 1 to 4294967295, got 0"),
          "zone objects Play: kast.nearestInteractable and kast.interact from Lua");
    stopPlay(doc);
}

void checkColliderWireframe()
{
    kast::Scene scene;
    const entt::entity e = physicsBody(scene, "Box", {1.0f, 2.0f, 3.0f}, boxCollider({1.0f, 1.0f, 1.0f}), std::nullopt);
    kast::Transform& t = scene.registry().get<kast::Transform>(e);
    t.rotation = kast::quatFromEulerDeg({0.0f, 90.0f, 0.0f});
    t.scale = {2.0f, 1.0f, 1.0f};
    scene.registry().get<kast::Collider>(e).center = {0.5f, 0.0f, 0.0f};
    const auto bounds = [](const std::vector<std::pair<bx::Vec3, bx::Vec3>>& lines) {
        kast::Aabb box{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
        for (const auto& [a, b] : lines)
            for (const bx::Vec3& p : {a, b}) {
                box.min = bx::min(box.min, p);
                box.max = bx::max(box.max, p);
            }
        return box;
    };
    const auto lines = kast::colliderWireframe(scene, e);
    const kast::Aabb b = bounds(lines);
    // Local center (0.5, 0, 0) * scale (2, 1, 1) = (1, 0, 0), yawed 90 degrees: (0, 0, -1). Half extents (1, 0.5, 0.5)
    // yawed: (0.5, 0.5, 1) around world (1, 2, 2).
    check(lines.size() == 12 && near(b.min, {0.5f, 1.5f, 1.0f}, 1e-4f) && near(b.max, {1.5f, 2.5f, 3.0f}, 1e-4f),
          "physics: the box collider wireframe has the world position, rotation, scale and center offset");
    kast::Collider cylinder;
    cylinder.shape = kast::ColliderShape::Cylinder;
    cylinder.radius = 0.5f;
    cylinder.height = 0.4f; // shorter than its diameter: fine for a cylinder
    scene.registry().replace<kast::Collider>(e, cylinder); // max scale 2: radius 1, height 0.8
    const kast::Aabb cyl = bounds(kast::colliderWireframe(scene, e));
    const auto validate = kast::componentInfo(kast::findComponentType("Collider")).validate;
    kast::Collider capsule = cylinder;
    capsule.shape = kast::ColliderShape::Capsule;
    check(near(cyl.min, {0.0f, 1.6f, 2.0f}, 1e-3f) && near(cyl.max, {2.0f, 2.4f, 4.0f}, 1e-3f) && validate &&
              !validate(entt::meta_any(cylinder)) && validate(entt::meta_any(capsule)),
          "physics: a cylinder collider's wireframe is its two circles round the center; height < 2 * radius is valid for it "
          "(not for a capsule)");
    scene.registry().remove<kast::Collider>(e);
    check(kast::colliderWireframe(scene, e).empty(), "physics: no collider, no wireframe");
    scene.registry().emplace<kast::CharacterController>(e);
    const kast::Aabb c = bounds(kast::colliderWireframe(scene, e));
    // Capsule radius 0.4 * 2 (max scale), height 1.8 * 2, feet at the entity, upright.
    check(near(c.min, {1.0f - 0.8f, 2.0f, 3.0f - 0.8f}, 1e-3f) && near(c.max, {1.0f + 0.8f, 2.0f + 3.6f, 3.0f + 0.8f}, 1e-3f),
          "physics: the character capsule wireframe stands on the entity's position");
}

// kast_self_check --bench-physics: average fixed step time with 500 dynamic boxes falling onto a floor and settling.
int benchPhysics()
{
    kast::Scene scene;
    physicsFloor(scene);
    for (int i = 0; i < 500; ++i)
        physicsBody(scene, std::format("Box {}", i),
                    {static_cast<float>(i % 10) * 1.5f - 7.0f, 1.0f + static_cast<float>(i / 100) * 1.5f,
                     static_cast<float>((i / 10) % 10) * 1.5f - 7.0f},
                    boxCollider({1.0f, 1.0f, 1.0f}));
    kast::EventBus bus;
    kast::Physics physics;
    physics.start(scene, bus);
    // The first second: every box awake (falling, landing); the rest: settling and asleep.
    const auto run = [&](int steps) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < steps; ++i) {
            physics.update(kast::kPhysicsStep);
            bus.drain();
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / steps;
    };
    const double awake = run(60);
    const double settled = run(540);
    kast::log::info(std::format("Physics: 500 dynamic boxes, {:.3f} ms average per step in the first second (all "
                                "awake), {:.3f} ms over the next 9 s (settling, asleep)",
                                awake, settled));
    return 0;
}

// --- Audio ---
// Offline Audio (no device, the mix pulled with render()) unless a check says otherwise; main() makes Offline the
// default, so LuaSystems and Document sessions are offline too.

constexpr uint32_t kToneRate = 48000; // the offline engine's rate: test clips play unresampled

// Writes a 16-bit PCM WAV of frames frames; sample(i, channel) in -1..1.
template <typename Sample>
void writeWav(const std::filesystem::path& path, uint32_t frames, uint16_t channels, uint32_t rate, Sample sample)
{
    std::string data;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c) {
            const auto value = static_cast<int16_t>(std::lround(std::clamp(sample(i, c), -1.0, 1.0) * 32767.0));
            data.append(reinterpret_cast<const char*>(&value), 2);
        }
    std::string out = "RIFF";
    const auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
    u32(static_cast<uint32_t>(36 + data.size()));
    out += "WAVEfmt ";
    u32(16);
    u16(1); // PCM
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    out += "data";
    u32(static_cast<uint32_t>(data.size()));
    writeFile(path, out + data);
}

// A sine of amplitude 0.5 (stereo, kToneRate), frames long.
void writeTone(const std::filesystem::path& path, uint32_t frames)
{
    writeWav(path, frames, 2, kToneRate, [](uint32_t i, uint16_t) {
        return 0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kToneRate);
    });
}

// A project with sounds/tone.wav (12000 frames = 0.25 s), sounds/ramp.wav (sample i = i / 65536, 48000 frames),
// sounds/broken.wav and sounds/broken.mp3 (not audio) and sounds/tone.ogg.
std::filesystem::path audioTestDir()
{
    const std::filesystem::path dir = projectTestDir("kast_audio_check", "Audio");
    std::filesystem::create_directories(dir / "sounds");
    writeTone(dir / "sounds/tone.wav", 12000);
    writeWav(dir / "sounds/ramp.wav", 48000, 2, kToneRate, [](uint32_t i, uint16_t) { return i / 65536.0; });
    writeFile(dir / "sounds/broken.wav", "this is not a wave file");
    writeFile(dir / "sounds/broken.mp3", "this is not an mp3 file either");
    writeFile(dir / "sounds/tone.ogg", "OggS");
    return dir;
}

// Peak absolute sample of the next frames of the mix.
float renderPeak(kast::Audio& audio, uint32_t frames)
{
    std::vector<float> out(static_cast<size_t>(frames) * audio.channels());
    audio.render(out.data(), frames);
    float peak = 0.0f;
    for (float s : out)
        peak = std::max(peak, std::abs(s));
    return peak;
}

// miniaudio lags a change by up to a node cache (480 frames) plus a short stop fade: render past that before measuring.
constexpr uint32_t kSettleFrames = 1024;
void settle(kast::Audio& audio) { renderPeak(audio, kSettleFrames); }

// The first sample of the next frame of the mix.
float renderOne(kast::Audio& audio)
{
    std::vector<float> out(audio.channels());
    audio.render(out.data(), 1);
    return out[0];
}

bool throwsInvalid(const auto& body, const std::string& part)
{
    try {
        body();
    } catch (const std::invalid_argument& e) {
        if (contains(e.what(), part))
            return true;
        kast::log::error(std::format("  threw '{}', expected '{}'", e.what(), part));
        return false;
    }
    return false;
}

void checkAudioDecode()
{
    const std::filesystem::path project = KAST_TEST_PROJECT_DIR;
    std::string error;
    // Fixture formats read independently (Python's wave module; the MP3's frame headers: 76 MPEG-1 Layer III frames
    // x 1152, no Xing/LAME tag).
    const auto expect = [&](const char* path, uint64_t frames, uint32_t channels, uint32_t rate) {
        const std::optional<kast::ClipInfo> info = kast::clipInfo(project, path, error);
        check(info && info->frames == frames && info->channels == channels && info->sampleRate == rate,
              std::format("decode {}: frames, channels and sample rate", path).c_str());
        if (info)
            kast::log::info(std::format("{}: {} frames, {} ch, {} Hz", path, info->frames, info->channels, info->sampleRate));
        else
            kast::log::error(std::format("  {}", error));
    };
    expect("sounds/tone.wav", 22050, 1, 44100);
    expect("sounds/wave-demo.wav", 84808, 2, 44100);
    expect("sounds/mp3-demo.mp3", 87552, 2, 44100);
    expect("sounds/MP3-DEMO.MP3", 87552, 2, 44100); // extension case is ignored (and Windows paths are too)

    const std::filesystem::path dir = audioTestDir();
    writeWav(dir / "sounds/mono.wav", 1234, 1, 22050, [](uint32_t, uint16_t) { return 0.25; });
    const std::optional<kast::ClipInfo> mono = kast::clipInfo(dir, "sounds/mono.wav", error);
    check(mono && mono->frames == 1234 && mono->channels == 1 && mono->sampleRate == 22050,
          "decode a generated WAV: frames, channels and sample rate");
    check(!kast::clipInfo(dir, "sounds/broken.wav", error) && contains(error, "cannot decode"), "a corrupt WAV is an error");
    check(!kast::clipInfo(dir, "sounds/broken.mp3", error) && contains(error, "cannot decode"), "a corrupt MP3 is an error");
    check(!kast::clipInfo(dir, "sounds/tone.ogg", error) && contains(error, "not a .wav or .mp3"), ".ogg is rejected");
    check(!kast::clipInfo(dir, "sounds/none.wav", error) && contains(error, "cannot read"), "a missing clip is an error");
    check(!kast::clipInfo(dir, "../outside.wav", error) && !error.empty(), "a path outside the project is an error");
    check(kast::isAudioFile("a/B.WAV") && kast::isAudioFile("x.Mp3") && !kast::isAudioFile("x.ogg") &&
              !kast::isAudioFile("wav"),
          "isAudioFile: .wav / .mp3, any case");
}

void checkAudioMix()
{
    const std::filesystem::path dir = audioTestDir();
    kast::Audio audio(kast::Audio::Output::Offline);
    audio.start(dir);
    check(audio.channels() == 2 && audio.sampleRate() == kToneRate, "offline audio: stereo at 48 kHz");
    check(renderPeak(audio, 480) == 0.0f, "nothing playing renders silence");

    const kast::AudioHandle tone = audio.play("sounds/tone.wav", {.loop = true});
    check(tone != 0 && audio.isPlaying(tone) && audio.voiceCount() == 1, "play returns a live handle");
    const float full = renderPeak(audio, 2400);
    check(std::abs(full - 0.5f) < 0.01f, "a playing clip renders non-silent output at its amplitude");
    // Each change is measured after settle() (see kSettleFrames).
    audio.setBusMuted(kast::AudioBus::SFX, true);
    settle(audio);
    check(renderPeak(audio, 2400) == 0.0f && audio.busMuted(kast::AudioBus::SFX), "its bus muted: silence");
    audio.setBusMuted(kast::AudioBus::SFX, false);
    audio.setBusMuted(kast::AudioBus::Master, true);
    settle(audio);
    check(renderPeak(audio, 2400) == 0.0f, "Master muted: silence");
    audio.setBusMuted(kast::AudioBus::Master, false);
    settle(audio);
    check(renderPeak(audio, 2400) > 0.4f, "unmuted again: sound");
    audio.setBusMuted(kast::AudioBus::Music, true);
    settle(audio);
    check(renderPeak(audio, 2400) > 0.4f, "another bus muted: still sound");
    audio.setBusMuted(kast::AudioBus::Music, false);

    // Gain = sound volume x bus volume x Master volume. Tolerance 0.005 (16-bit samples, the sine's sampled peak).
    audio.setVolume(tone, 0.5f);
    audio.setBusVolume(kast::AudioBus::SFX, 0.8f);
    audio.setBusVolume(kast::AudioBus::Master, 0.5f);
    settle(audio);
    const float gained = renderPeak(audio, 2400);
    check(std::abs(gained - 0.5f * 0.5f * 0.8f * 0.5f) < 0.005f, "rendered peak = amplitude x volume x bus x Master");
    kast::log::info(std::format("audio gain: peak {:.4f}, expected {:.4f}", gained, 0.5f * 0.5f * 0.8f * 0.5f));
    check(audio.busVolume(kast::AudioBus::SFX) == 0.8f && audio.busVolume(kast::AudioBus::Master) == 0.5f,
          "bus volumes read back");
    check(throwsInvalid([&] { audio.setBusVolume(kast::AudioBus::UI, 1.5f); }, "between 0 and 1") &&
              throwsInvalid([&] { audio.setBusVolume(kast::AudioBus::UI, -0.1f); }, "between 0 and 1") &&
              throwsInvalid([&] { audio.setVolume(tone, 2.0f); }, "between 0 and 1"),
          "out-of-range volumes are rejected");
    kast::AudioMix mix{};
    mix[static_cast<int>(kast::AudioBus::Music)].volume = 2.0f;
    check(throwsInvalid([&] { audio.setMix(mix); }, "between 0 and 1"), "setMix rejects an out-of-range volume");

    audio.start(dir); // a new session: voices stopped, buses back to 1.0 unmuted
    check(audio.voiceCount() == 0 && !audio.isPlaying(tone) && audio.busVolume(kast::AudioBus::SFX) == 1.0f &&
              audio.busVolume(kast::AudioBus::Master) == 1.0f,
          "start() stops every voice and resets the buses");

    // Music streams (decoded while playing) through the same buses.
    const kast::AudioHandle music = audio.play("sounds/tone.wav", {.bus = kast::AudioBus::Music, .volume = 0.5f});
    check(std::abs(renderPeak(audio, 2400) - 0.25f) < 0.005f, "a Music (streamed) clip plays at its volume");
    audio.setBusVolume(kast::AudioBus::Music, 0.5f);
    settle(audio);
    check(std::abs(renderPeak(audio, 2400) - 0.125f) < 0.005f, "the Music bus volume applies");
    audio.stopAll(kast::AudioBus::SFX);
    check(audio.isPlaying(music), "stopAll(SFX) leaves Music playing");
    audio.stopAll(kast::AudioBus::Music);
    check(audio.voiceCount() == 0, "stopAll(Music) stops it");
}

void checkAudioLooping()
{
    const std::filesystem::path dir = audioTestDir();
    kast::Audio audio(kast::Audio::Output::Offline);
    audio.start(dir);
    constexpr uint32_t length = 12000; // tone.wav
    const kast::AudioHandle loop = audio.play("sounds/tone.wav", {.loop = true});
    bool silentChunk = false;
    for (uint32_t done = 0; done < length * 5 / 2; done += 600) // 2.5x its length, in 12.5 ms chunks
        silentChunk |= renderPeak(audio, 600) < 0.4f;
    check(!silentChunk && audio.isPlaying(loop) && audio.voiceCount() == 1,
          "a looping clip keeps sounding past 2x its length, with no silent gap");
    audio.stop(loop);

    const kast::AudioHandle once = audio.play("sounds/tone.wav", {.bus = kast::AudioBus::UI});
    check(renderPeak(audio, length - 600) > 0.4f, "a one-shot sounds for its length");
    renderPeak(audio, 1200); // past its end
    check(renderPeak(audio, 600) == 0.0f, "a one-shot goes silent after its length");
    check(!audio.isPlaying(once) && audio.voiceCount() == 0, "an ended one-shot frees its voice; its handle is stale");

    // A streamed (Music) loop too.
    const kast::AudioHandle music = audio.play("sounds/tone.wav", {.bus = kast::AudioBus::Music, .loop = true});
    silentChunk = false;
    for (uint32_t done = 0; done < length * 5 / 2; done += 600)
        silentChunk |= renderPeak(audio, 600) < 0.4f;
    check(!silentChunk && audio.isPlaying(music), "a looping Music stream keeps sounding past 2x its length");
}

void checkAudioVoices()
{
    const std::filesystem::path dir = audioTestDir();
    kast::Audio audio(kast::Audio::Output::Offline);
    audio.start(dir);
    std::vector<kast::AudioHandle> handles;
    for (int i = 0; i <= kast::kMaxVoices; ++i) // 33 one-shots
        handles.push_back(audio.play("sounds/tone.wav", {}));
    check(audio.voiceCount() == kast::kMaxVoices, "33 one-shots: the voice count stays at 32");
    check(!audio.isPlaying(handles[0]), "the oldest one-shot was stolen: its handle is stale");
    check(std::ranges::all_of(handles.begin() + 1, handles.end(), [&](kast::AudioHandle h) { return audio.isPlaying(h); }),
          "the 32 newer one-shots play");
    check(std::ranges::adjacent_find(handles) == handles.end() && handles[kast::kMaxVoices] != handles[0],
          "a reused voice gets a new handle");
    audio.stop(handles[0]); // stale: no-op, must not stop the voice that took its slot
    check(audio.voiceCount() == kast::kMaxVoices && audio.isPlaying(handles[kast::kMaxVoices]),
          "stopping a stolen voice's stale handle does nothing");

    // Looping voices are never stolen: one loop among one-shots survives 40 more plays.
    audio.stopAll();
    const kast::AudioHandle keeper = audio.play("sounds/tone.wav", {.loop = true});
    for (int i = 0; i < 40; ++i)
        audio.play("sounds/tone.wav", {});
    check(audio.isPlaying(keeper) && audio.voiceCount() == kast::kMaxVoices, "a looping voice is not stolen");

    audio.stopAll();
    std::vector<kast::AudioHandle> loops;
    for (int i = 0; i < kast::kMaxVoices; ++i)
        loops.push_back(audio.play("sounds/tone.wav", {.loop = true}));
    const auto warnings = [] {
        return std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
            return contains(line, "Sound 'sounds/ramp.wav' dropped: all 32 voices are looping");
        });
    };
    const auto before = warnings();
    const kast::AudioHandle dropped = audio.play("sounds/ramp.wav", {});
    check(dropped == 0 && warnings() == before + 1, "all 32 looping: a 33rd play is dropped with a warning naming the clip");
    check(audio.voiceCount() == kast::kMaxVoices &&
              std::ranges::all_of(loops, [&](kast::AudioHandle h) { return audio.isPlaying(h); }),
          "no looping voice was stolen");
    check(!audio.isPlaying(dropped), "the dropped play's handle (0) is stale");

    const std::vector<kast::Audio::Voice> voices = audio.voices();
    check(voices.size() == kast::kMaxVoices && voices.front().handle == loops.front() && voices.front().loop &&
              voices.front().clip == "sounds/tone.wav" && voices.front().bus == kast::AudioBus::SFX,
          "voices() lists the live voices oldest first");
}

void checkAudioHandles()
{
    const std::filesystem::path dir = audioTestDir();
    kast::Audio audio(kast::Audio::Output::Offline);
    audio.start(dir);
    // ramp.wav: sample i = i / 65536, so a rendered sample tells the playback position. Tolerance: 64 frames
    // (miniaudio's stop / start fades and read-ahead move it by a few frames; 16-bit samples step by 2 / 65536).
    const auto position = [&] {
        renderPeak(audio, 16); // past the start fade
        return renderOne(audio) * 65536.0f;
    };
    const kast::AudioHandle ramp = audio.play("sounds/ramp.wav", {});
    renderPeak(audio, 1000);
    audio.pause(ramp);
    check(!audio.isPlaying(ramp) && audio.voiceCount() == 1 && audio.voices().front().paused,
          "pause: not playing, voice kept");
    settle(audio);
    check(renderPeak(audio, 5000) == 0.0f, "a paused voice is silent");
    audio.resume(ramp);
    check(audio.isPlaying(ramp), "resume: playing again");
    float at = position();
    check(std::abs(at - 1017.0f) < 64.0f, "resume continues from the paused position (not reset, not advanced)");
    kast::log::info(std::format("audio resume: position {} (paused at 1000, +17 rendered since resume)", at));
    audio.setVolume(ramp, 0.5f);
    check(audio.voices().front().volume == 0.5f, "setVolume sets the voice volume");

    // Editor Pause (setPaused) holds every voice and keeps per-handle pauses.
    const kast::AudioHandle held = audio.play("sounds/tone.wav", {.loop = true});
    audio.pause(held);
    renderPeak(audio, 1000);
    at = position();
    audio.setPaused(true);
    settle(audio);
    check(renderPeak(audio, 4800) == 0.0f, "setPaused(true): silence");
    audio.setPaused(false);
    const float after = position() / 0.5f; // the ramp's volume is 0.5
    check(std::abs(after - (at / 0.5f + 18.0f)) < 64.0f && !audio.isPlaying(held),
          "setPaused(false): the ramp continues where it was held; the handle-paused voice stays paused");

    audio.stop(ramp);
    check(!audio.isPlaying(ramp) && audio.voiceCount() == 1, "stop frees the voice; the handle is stale");
    audio.stop(ramp);
    audio.pause(ramp);
    audio.resume(ramp);
    audio.setVolume(ramp, 0.3f);
    check(!audio.isPlaying(ramp) && audio.voiceCount() == 1, "stale handle calls are no-ops, not errors");
    check(!audio.isPlaying(0) && !audio.isPlaying(0xFFFFFFFFFFull), "never-issued handles are stale");

    check(throwsInvalid([&] { audio.play("sounds/none.wav", {}); }, "cannot read 'sounds/none.wav'"), "missing file");
    check(throwsInvalid([&] { audio.play("sounds/broken.wav", {}); }, "cannot decode 'sounds/broken.wav'"), "corrupt WAV");
    check(throwsInvalid([&] { audio.play("sounds/broken.mp3", {.bus = kast::AudioBus::Music}); }, "cannot decode"),
          "corrupt MP3 (streamed)");
    check(throwsInvalid([&] { audio.play("sounds/tone.ogg", {}); }, "not a .wav or .mp3 file"), ".ogg");
    check(throwsInvalid([&] { audio.play("sounds/tone.wav", {.volume = 1.5f}); }, "between 0 and 1"), "volume 1.5");
    check(audio.voiceCount() == 1, "failed plays take no voice");

    kast::Audio noProject(kast::Audio::Output::Offline);
    noProject.start(std::nullopt);
    check(throwsInvalid([&] { noProject.play("sounds/tone.wav", {}); }, "no project"), "no project: play fails");
}

// The null backend (no device): one warning, then everything behaves normally (time passes on the null device).
void checkAudioNullDevice()
{
    const std::filesystem::path dir = audioTestDir();
    const auto warnings = [] {
        return std::ranges::count_if(kast::log::lines(),
                                     [](const std::string& line) { return contains(line, "No audio output device"); });
    };
    const auto before = warnings();
    kast::Audio audio(kast::Audio::Output::Null);
    audio.start(dir);
    audio.start(dir); // a second session: no second warning
    check(warnings() == before + 1, "no audio device: one warning");
    const kast::AudioHandle h = audio.play("sounds/tone.wav", {.bus = kast::AudioBus::Voice, .loop = true});
    check(audio.isPlaying(h) && audio.voiceCount() == 1 && audio.sampleRate() > 0, "the null backend plays");
    audio.pause(h);
    check(!audio.isPlaying(h), "pause works");
    audio.resume(h);
    audio.setBusVolume(kast::AudioBus::Voice, 0.25f);
    check(audio.isPlaying(h) && audio.busVolume(kast::AudioBus::Voice) == 0.25f, "resume and bus volume work");
    const kast::AudioHandle once = audio.play("sounds/tone.wav", {});
    for (int i = 0; i < 200 && audio.isPlaying(once); ++i) { // 0.25 s clip; the null device consumes in real time
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        audio.update();
    }
    check(!audio.isPlaying(once) && audio.isPlaying(h), "a one-shot ends on the null device; the loop keeps going");
    check(throwsInvalid([&] { audio.play("sounds/broken.wav", {}); }, "cannot decode"), "errors as with a device");
    audio.stop();
    check(audio.voiceCount() == 0, "stop() frees every voice");
}

// AudioSource: registry, validation, save/load round trip.
void checkAudioSourceComponent()
{
    const entt::meta_type type = kast::findComponentType("AudioSource");
    check(static_cast<bool>(type), "AudioSource is registered");
    std::string error;
    const auto fromJson = [&](const char* json) {
        return static_cast<bool>(kast::componentFromJson(type, nlohmann::ordered_json::parse(json), error));
    };
    check(fromJson(R"({"clip": "sounds/a.WAV", "bus": "Music", "volume": 0.5, "loop": true, "playOnStart": true})"),
          "a valid AudioSource loads");
    check(!fromJson(R"({"clip": "a.wav", "bus": "SFX", "volume": 1.5, "loop": false, "playOnStart": false})") &&
              contains(error, "volume"),
          "volume 1.5 is rejected");
    check(!fromJson(R"({"clip": "a.wav", "bus": "SFX", "volume": -0.5, "loop": false, "playOnStart": false})"),
          "volume -0.5 is rejected");
    check(!fromJson(R"({"clip": "a.ogg", "bus": "SFX", "volume": 1, "loop": false, "playOnStart": false})") &&
              contains(error, ".wav or .mp3"),
          "a .ogg clip is rejected");
    check(!fromJson(R"({"clip": "a.wav", "bus": "Ambient", "volume": 1, "loop": false, "playOnStart": false})"),
          "an unknown bus is rejected");
    check(fromJson(R"({"clip": "", "bus": "SFX", "volume": 1, "loop": false, "playOnStart": false})"), "no clip is valid");

    kast::Scene scene;
    const entt::entity e = scene.create("Speaker");
    scene.registry().emplace<kast::AudioSource>(e, "sounds/tone.wav", kast::AudioBus::UI, 0.25f, true, true);
    const std::filesystem::path dir = looseTestDir("kast_audio_source_check");
    check(kast::saveScene(scene, (dir / "a.scene").string(), error), "save a scene with an AudioSource");
    kast::Scene loaded;
    check(kast::loadScene((dir / "a.scene").string(), loaded, error), "load it back");
    const auto* source = loaded.registry().try_get<kast::AudioSource>(named(loaded, "Speaker"));
    check(source && source->clip == "sounds/tone.wav" && source->bus == kast::AudioBus::UI && source->volume == 0.25f &&
              source->loop && source->playOnStart,
          "AudioSource round-trips through save/load");
    check(kast::sceneToJson(loaded).dump() == kast::sceneToJson(scene).dump(), "the reloaded scene saves identically");
}

void checkAudioProjectSettings()
{
    const std::filesystem::path dir = projectTestDir("kast_audio_project_check", "Mixed");
    kast::Project project;
    std::string error;
    check(kast::loadProject(dir / kast::kProjectFileName, project, error) && project.audio == kast::AudioMix{},
          "project.kast without \"audio\": every bus 1.0, unmuted");
    check(std::ranges::all_of(project.audio, [](const kast::BusMix& b) { return b.volume == 1.0f && !b.muted; }),
          "the default mix is 1.0 unmuted");
    check(kast::saveProject(project, error), "save it");
    std::string text;
    kast::readFile(dir / kast::kProjectFileName, text);
    check(!contains(text, "\"audio\""), "an all-default mix is left out of project.kast (existing files stay unchanged)");

    project.audio[static_cast<int>(kast::AudioBus::Music)] = {0.25f, false};
    project.audio[static_cast<int>(kast::AudioBus::UI)] = {0.5f, true};
    check(kast::saveProject(project, error), "save bus defaults");
    kast::Project reloaded;
    check(kast::loadProject(dir / kast::kProjectFileName, reloaded, error) && reloaded.audio == project.audio,
          "bus defaults round-trip through project.kast");
    kast::readFile(dir / kast::kProjectFileName, text);
    check(contains(text, "\"version\": 1") && contains(text, "\"audio\""), "kProjectFileVersion stays 1");

    writeFile(dir / "partial.kast", R"({"version": 1, "name": "P", "startScene": null, "audio": {"SFX": {"volume": 0.5}}})");
    check(kast::loadProject(dir / "partial.kast", reloaded, error) &&
              reloaded.audio[static_cast<int>(kast::AudioBus::SFX)] == kast::BusMix{0.5f, false} &&
              reloaded.audio[static_cast<int>(kast::AudioBus::Music)] == kast::BusMix{},
          "a partial audio block: missing buses and keys keep their defaults");
    for (const char* bad : {R"("audio": {"SFX": {"volume": 1.5}})", R"("audio": {"Ambient": {}})",
                            R"("audio": {"SFX": {"muted": 1}})", R"("audio": {"SFX": {"gain": 1}})", R"("audio": [])"}) {
        writeFile(dir / "bad.kast", std::format(R"({{"version": 1, "name": "P", "startScene": null, {}}})", bad));
        check(!kast::loadProject(dir / "bad.kast", reloaded, error) && contains(error, "audio"),
              std::format("invalid audio block rejected: {}", bad).c_str());
    }

    // Applied at Play start (editor): the session's buses take the project defaults.
    kast::Scene scene;
    scene.create("E");
    check(kast::saveScene(scene, (dir / "a.scene").string(), error), "save a scene in the project");
    Document doc;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open it");
    startPlay(doc);
    check(doc.lua.audio().busVolume(kast::AudioBus::Music) == 0.25f && doc.lua.audio().busMuted(kast::AudioBus::UI) &&
              doc.lua.audio().busVolume(kast::AudioBus::SFX) == 1.0f,
          "Play start applies the project's bus defaults");
    doc.lua.audio().setBusVolume(kast::AudioBus::Music, 0.9f); // a runtime change (kast.audio.setBusVolume)
    stopPlay(doc);
    check(doc.project->audio[static_cast<int>(kast::AudioBus::Music)].volume == 0.25f, "runtime changes are not saved");
    check(setBusDefault(doc, kast::AudioBus::Voice, {0.75f, true}, error), "setBusDefault (Mixer panel)");
    check(kast::loadProject(dir / kast::kProjectFileName, reloaded, error) &&
              reloaded.audio[static_cast<int>(kast::AudioBus::Voice)] == kast::BusMix{0.75f, true},
          "setBusDefault saves project.kast");
    check(!setBusDefault(doc, kast::AudioBus::Voice, {1.5f, false}, error), "setBusDefault rejects volume 1.5");
}

// Online Services (phase 1): the project.kast "online" block, its checks, the modal's Save and what Build packs.
void checkOnlineSettings()
{
    const std::filesystem::path dir = projectTestDir("kast_online_check", "Online");
    const std::filesystem::path file = dir / kast::kProjectFileName;
    const std::string testCa = readText(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "certs/net_test_ca.pem");
    std::filesystem::create_directories(dir / "certs");
    std::filesystem::create_directories(dir / ".secret");
    writeFile(dir / "certs/ca.pem", testCa);
    writeFile(dir / ".secret/ca.pem", testCa);
    writeFile(dir / "certs/bad.pem", "-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n");
    check(kast::isValidCaPem(testCa) && !kast::isValidCaPem("") && !kast::isValidCaPem("hello") &&
              !kast::isValidCaPem(readText(dir / "certs/bad.pem")),
          "isValidCaPem accepts the test CA, rejects empty, plain text and a broken PEM");

    // Old files: no "online" block = disabled, and saving keeps it out (older readers reject unknown keys).
    kast::Project project;
    std::string error;
    check(kast::loadProject(file, project, error) && project.online == kast::OnlineConfig{} && !project.online.enabled,
          "project.kast without \"online\" loads with Online Services disabled");
    check(kast::saveProject(project, error) && !contains(readText(file), "online"),
          "saving an all-default online config leaves the block out");

    // Modal Save: checked first, project.kast byte-identical and project unchanged on failure.
    const std::string before = readText(file);
    const auto rejected = [&](const kast::OnlineConfig& online, const char* why, const char* what) {
        check(!setOnlineConfig(project, online, error) && contains(error, why) && readText(file) == before &&
                  project.online == kast::OnlineConfig{},
              what);
        if (!contains(error, why))
            kast::log::error(std::format("  got: {}", error));
    };
    rejected({.enabled = true}, "Auth URL is required", "enabled without an Auth URL is rejected");
    rejected({.enabled = true, .authUrl = "ftp://auth.example.com"}, "Auth URL", "a non-http(s) Auth URL is rejected");
    rejected({.enabled = false, .authUrl = "not a url"}, "Auth URL", "a bad Auth URL is rejected even while disabled");
    rejected({.enabled = true, .authUrl = "https://localhost:3100", .caFile = "certs/none.pem"}, "missing",
             "a missing CA file is rejected");
    rejected({.enabled = true, .authUrl = "https://localhost:3100", .caFile = "certs/bad.pem"}, "no valid PEM",
             "a CA file without a valid certificate is rejected");
    rejected({.enabled = true, .authUrl = "https://localhost:3100", .caFile = "../ca.pem"}, "outside the project",
             "a CA file outside the project is rejected");
    rejected({.enabled = true, .authUrl = "https://localhost:3100", .caFile = (dir / "certs/ca.pem").string()},
             "relative to the project root", "an absolute CA file path is rejected");
    rejected({.enabled = true, .authUrl = "https://localhost:3100", .caFile = ".secret/ca.pem"}, "game.pak",
             "a CA file in a dot-folder (left out of game.pak) is rejected");

    // Round trip: enable, set URL and CA file, save, reopen.
    const kast::OnlineConfig online{.enabled = true, .authUrl = "https://localhost:3100", .caFile = "certs/ca.pem"};
    check(setOnlineConfig(project, online, error) && project.online == online, "Save a valid online config");
    check(readText(file) == "{\n  \"version\": 1,\n  \"name\": \"Online\",\n  \"startScene\": null,\n"
                            "  \"online\": {\n    \"enabled\": true,\n    \"authUrl\": \"https://localhost:3100\",\n"
                            "    \"caFile\": \"certs/ca.pem\"\n  }\n}\n",
          "project.kast gains the online block, version stays 1");
    kast::Project reloaded;
    check(kast::loadProject(file, reloaded, error) && reloaded.online == online, "the online block round-trips");
    std::string caPem;
    check(kast::loadOnlineCa(reloaded, caPem, error) && caPem == testCa, "loadOnlineCa reads the CA file");

    // Disabled: values kept (toggling off loses nothing), no CA file required, no CA read.
    const kast::OnlineConfig off{.enabled = false, .authUrl = "http://localhost:3100", .caFile = "certs/none.pem"};
    check(setOnlineConfig(project, off, error) && kast::loadProject(file, reloaded, error) && reloaded.online == off,
          "disabled with a missing CA file saves and round-trips (no CA file required)");
    check(kast::loadOnlineCa(reloaded, caPem, error) && caPem.empty(), "loadOnlineCa reads nothing while disabled");

    // loadProject checks the block's shape and values but not files: a deleted CA file still opens the project.
    for (const char* bad : {R"("online": [])", R"("online": {"enabled": 1})", R"("online": {"authUrl": 5})",
                            R"("online": {"token": "x"})", R"("online": {"enabled": true})",
                            R"("online": {"authUrl": "ws://a"})", R"("online": {"caFile": "../ca.pem"})"}) {
        writeFile(dir / "bad.kast", std::format(R"({{"version": 1, "name": "P", "startScene": null, {}}})", bad));
        reloaded = kast::Project{.name = "untouched"};
        check(!kast::loadProject(dir / "bad.kast", reloaded, error) && contains(error, "online") &&
                  reloaded.name == "untouched",
              std::format("invalid online block rejected, out untouched: {}", bad).c_str());
    }
    writeFile(dir / "gone.kast", R"({"version": 1, "name": "P", "startScene": null,
        "online": {"enabled": true, "authUrl": "https://a", "caFile": "certs/gone.pem"}})");
    check(kast::loadProject(dir / "gone.kast", reloaded, error) && !kast::loadOnlineCa(reloaded, caPem, error) &&
              contains(error, "certs/gone.pem"),
          "a missing CA file loads, then loadOnlineCa reports it");

    // Build: checks the config, and game.pak carries project.kast's block and the CA file.
    const std::filesystem::path playerDir = std::filesystem::path(SDL_GetBasePath());
    saveTestScene(dir / "a.scene", "Thing", {});
    project.startScene = "a.scene";
    project.online = {.enabled = true, .authUrl = "https://localhost:3100", .caFile = "certs/none.pem"};
    std::filesystem::path exe;
    const std::filesystem::path out = looseTestDir("kast_online_out");
    check(!buildGame(project, out / "bad", playerDir, exe, error) && contains(error, "Online Services") &&
              !std::filesystem::exists(out / "bad"),
          "Build fails on a missing CA file while enabled, writing nothing");
    project.online = online;
    check(kast::saveProject(project, error) && buildGame(project, out / "on", playerDir, exe, error),
          "Build an online project");
    kast::Pak pak;
    const std::optional<std::string_view> packedProject =
        pak.open(out / "on" / kGamePakName, error) ? pak.find("project.kast") : std::nullopt;
    check(packedProject && contains(std::string(*packedProject), "\"authUrl\": \"https://localhost:3100\"") &&
              pak.find("certs/ca.pem") == std::optional<std::string_view>(testCa),
          "game.pak holds the online block and the CA file");

    // Disabled: no CA file needed and nothing online-specific packed.
    const std::filesystem::path plain = projectTestDir("kast_online_off_check", "Offline");
    saveTestScene(plain / "a.scene", "Thing", {});
    const kast::Project offline{.root = plain, .name = "Offline", .startScene = "a.scene"};
    check(kast::saveProject(offline, error) && buildGame(offline, out / "off", playerDir, exe, error) &&
              pak.open(out / "off" / kGamePakName, error) &&
              pak.list("") == std::vector<std::string>{"a.scene", "project.kast"} &&
              !contains(std::string(*pak.find("project.kast")), "online"),
          "a disabled project builds without a CA file and packs no online block");

    // Modal state: open blocks other file actions.
    Document doc;
    resetToDefault(doc);
    doc.project = project;
    FileActions files(doc);
    files.openOnlineSettings();
    check(files.onlineSettingsOpen() && !files.idle() && !files.canBuild(), "Online Services open: not idle, Build disabled");
}

// AudioSource playOnStart, kast.audio from Lua, Stop / Pause / kast.loadScene in an editor Play session.
void checkAudioPlaySession()
{
    const std::filesystem::path dir = audioTestDir();
    kast::Scene a;
    const entt::entity music = a.create("Music");
    a.registry().emplace<kast::AudioSource>(music, "sounds/tone.wav", kast::AudioBus::Music, 0.5f, true, true);
    const entt::entity broken = a.create("Broken");
    a.registry().emplace<kast::AudioSource>(broken, "sounds/broken.wav", kast::AudioBus::SFX, 1.0f, false, true);
    const entt::entity button = a.create("Button");
    a.registry().emplace<kast::AudioSource>(button, "sounds/ramp.wav", kast::AudioBus::UI, 1.0f, false, false);
    const entt::entity plain = a.create("Plain");
    a.systems = {"scripts/audio.lua"};
    writeFile(dir / "scripts/audio.lua", std::format(R"lua(local MUSIC, BUTTON, PLAIN = {}, {}, {}
local frame = 0
local function fails(pattern, f, ...)
    local ok, err = pcall(f, ...)
    assert(not ok, "expected an error matching " .. pattern)
    assert(string.find(err, pattern, 1, true), "error was: " .. tostring(err))
end
return {{
    update = function(dt)
        frame = frame + 1
        if frame == 1 then
            local h = kast.audio.play("sounds/tone.wav", {{bus = "SFX", volume = 0.5}})
            assert(type(h) == "number" and h > 0 and kast.audio.isPlaying(h), "play returns a playing handle")
            kast.audio.pause(h)
            assert(not kast.audio.isPlaying(h), "pause")
            kast.audio.resume(h)
            assert(kast.audio.isPlaying(h), "resume")
            kast.audio.setVolume(h, 0.25)
            kast.audio.stop(h)
            assert(not kast.audio.isPlaying(h), "stop")
            kast.audio.stop(h); kast.audio.pause(h); kast.audio.resume(h); kast.audio.setVolume(h, 1)
            assert(kast.audio.isPlaying(h) == false, "a stale handle: no-ops, no error")
            local loop = kast.audio.play("sounds/tone.wav", {{loop = true}})
            local ui = kast.audio.play("sounds/tone.wav", {{bus = "UI"}})
            kast.audio.stopAll("UI")
            assert(kast.audio.isPlaying(loop) and not kast.audio.isPlaying(ui), "stopAll(bus)")
            kast.audio.setBusVolume("Music", 0.5)
            assert(kast.audio.getBusVolume("Music") == 0.5, "bus volume")
            kast.audio.setBusMuted("Voice", true)
            local b = kast.audio.playSource(BUTTON)
            assert(kast.audio.isPlaying(b), "playSource")
            kast.audio.stopSource(BUTTON)
            assert(not kast.audio.isPlaying(b), "stopSource")
            kast.audio.stopSource(MUSIC) -- the playOnStart voice
            kast.audio.playSource(MUSIC)
            fails("cannot read 'sounds/none.wav'", kast.audio.play, "sounds/none.wav")
            fails("cannot decode 'sounds/broken.wav'", kast.audio.play, "sounds/broken.wav")
            fails("'sounds/tone.ogg' is not a .wav or .mp3 file", kast.audio.play, "sounds/tone.ogg")
            fails("kast.audio.play: unknown bus Ambient (expected", kast.audio.play, "sounds/tone.wav", {{bus = "Ambient"}})
            fails("kast.audio.play: volume must be a number between 0 and 1, got 1.5", kast.audio.play, "sounds/tone.wav", {{volume = 1.5}})
            fails("kast.audio.play: unknown option pitch (expected bus, volume or loop)", kast.audio.play, "sounds/tone.wav", {{pitch = 2}})
            fails("kast.audio.play: path must be a string", kast.audio.play, 42)
            fails("kast.audio.setVolume: volume must be a number between 0 and 1", kast.audio.setVolume, loop, 2)
            fails("kast.audio.setBusVolume: volume must be a number between 0 and 1", kast.audio.setBusVolume, "SFX", -1)
            fails("kast.audio.setBusVolume: unknown bus", kast.audio.setBusVolume, "sfx", 1)
            fails("kast.audio.stopAll: unknown bus", kast.audio.stopAll, "Everything")
            fails("kast.audio.isPlaying: invalid sound handle", kast.audio.isPlaying, "h")
            fails("kast.audio.playSource: entity " .. PLAIN .. " has no AudioSource", kast.audio.playSource, PLAIN)
            fails("kast.audio.stopSource: entity " .. PLAIN .. " has no AudioSource", kast.audio.stopSource, PLAIN)
            fails("scripts/audio.lua:", function() kast.audio.play("sounds/none.wav") end) -- raised at the calling line
            kast.log("audio lua frame 1 ok")
        elseif frame == 2 then
            kast.log("audio lua frame 2 ran") -- the errors did not stop the system
        elseif frame == 3 then
            kast.loadScene("b.scene")
        end
    end,
}}
)lua",
                                                     idOf(a, music), idOf(a, button), idOf(a, plain)));
    kast::Scene b;
    b.create("Quiet");
    std::string error;
    check(kast::saveScene(a, (dir / "a.scene").string(), error) && kast::saveScene(b, (dir / "b.scene").string(), error),
          "save the audio scenes");

    Document doc;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open the audio scene");
    const size_t errorsBefore = std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
        return contains(line, "AudioSource on 'Broken'");
    });
    startPlay(doc);
    kast::Audio& audio = doc.lua.audio();
    check(audio.voiceCount() == 1 && audio.voices().front().clip == "sounds/tone.wav" &&
              audio.voices().front().bus == kast::AudioBus::Music && audio.voices().front().loop,
          "Play start: the playOnStart AudioSource plays (looping, Music)");
    check(std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
              return contains(line, "AudioSource on 'Broken': cannot decode 'sounds/broken.wav'");
          }) == static_cast<std::ptrdiff_t>(errorsBefore) + 1,
          "a corrupt playOnStart clip logs one error and plays nothing");
    check(std::abs(renderPeak(audio, 2400) - 0.25f) < 0.005f, "the playOnStart clip sounds at its volume");

    updatePlay(doc, 1.0f / 60.0f);
    checkOk(doc.lua, 0, "kast.audio from Lua: handles, buses, sources and errors");
    check(logged("audio lua frame 1 ok"), "the kast.audio checks ran");
    check(audio.voiceCount() == 2 && audio.busVolume(kast::AudioBus::Music) == 0.5f && audio.busMuted(kast::AudioBus::Voice),
          "after frame 1: the loop and the restarted source play; bus changes applied");

    // Editor Pause holds every voice; resume continues them.
    togglePause(doc);
    settle(audio);
    check(doc.play == PlayState::Paused && renderPeak(audio, 2400) == 0.0f && audio.voiceCount() == 2,
          "Pause: every voice held (silent, kept)");
    togglePause(doc);
    check(renderPeak(audio, 2400) > 0.1f, "resume: sound again");

    updatePlay(doc, 1.0f / 60.0f);
    check(logged("audio lua frame 2 ran") && statusOf(doc.lua, 0).ok, "the system keeps running after its Lua errors");
    check(audio.voiceCount() == 2, "voices before the scene switch");
    updatePlay(doc, 1.0f / 60.0f); // frame 3: kast.loadScene("b.scene")
    check(doc.lua.scenePath() == "b.scene" && audio.voiceCount() == 0, "kast.loadScene leaves zero active voices");
    check(audio.busVolume(kast::AudioBus::Music) == 0.5f, "runtime bus levels carry over a scene switch");

    stopPlay(doc);
    startPlay(doc);
    check(audio.voiceCount() == 1, "Play again: the playOnStart source plays");
    updatePlay(doc, 1.0f / 60.0f);
    check(audio.voiceCount() == 2, "the script played a loop");
    stopPlay(doc);
    check(audio.voiceCount() == 0, "Stop leaves zero active voices");
}

// Last: mounting a pak lasts for the rest of the process, so later disk reads would fail.
void checkMountedPak()
{
    const std::filesystem::path dir = looseTestDir("kast_mount_check");
    std::filesystem::create_directories(dir / "src/components/sub");
    writeFile(dir / "src/project.kast", R"({"version": 1, "name": "Packed", "startScene": "a.scene",
        "online": {"enabled": true, "authUrl": "https://localhost:3100", "caFile": "certs/ca.pem"}})");
    std::filesystem::create_directories(dir / "src/certs");
    std::filesystem::copy_file(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "certs/net_test_ca.pem", dir / "src/certs/ca.pem");
    writeFile(dir / "src/components/sub/c.lua", "return 1");
    writeFile(dir / "src/components/d.lua", "return 2");
    std::filesystem::create_directories(dir / "src/sounds");
    writeTone(dir / "src/sounds/tone.wav", 12000);
    std::filesystem::copy_file(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "sounds/mp3-demo.mp3",
                               dir / "src/sounds/mp3-demo.mp3");
    std::string error;
    check(kast::writePak(dir / "src", {"project.kast", "components/sub/c.lua", "components/d.lua", "sounds/tone.wav",
                                       "sounds/mp3-demo.mp3", "certs/ca.pem"},
                         dir / "game.pak", error),
          "write a pak to mount");
    kast::Pak pak;
    check(pak.open(dir / "game.pak", error), "open the pak to mount");
    const std::filesystem::path root = dir / "game"; // not on disk: everything comes from the pak
    kast::mountPak(std::move(pak), root);
    kast::Project project;
    check(kast::loadProject(root / "project.kast", project, error) && project.name == "Packed", "project.kast from the pak");
    // As kast_player loads it: the online config and its CA file come from the pak.
    std::string caPem;
    check(project.online == kast::OnlineConfig{.enabled = true, .authUrl = "https://localhost:3100", .caFile = "certs/ca.pem"} &&
              kast::loadOnlineCa(project, caPem, error) &&
              caPem == readText(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "certs/net_test_ca.pem"),
          "the online config and CA file load from the pak");
    std::string text;
    check(kast::readFile(root / "components/d.lua", text) && text == "return 2", "readFile from the pak");
    check(!kast::readFile(dir / "src/components/d.lua", text), "with a pak mounted, the disk is not read");
    check(kast::listFiles(root / "components") ==
              std::vector<std::filesystem::path>{root / "components/d.lua", root / "components/sub/c.lua"},
          "listFiles from the pak");
    check(kast::listFiles(root).size() == 6 && kast::listFiles(root / "nothing").empty(),
          "listFiles of the root and of a missing folder");

    // Audio from the pak (as kast_player plays it): decoded through kast::readFile, never the disk.
    std::string reason;
    const std::optional<kast::ClipInfo> wav = kast::clipInfo(root, "sounds/tone.wav", reason);
    check(wav && wav->frames == 12000 && wav->channels == 2 && wav->sampleRate == kToneRate, "a WAV decodes from the pak");
    const std::optional<kast::ClipInfo> mp3 = kast::clipInfo(root, "sounds/mp3-demo.mp3", reason);
    check(mp3 && mp3->frames == 87552 && mp3->channels == 2 && mp3->sampleRate == 44100, "an MP3 decodes from the pak");
    kast::Audio audio(kast::Audio::Output::Offline);
    audio.start(root);
    audio.play("sounds/tone.wav", {});
    audio.play("sounds/mp3-demo.mp3", {.bus = kast::AudioBus::Music});
    check(audio.voiceCount() == 2 && renderPeak(audio, 4800) > 0.4f, "pak clips play (cached WAV, streamed MP3)");
}

// --window-pos / KAST_WINDOW_POS: "X,Y" integers, negative allowed (a monitor left of or above the main one).
void checkWindowPos()
{
    using P = std::optional<std::pair<int, int>>;
    check(kast::parseWindowPos("100,200") == P{{100, 200}} && kast::parseWindowPos("-1900,40") == P{{-1900, 40}} &&
              kast::parseWindowPos("0,-5") == P{{0, -5}},
          "window pos: X,Y with negative values");
    bool rejected = true;
    for (const char* bad : {"", "100", "100,", ",5", "1x2", "1,2,3", "a,b", "1.5,2", " 1,2"})
        rejected &= !kast::parseWindowPos(bad);
    check(rejected, "window pos: anything but two integers is rejected");
}


// --- Prefabs (engine/prefab.h): files, instances, overrides, revert / apply / unpack, nesting, placeholders ---

entt::entity childNamed(const kast::Scene& scene, entt::entity parent, const std::string& name)
{
    for (entt::entity child : scene.children(parent))
        if (scene.registry().get<kast::Name>(child).value == name)
            return child;
    return entt::null;
}

// Saves scene, loads it back from the file (prefabs from dir) and returns whether that worked.
bool reloadScene(kast::Scene& scene, const std::filesystem::path& dir, const char* name = "prefab.scene")
{
    std::string error;
    const std::string file = (dir / name).string();
    kast::Scene loaded;
    if (!kast::saveScene(scene, file, error) || !kast::loadScene(file, loaded, error, nullptr, dir)) {
        kast::log::error(std::format("reloadScene: {}", error));
        return false;
    }
    scene = std::move(loaded);
    return true;
}

// Loads a prefab file as a scene, runs edit on it and saves it back (an edit made in prefab edit mode).
template <typename Fn>
bool editPrefab(const std::filesystem::path& dir, const std::string& path, Fn&& edit)
{
    kast::Scene prefab;
    std::string error;
    if (!kast::loadPrefabScene(dir, path, prefab, error))
        return kast::log::error(error), false;
    edit(prefab);
    if (!kast::savePrefab(prefab, dir, path, error))
        return kast::log::error(error), false;
    return true;
}

nlohmann::ordered_json readJson(const std::filesystem::path& file) { return nlohmann::ordered_json::parse(readText(file)); }

// "Orc" (MeshRenderer) with children Club (Collider, Rotator), Hat and Cape, made into prefabs/orc.prefab.
entt::entity makeOrc(kast::Scene& scene, const std::filesystem::path& dir)
{
    entt::registry& registry = scene.registry();
    const entt::entity orc = scene.create("Orc");
    registry.get<kast::Transform>(orc).position = {1, 0, 0};
    registry.emplace<kast::MeshRenderer>(orc);
    const entt::entity club = scene.create("Club", orc);
    registry.emplace<kast::Collider>(club).radius = 0.5f;
    registry.emplace<kast::Rotator>(club);
    scene.create("Hat", orc);
    scene.create("Cape", orc);
    std::string error;
    check(kast::createPrefab(scene, orc, "prefabs/orc.prefab", dir, error), "prefab: create orc.prefab");
    if (!error.empty())
        kast::log::error(error);
    return orc;
}

void checkPrefabFiles()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_files", "Prefabs");
    kast::Scene scene;
    const entt::entity orc = makeOrc(scene, dir);
    entt::registry& registry = scene.registry();
    check(std::filesystem::exists(dir / "prefabs/orc.prefab") && kast::isPrefabRoot(scene, orc) &&
              registry.all_of<kast::PrefabLink>(childNamed(scene, orc, "Club")) &&
              kast::prefabOverrides(scene, orc) == nlohmann::ordered_json::object() &&
              kast::prefabOverrides(scene, childNamed(scene, orc, "Club")) == nlohmann::ordered_json::object(),
          "prefab: Create Prefab writes the file and the source becomes a linked instance with no overrides");
    const nlohmann::ordered_json file = readJson(dir / "prefabs/orc.prefab");
    check(file["version"] == kast::kSceneFileVersion && file.size() == 2 && file["entities"].size() == 4 &&
              file["entities"][0]["name"] == "Orc" && file["entities"][0]["parent"].is_null() &&
              file["entities"][1]["parent"] == file["entities"][0]["id"],
          "prefab: the file holds version and the subtree's entities, root first");

    // Round trip: load -> save -> save is byte-identical, for the prefab and for a scene holding instances.
    kast::Scene prefab;
    std::string error;
    check(kast::loadPrefabScene(dir, "prefabs/orc.prefab", prefab, error) &&
              kast::savePrefab(prefab, dir, "prefabs/orc_copy.prefab", error) &&
              readText(dir / "prefabs/orc_copy.prefab") == readText(dir / "prefabs/orc.prefab"),
          "prefab: a prefab file round trip is byte-identical");
    check(reloadScene(scene, dir, "a.scene") && reloadScene(scene, dir, "b.scene") &&
              readText(dir / "a.scene") == readText(dir / "b.scene") && kast::isPrefabRoot(scene, named(scene, "Orc")),
          "prefab: a scene with an instance round trips byte-identically and stays linked");

    // Bad prefab files.
    writeFile(dir / "prefabs/two_roots.prefab", R"({"version": 5, "entities": [
        {"id": 1, "name": "A", "parent": null, "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}},
        {"id": 2, "name": "B", "parent": null, "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}}]})");
    writeFile(dir / "prefabs/v4.prefab", R"({"version": 4, "entities": []})");
    std::shared_ptr<const kast::PrefabAsset> asset;
    check(!kast::loadPrefab(dir, "prefabs/two_roots.prefab", asset, error) && contains(error, "exactly one root"),
          "prefab: a prefab with two roots is refused");
    check(!kast::loadPrefab(dir, "prefabs/v4.prefab", asset, error) && contains(error, "unsupported \"version\" 4"),
          "prefab: a version 4 prefab is refused");
    check(!kast::loadPrefab(dir, "prefabs/none.prefab", asset, error) && contains(error, "cannot open"),
          "prefab: a missing prefab file fails with why");
    kast::Scene twoRoots;
    twoRoots.create("A");
    twoRoots.create("B");
    check(!kast::savePrefab(twoRoots, dir, "prefabs/x.prefab", error) && contains(error, "exactly one root"),
          "prefab: saving two roots as a prefab is refused");

    // v4 scenes no longer load.
    writeFile(dir / "old.scene", R"({"version": 4, "entities": []})");
    kast::Scene old;
    check(!kast::loadScene((dir / "old.scene").string(), old, error) && contains(error, "unsupported \"version\" 4"),
          "scene v4 is rejected with a clear error");
}

void checkPrefabOverrides()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_overrides", "Prefabs");
    kast::Scene scene;
    makeOrc(scene, dir);
    std::shared_ptr<const kast::PrefabAsset> asset;
    std::string error;
    check(kast::loadPrefab(dir, "prefabs/orc.prefab", asset, error), "prefab: load orc.prefab");

    // Instantiate: a second linked instance with fresh ids, at the prefab root's transform.
    const entt::entity second = kast::instantiatePrefab(scene, asset, entt::null, error);
    entt::registry& registry = scene.registry();
    check(kast::isPrefabRoot(scene, second) && scene.children(second).size() == 3 &&
              idOf(scene, second) != idOf(scene, named(scene, "Orc")) &&
              registry.get<kast::Transform>(second).position.x == 1.0f,
          "prefab: instantiate makes a linked copy with its own ids at the prefab root's transform");
    kast::Scene deep;
    entt::entity bottom = entt::null;
    for (int i = 0; i < kast::kMaxHierarchyDepth - 1; ++i)
        bottom = deep.create("Level", bottom);
    check(kast::instantiatePrefab(deep, asset, bottom, error) == entt::null && contains(error, "deeper than"),
          "prefab: an instance past kMaxHierarchyDepth is refused whole");

    // Overrides on the second instance: a field, an added and a removed component, an added and a deleted child,
    // a renamed child and the root's own transform.
    registry.get<kast::Name>(second).value = "Boss";
    registry.get<kast::Transform>(second).position = {7, 0, 7};
    const entt::entity club = childNamed(scene, second, "Club");
    registry.get<kast::Collider>(club).radius = 0.9f;
    registry.remove<kast::Rotator>(club);
    registry.emplace<kast::Rotator>(childNamed(scene, second, "Hat"));
    registry.get<kast::Name>(childNamed(scene, second, "Hat")).value = "Helmet";
    scene.destroy(childNamed(scene, second, "Cape"));
    scene.create("Extra", second);
    using Json = nlohmann::ordered_json;
    check(kast::prefabOverrides(scene, club) ==
              Json{{"components", {{"Collider", {{"radius", 0.9}}}}}, {"removed", Json::array({"Rotator"})}} &&
              kast::prefabOverrides(scene, second) == Json::object(),
          "prefab: overrides are the changed fields and removed components; the root's name and transform are its own");
    check(kast::deletedPrefabChildren(scene, second).size() == 1 &&
              kast::deletedPrefabChildren(scene, second)[0]->name == "Cape" &&
              kast::addedPrefabChildren(scene, second).size() == 1,
          "prefab: the deleted and the added child are listed");

    // Saved and reloaded, then the prefab changes: overrides stay, the rest follows.
    check(reloadScene(scene, dir), "prefab: save and reload the overrides");
    check(editPrefab(dir, "prefabs/orc.prefab", [](kast::Scene& prefab) {
        const entt::entity root = prefab.children(entt::null).front();
        kast::Collider& collider = prefab.registry().get<kast::Collider>(childNamed(prefab, root, "Club"));
        collider.radius = 0.75f;
        collider.friction = 0.9f;
        prefab.registry().emplace<kast::PointLight>(childNamed(prefab, root, "Hat"));
        prefab.create("Boots", root);
    }), "prefab: change orc.prefab");
    check(reloadScene(scene, dir), "prefab: reload after the prefab changed");
    const entt::entity boss = named(scene, "Boss");
    const entt::entity orc = named(scene, "Orc");
    const entt::entity bossClub = childNamed(scene, boss, "Club");
    const entt::entity helmet = childNamed(scene, boss, "Helmet");
    check(registry.valid(boss) && scene.registry().get<kast::Transform>(boss).position.x == 7.0f &&
              scene.registry().get<kast::Collider>(bossClub).radius == 0.9f &&
              scene.registry().get<kast::Collider>(bossClub).friction == 0.9f &&
              !scene.registry().all_of<kast::Rotator>(bossClub) && scene.registry().all_of<kast::Rotator>(helmet) &&
              scene.registry().all_of<kast::PointLight>(helmet) && childNamed(scene, boss, "Cape") == entt::null &&
              childNamed(scene, boss, "Extra") != entt::null && childNamed(scene, boss, "Boots") != entt::null,
          "prefab: field, component and child overrides survive a prefab change; everything else follows it");
    check(scene.registry().get<kast::Collider>(childNamed(scene, orc, "Club")).radius == 0.75f &&
              childNamed(scene, orc, "Cape") != entt::null && childNamed(scene, orc, "Boots") != entt::null,
          "prefab: an instance without overrides follows the prefab completely");
    check(reloadScene(scene, dir, "c.scene") && reloadScene(scene, dir, "d.scene") &&
              readText(dir / "c.scene") == readText(dir / "d.scene"),
          "prefab: overrides round trip byte-identically");

    // Revert, in memory: field, component (removed and added), deleted child, then everything.
    kast::Scene& s = scene;
    entt::entity root = named(s, "Boss");
    check(kast::revertPrefabField(s, childNamed(s, root, "Club"), "Collider", "radius") &&
              s.registry().get<kast::Collider>(childNamed(s, root, "Club")).radius == 0.75f &&
              !kast::revertPrefabField(s, childNamed(s, root, "Club"), "Collider", "radius"),
          "prefab: Revert field restores the prefab value (and then has nothing left to revert)");
    check(kast::revertPrefabComponent(s, childNamed(s, root, "Club"), "Rotator") &&
              s.registry().all_of<kast::Rotator>(childNamed(s, root, "Club")) &&
              kast::revertPrefabComponent(s, childNamed(s, root, "Helmet"), "Rotator") &&
              !s.registry().all_of<kast::Rotator>(childNamed(s, root, "Helmet")),
          "prefab: Revert component brings a removed component back and takes an added one away");
    const kast::EntityId capeLocal = kast::deletedPrefabChildren(s, root)[0]->id;
    check(kast::revertDeletedChild(s, root, capeLocal, dir, error) &&
              childNamed(s, named(s, "Boss"), "Cape") != entt::null,
          "prefab: Revert brings a deleted child back");
    root = named(s, "Boss");
    check(kast::revertAllOverrides(s, root, dir, error), "prefab: Revert All runs");
    root = named(s, "Boss");
    check(s.valid(root) && s.registry().get<kast::Transform>(root).position.x == 7.0f &&
              childNamed(s, root, "Extra") == entt::null && childNamed(s, root, "Hat") != entt::null &&
              kast::prefabOverrides(s, childNamed(s, root, "Hat")) == Json::object() &&
              kast::addedPrefabChildren(s, root).empty() && kast::deletedPrefabChildren(s, root).empty(),
          "prefab: Revert All drops every override and added child but keeps the root's name and transform");

    // Apply: a field, a component, a deleted child, then everything; the prefab file changes and the overrides go.
    registry.get<kast::Collider>(childNamed(s, root, "Club")).radius = 0.6f;
    check(kast::applyPrefabOverride(s, childNamed(s, root, "Club"), "Collider", "radius", 0, dir, error) &&
              readJson(dir / "prefabs/orc.prefab")["entities"][1]["components"]["Collider"]["radius"] == 0.6 &&
              kast::prefabOverrides(s, childNamed(s, named(s, "Boss"), "Club")) == Json::object() &&
              s.registry().get<kast::Collider>(childNamed(s, named(s, "Orc"), "Club")).radius == 0.6f,
          "prefab: Apply field writes the prefab and every linked instance follows");
    root = named(s, "Boss");
    s.registry().emplace<kast::Camera>(childNamed(s, root, "Hat"));
    check(kast::applyPrefabOverride(s, childNamed(s, root, "Hat"), "Camera", "", 0, dir, error) &&
              s.registry().all_of<kast::Camera>(childNamed(s, named(s, "Orc"), "Hat")),
          "prefab: Apply component adds it to the prefab");
    root = named(s, "Boss");
    s.registry().remove<kast::Camera>(childNamed(s, root, "Hat"));
    check(kast::applyPrefabOverride(s, childNamed(s, root, "Hat"), "Camera", "", 0, dir, error) &&
              !s.registry().all_of<kast::Camera>(childNamed(s, named(s, "Orc"), "Hat")),
          "prefab: Apply of a removed component removes it from the prefab");
    root = named(s, "Boss");
    s.destroy(childNamed(s, root, "Boots"));
    const kast::EntityId bootsLocal = kast::deletedPrefabChildren(s, root)[0]->id;
    check(kast::applyDeletedChild(s, root, bootsLocal, dir, error) &&
              childNamed(s, named(s, "Orc"), "Boots") == entt::null &&
              kast::deletedPrefabChildren(s, named(s, "Boss")).empty(),
          "prefab: Apply of a deleted child removes it from the prefab");
    root = named(s, "Boss");
    s.create("Shield", root);
    s.destroy(childNamed(s, root, "Cape"));
    s.registry().get<kast::Name>(childNamed(s, root, "Hat")).value = "Crown";
    check(kast::applyAllOverrides(s, named(s, "Boss"), dir, error), "prefab: Apply All runs");
    const entt::entity plain = named(s, "Orc");
    root = named(s, "Boss");
    check(childNamed(s, plain, "Shield") != entt::null && childNamed(s, plain, "Cape") == entt::null &&
              childNamed(s, plain, "Crown") != entt::null &&
              s.registry().get<kast::Transform>(plain).position.x == 1.0f &&
              kast::addedPrefabChildren(s, root).empty() && kast::deletedPrefabChildren(s, root).empty() &&
              kast::prefabOverrides(s, childNamed(s, root, "Shield")) == Json::object() &&
              readJson(dir / "prefabs/orc.prefab")["entities"][0]["name"] == "Orc",
          "prefab: Apply All writes added and deleted children and overrides, not the root's own name and transform");
    if (!error.empty())
        kast::log::error(error);

    // Unpack and Duplicate.
    const entt::entity copy = kast::duplicateLinked(s, named(s, "Boss"));
    check(kast::isPrefabRoot(s, copy) && kast::prefabBase(s, childNamed(s, copy, "Club")) != nullptr &&
              idOf(s, copy) != idOf(s, named(s, "Boss")),
          "prefab: Duplicate of an instance is another linked instance");
    const entt::entity duplicatedChild = kast::duplicateLinked(s, childNamed(s, copy, "Club"));
    check(!s.registry().all_of<kast::PrefabLink>(duplicatedChild) && kast::addedPrefabChildren(s, copy).size() == 1,
          "prefab: Duplicate of a prefab child is an added child");
    check(kast::reparentBlocker(s, childNamed(s, copy, "Club")) != nullptr && kast::reparentBlocker(s, copy) == nullptr,
          "prefab: prefab children can't be reparented; the instance root can");
    kast::unpackPrefab(s, copy);
    check(!s.registry().any_of<kast::PrefabLink, kast::PrefabInstance>(copy) &&
              !s.registry().all_of<kast::PrefabLink>(childNamed(s, copy, "Club")) &&
              s.registry().get<kast::Collider>(childNamed(s, copy, "Club")).radius == 0.6f,
          "prefab: Unpack leaves plain entities with their current values");
    check(reloadScene(s, dir, "e.scene") && reloadScene(s, dir, "f.scene") &&
              readText(dir / "e.scene") == readText(dir / "f.scene"),
          "prefab: the scene round trips byte-identically after the edits");
}

void checkPrefabNesting()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_nesting", "Prefabs");
    std::string error;
    // gem.prefab (a PointLight), then chest.prefab holding an instance of it.
    kast::Scene scene;
    const entt::entity gem = scene.create("Gem");
    scene.registry().emplace<kast::PointLight>(gem);
    check(kast::createPrefab(scene, gem, "prefabs/gem.prefab", dir, error), "prefab nesting: create gem.prefab");
    const entt::entity chest = scene.create("Chest");
    scene.registry().emplace<kast::MeshRenderer>(chest);
    scene.reparent(gem, chest);
    check(kast::createPrefab(scene, chest, "prefabs/chest.prefab", dir, error), "prefab nesting: create chest.prefab");
    const nlohmann::ordered_json file = readJson(dir / "prefabs/chest.prefab");
    check(file["entities"].size() == 2 && file["entities"][1]["prefab"] == "prefabs/gem.prefab",
          "prefab nesting: the outer prefab stores the inner one as an instance");
    check(reloadScene(scene, dir), "prefab nesting: reload");
    const entt::entity inner = childNamed(scene, named(scene, "Chest"), "Gem");
    check(kast::prefabChain(scene, inner) == std::vector<std::string>{"prefabs/chest.prefab", "prefabs/gem.prefab"} &&
              kast::prefabRoot(scene, inner) == named(scene, "Chest") &&
              scene.registry().all_of<kast::PrefabInstance>(inner),
          "prefab nesting: an inner entity belongs to both prefabs, outermost first");

    // Apply to the inner prefab: gem.prefab changes, chest.prefab keeps no override for it.
    scene.registry().get<kast::PointLight>(inner).range = 4.0f;
    check(kast::applyPrefabOverride(scene, inner, "PointLight", "range", 1, dir, error), "prefab nesting: Apply to inner");
    check(readJson(dir / "prefabs/gem.prefab")["entities"][0]["components"]["PointLight"]["range"] == 4.0 &&
              !contains(readText(dir / "prefabs/chest.prefab"), "\"range\""),
          "prefab nesting: Apply to Inner.prefab writes it and clears the outer override");
    // Apply to the outer prefab: chest.prefab gets the override, gem.prefab is unchanged.
    scene.registry().get<kast::PointLight>(childNamed(scene, named(scene, "Chest"), "Gem")).intensity = 3.0f;
    check(kast::applyPrefabOverride(scene, childNamed(scene, named(scene, "Chest"), "Gem"), "PointLight", "intensity", 0,
                                    dir, error) &&
              readJson(dir / "prefabs/gem.prefab")["entities"][0]["components"]["PointLight"]["intensity"] == 1.0 &&
              contains(readText(dir / "prefabs/chest.prefab"), "\"intensity\": 3.0"),
          "prefab nesting: Apply to Outer.prefab keeps the change as the outer prefab's override");

    // An inner prefab change reaches instances of the outer one.
    check(editPrefab(dir, "prefabs/gem.prefab", [](kast::Scene& prefab) {
        prefab.registry().get<kast::PointLight>(prefab.children(entt::null).front()).color = {1, 0, 0};
    }), "prefab nesting: change gem.prefab");
    kast::Scene other;
    std::shared_ptr<const kast::PrefabAsset> chestAsset;
    check(kast::loadPrefab(dir, "prefabs/chest.prefab", chestAsset, error), "prefab nesting: load chest.prefab");
    const entt::entity placed = kast::instantiatePrefab(other, chestAsset, entt::null, error);
    const kast::PointLight& light = other.registry().get<kast::PointLight>(other.children(placed).front());
    check(light.color.y == 0.0f && light.intensity == 3.0f && light.range == 4.0f,
          "prefab nesting: the inner prefab's change shows through the outer prefab, under its override");

    // Unpacking the outer instance keeps the inner one linked, with the outer override as its own.
    check(reloadScene(scene, dir), "prefab nesting: reload before unpack");
    kast::unpackPrefab(scene, named(scene, "Chest"));
    const entt::entity loose = childNamed(scene, named(scene, "Chest"), "Gem");
    check(!scene.registry().all_of<kast::PrefabInstance>(named(scene, "Chest")) && kast::isPrefabRoot(scene, loose) &&
              kast::prefabOverrides(scene, loose) == nlohmann::ordered_json{{"components", {{"PointLight", {{"intensity", 3.0}}}}}} &&
              scene.registry().get<kast::PointLight>(loose).intensity == 3.0f,
          "prefab nesting: Unpack leaves nested instances linked");
    check(reloadScene(scene, dir) && scene.registry().get<kast::PointLight>(childNamed(scene, named(scene, "Chest"), "Gem"))
                                             .intensity == 3.0f,
          "prefab nesting: the unpacked scene saves the inner instance with its override");

    // Cycles: saving gem.prefab with a chest inside, or making a prefab that holds an instance of its own path.
    kast::Scene gemScene;
    check(kast::loadPrefabScene(dir, "prefabs/gem.prefab", gemScene, error), "prefab nesting: open gem.prefab");
    kast::instantiatePrefab(gemScene, chestAsset, gemScene.children(entt::null).front(), error);
    check(!kast::savePrefab(gemScene, dir, "prefabs/gem.prefab", error) && contains(error, "can't contain itself"),
          "prefab nesting: a prefab that would contain itself through nesting is refused with an error");
    // Hand-made cycle a -> b -> a: loading stops at the repeat with a placeholder and a warning.
    writeFile(dir / "prefabs/a.prefab", R"({"version": 5, "entities": [
        {"id": 1, "name": "A", "parent": null, "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}},
        {"id": 2, "name": "B", "parent": 1, "prefab": "prefabs/b.prefab", "instance": []}]})");
    writeFile(dir / "prefabs/b.prefab", R"({"version": 5, "entities": [
        {"id": 1, "name": "B", "parent": null, "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}},
        {"id": 2, "name": "A", "parent": 1, "prefab": "prefabs/a.prefab", "instance": []}]})");
    std::shared_ptr<const kast::PrefabAsset> cyclic;
    kast::SceneLoadReport report;
    check(kast::loadPrefab(dir, "prefabs/a.prefab", cyclic, error, &report) && report.warnings.size() == 1 &&
              contains(report.warnings[0], "would contain itself") && cyclic->entities.size() == 3 &&
              cyclic->entities[2].missingPrefab == "prefabs/a.prefab",
          "prefab nesting: a cycle in the files loads with the repeat as a placeholder and a warning");
}

void checkMissingPrefabs()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_missing", "Prefabs");
    writeFile(dir / "prefabs/broken.prefab", "{ not json");
    const std::string record = R"({
      "id": 5,
      "name": "Ghost",
      "parent": null,
      "prefab": "prefabs/gone.prefab",
      "instance": [
        {
          "source": 1,
          "id": 5,
          "components": {
            "Transform": {
              "position": [
                2.0,
                0.0,
                3.0
              ],
              "rotation": [
                0.0,
                0.0,
                0.0,
                1.0
              ],
              "scale": [
                1.0,
                1.0,
                1.0
              ]
            }
          }
        },
        {
          "source": 2,
          "id": 6,
          "components": {
            "Collider": {
              "radius": 0.85
            }
          }
        },
        {
          "source": 3,
          "deleted": true
        }
      ]
    })";
    writeFile(dir / "missing.scene", std::format(R"({{"version": 5, "entities": [{}, {}, {}]}})", record,
        R"({"id": 7, "name": "Under Child", "parent": 6, "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}})",
        R"({"id": 8, "name": "Broken", "parent": null, "prefab": "prefabs/broken.prefab", "instance": []})"));
    kast::Scene scene;
    kast::SceneLoadReport report;
    std::string error;
    check(kast::loadScene((dir / "missing.scene").string(), scene, error, &report, dir), "prefab missing: the scene loads");
    const entt::entity ghost = named(scene, "gone");
    check(kast::isMissingPrefab(scene, ghost) && kast::isMissingPrefab(scene, named(scene, "broken")) &&
              scene.registry().get<kast::Transform>(ghost).position.z == 3.0f &&
              scene.parent(named(scene, "Under Child")) == ghost,
          "prefab missing: placeholders named after the prefab, at their saved position, keeping children");
    check(report.warnings.size() == 2 && contains(report.warnings[0], "Entity 'Ghost' (id 5)") &&
              contains(report.warnings[0], "prefabs/gone.prefab") && contains(report.warnings[1], "prefabs/broken.prefab"),
          "prefab missing: a warning per placeholder names the entity and the path");
    check(kast::saveScene(scene, (dir / "out.scene").string(), error), "prefab missing: save");
    const nlohmann::ordered_json saved = readJson(dir / "out.scene");
    check(saved["entities"][0] == nlohmann::ordered_json::parse(record) && saved["entities"][1]["parent"] == 6,
          "prefab missing: saving keeps the reference, the overrides and the child's parent unchanged");
    kast::Scene again;
    check(kast::loadScene((dir / "out.scene").string(), again, error, nullptr, dir) &&
              kast::saveScene(again, (dir / "out2.scene").string(), error) &&
              readText(dir / "out.scene") == readText(dir / "out2.scene"),
          "prefab missing: placeholders round trip byte-identically");
    // Once the prefab exists, the instance comes back with its overrides.
    kast::Scene source;
    const entt::entity root = source.create("Gone");
    source.registry().emplace<kast::Collider>(source.create("Inner", root));
    source.create("Deleted", root);
    check(kast::savePrefab(source, dir, "prefabs/gone.prefab", error), "prefab missing: write the prefab");
    kast::Scene found;
    check(kast::loadScene((dir / "out.scene").string(), found, error, nullptr, dir) &&
              kast::isPrefabRoot(found, named(found, "Ghost")) &&
              found.registry().get<kast::Collider>(named(found, "Inner")).radius == 0.85f &&
              named(found, "Deleted") == entt::null && found.parent(named(found, "Under Child")) == named(found, "Inner"),
          "prefab missing: when the prefab is back the instance loads with every override");
}

void checkPrefabRemap()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_remap", "Prefabs");
    kast::Scene scene;
    makeOrc(scene, dir);
    std::string error;
    std::filesystem::create_directories(dir / "scenes");
    check(kast::saveScene(scene, (dir / "scenes/one.scene").string(), error) &&
              kast::saveScene(scene, (dir / "two.scene").string(), error),
          "prefab remap: write two scenes using orc.prefab");
    // A prefab nesting orc.prefab.
    kast::Scene holder;
    const entt::entity pack = holder.create("Pack");
    std::shared_ptr<const kast::PrefabAsset> orc;
    check(kast::loadPrefab(dir, "prefabs/orc.prefab", orc, error), "prefab remap: load orc.prefab");
    kast::instantiatePrefab(holder, orc, pack, error);
    check(kast::createPrefab(holder, pack, "pack.prefab", dir, error), "prefab remap: pack.prefab nests orc.prefab");
    check((prefabUsers(dir, "prefabs/orc.prefab") == std::vector<std::string>{"pack.prefab", "scenes/one.scene", "two.scene"}) &&
              (prefabUsers(dir, "prefabs") == prefabUsers(dir, "prefabs/orc.prefab")) && prefabUsers(dir, "pack.prefab").empty(),
          "prefab remap: the users of a prefab (or a folder of them) are listed");

    // Move the folder, as the Assets panel does: the files on disk and the open scene follow.
    Document doc;
    check(openFile(doc, (dir / "two.scene").string(), error), "prefab remap: open two.scene");
    std::filesystem::create_directories(dir / "assets");
    std::filesystem::rename(dir / "prefabs", dir / "assets/prefabs");
    const std::vector<std::string> changed = remapPrefabReferences(dir, "prefabs", "assets/prefabs");
    remapScenePaths(doc, "prefabs", "assets/prefabs");
    refreshPrefabs(doc);
    check((changed == std::vector<std::string>{"pack.prefab", "scenes/one.scene", "two.scene"}) && doc.dirty &&
              kast::isPrefabRoot(doc.scene, named(doc.scene, "Orc")) &&
              doc.scene.registry().get<kast::PrefabInstance>(named(doc.scene, "Orc")).path == "assets/prefabs/orc.prefab",
          "prefab remap: a move rewrites every scene and prefab on disk and the open scene");
    kast::Scene moved;
    check(kast::loadScene((dir / "scenes/one.scene").string(), moved, error, nullptr, dir) &&
              kast::isPrefabRoot(moved, named(moved, "Orc")) && contains(readText(dir / "pack.prefab"), "assets/prefabs/orc.prefab"),
          "prefab remap: the moved references load");
    // Rename the prefab itself.
    std::filesystem::rename(dir / "assets/prefabs/orc.prefab", dir / "assets/prefabs/brute.prefab");
    remapPrefabReferences(dir, "assets/prefabs/orc.prefab", "assets/prefabs/brute.prefab");
    remapScenePaths(doc, "assets/prefabs/orc.prefab", "assets/prefabs/brute.prefab");
    refreshPrefabs(doc);
    check(kast::isPrefabRoot(doc.scene, named(doc.scene, "Orc")) && contains(readText(dir / "two.scene"), "brute.prefab") &&
              (prefabUsers(dir, "assets/prefabs/brute.prefab").size() == 3),
          "prefab remap: a rename rewrites the references too");
    check(uniquePrefabName(dir / "assets/prefabs", "brute") == "brute_2.prefab" &&
              uniquePrefabName(dir, "Big: Orc?") == "Big_ Orc_.prefab" && uniquePrefabName(dir, "...") == "Prefab.prefab",
          "prefab remap: new prefab names are unique and file-safe");
}

void checkPrefabEditMode()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_edit", "Prefabs");
    {
        kast::Scene scene;
        const entt::entity orc = makeOrc(scene, dir);
        scene.registry().get<kast::Collider>(childNamed(scene, orc, "Club")).radius = 0.95f; // an override
        std::string error;
        check(kast::saveScene(scene, (dir / "main.scene").string(), error), "prefab edit: write main.scene");
    }
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "main.scene").string(), error), "prefab edit: open main.scene");
    doc.scene.registry().get<kast::Name>(named(doc.scene, "Orc")).value = "Grunt"; // unsaved scene change
    markDirty(doc);
    check(openPrefab(doc, "prefabs/orc.prefab", error) && doc.prefabEdit && !doc.dirty &&
              doc.scene.children(entt::null).size() == 1 && breadcrumb(doc) == "main > orc.prefab" &&
              windowTitle(doc) == "Kast Editor - Prefabs - orc.prefab",
          "prefab edit: the prefab replaces the scene, under a breadcrumb");
    startPlay(doc);
    check(doc.play == PlayState::Stopped, "prefab edit: Play is disabled");
    const entt::entity root = doc.scene.children(entt::null).front();
    doc.scene.registry().get<kast::Collider>(childNamed(doc.scene, root, "Club")).friction = 0.25f;
    doc.scene.create("Horn", root);
    markDirty(doc);
    check(saveFile(doc, doc.path, error) && !doc.dirty && contains(readText(dir / "prefabs/orc.prefab"), "Horn"),
          "prefab edit: Save writes the .prefab");
    closePrefab(doc);
    const entt::entity grunt = named(doc.scene, "Grunt");
    check(!doc.prefabEdit && doc.dirty && grunt != entt::null && childNamed(doc.scene, grunt, "Horn") != entt::null &&
              doc.scene.registry().get<kast::Collider>(childNamed(doc.scene, grunt, "Club")).friction == 0.25f &&
              doc.scene.registry().get<kast::Collider>(childNamed(doc.scene, grunt, "Club")).radius == 0.95f,
          "prefab edit: Back restores the scene (unsaved changes kept) with instances refreshed, overrides kept");
    // A drop that would put the edited prefab inside itself is refused.
    check(openPrefab(doc, "prefabs/orc.prefab", error), "prefab edit: open again");
    check(dropPrefab(doc, "prefabs/orc.prefab", entt::null, error) == entt::null && contains(error, "can't contain itself"),
          "prefab edit: dropping a prefab into its own edit mode is refused");
    doc.scene.create("Unsaved", doc.scene.children(entt::null).front());
    closePrefab(doc); // discards
    check(!contains(readText(dir / "prefabs/orc.prefab"), "Unsaved") &&
              childNamed(doc.scene, named(doc.scene, "Grunt"), "Unsaved") == entt::null,
          "prefab edit: leaving without saving discards the prefab changes");
    // Create Prefab and the editor-side ops by id.
    std::string path;
    const entt::entity lone = doc.scene.create("Lone Wolf");
    check(createPrefabFrom(doc, lone, "", path, error) && path == "Lone Wolf.prefab" && kast::isPrefabRoot(doc.scene, lone),
          "prefab edit: Create Prefab saves <name>.prefab in the folder");
    check(!createPrefabFrom(doc, childNamed(doc.scene, named(doc.scene, "Grunt"), "Club"), "", path, error) &&
              contains(error, "already part of a prefab instance"),
          "prefab edit: Create Prefab on a prefab child is refused");
    PrefabOp op{PrefabOp::Kind::RevertField, idOf(doc.scene, childNamed(doc.scene, named(doc.scene, "Grunt"), "Club")),
                "Collider", "radius"};
    check(runPrefabOp(doc, op, error) &&
              doc.scene.registry().get<kast::Collider>(childNamed(doc.scene, named(doc.scene, "Grunt"), "Club")).radius == 0.5f,
          "prefab edit: runPrefabOp reverts by id");
}

// --- Undo / redo (editor/undo.h, editor/scene_undo.h) ---

void checkUndoStack()
{
    UndoStack stack;
    int value = 0;
    const auto step = [&](int to) {
        const int from = value;
        value = to;
        stack.push({std::format("Set {}", to), [&value, from] { value = from; }, [&value, to] { value = to; }});
    };
    check(!stack.canUndo() && !stack.canRedo() && stack.undoName().empty() && stack.atSavePoint(),
          "undo stack: empty, at its save point");
    step(1);
    step(2);
    check(stack.canUndo() && stack.undoName() == "Set 2" && !stack.canRedo() && !stack.atSavePoint(),
          "undo stack: push records a step");
    stack.undo();
    check(value == 1 && stack.redoName() == "Set 2" && stack.undoName() == "Set 1", "undo stack: undo runs the step's undo");
    stack.redo();
    check(value == 2 && !stack.canRedo(), "undo stack: redo runs the step's redo");
    stack.undo();
    stack.undo();
    check(value == 0 && stack.atSavePoint(), "undo stack: undo back to the save point");
    stack.redo();
    stack.markSaved();
    step(5);
    stack.undo();
    check(stack.atSavePoint() && value == 1, "undo stack: the save point after a save");
    step(7);
    check(!stack.canRedo() && stack.size() == 2 && value == 7, "undo stack: a new step discards the redo branch");
    stack.undo();
    check(stack.atSavePoint(), "undo stack: the save point before the discarded branch is kept");
    stack.redo();
    stack.markSaved();
    stack.undo();
    step(9); // the save point (after Set 7) was in the discarded branch
    stack.undo();
    stack.undo();
    stack.redo();
    stack.redo();
    check(!stack.atSavePoint() && value == 9, "undo stack: a discarded save point is unreachable");

    stack.clear(true);
    for (int i = 1; i <= 101; ++i)
        step(i);
    check(stack.size() == UndoStack::kMaxSteps && stack.undoName() == "Set 101", "undo stack: capped at 100 steps");
    while (stack.canUndo())
        stack.undo();
    check(value == 1 && !stack.atSavePoint(), "undo stack: the 101st push dropped the oldest (and the save point)");
    stack.clear(false);
    check(!stack.atSavePoint() && stack.size() == 0, "undo stack: clear(false) leaves no save point");

    UndoTarget target{.history = &stack, .undo = [] {}, .redo = [] {}};
    step(3);
    check(target.canUndo() && !target.canRedo(), "undo target: follows its history");
    target.blocked = true;
    check(!target.canUndo() && !target.canRedo(), "undo target: blocked (Play) can't undo or redo");
}

kast::EntityId selectedId(const Document& doc)
{
    return doc.scene.valid(doc.selected) ? doc.scene.registry().get<kast::PersistentId>(doc.selected).value : 0;
}

// edit makes exactly one step: undo puts the scene back as it was before (serialized form), redo as it was after.
template <typename Fn>
void checkUndoRedo(Document& doc, const char* what, Fn&& edit)
{
    const nlohmann::ordered_json before = kast::sceneToJson(doc.scene);
    const size_t steps = doc.history.size();
    edit();
    const nlohmann::ordered_json after = kast::sceneToJson(doc.scene);
    const std::string name = std::format("scene undo: {}", what);
    check(after != before, (name + ": changes the scene").c_str());
    // One undo getting back to before (below) proves it is one step.
    check(doc.history.size() <= steps + 1 && doc.history.canUndo() && !doc.history.canRedo(),
          (name + ": records a step").c_str());
    undoScene(doc);
    check(kast::sceneToJson(doc.scene) == before, (name + ": undo restores the exact prior state").c_str());
    redoScene(doc);
    check(kast::sceneToJson(doc.scene) == after, (name + ": redo restores the exact post state").c_str());
}

void checkSceneUndo()
{
    const std::filesystem::path dir = projectTestDir("kast_scene_undo", "Undo");
    Document doc;
    resetToDefault(doc); // light, ground, cube, camera
    doc.project = kast::Project{.root = dir, .name = "Undo"};
    check(doc.history.size() == 0 && !doc.dirty && doc.history.atSavePoint(), "scene undo: New starts an empty history");
    const auto id = [&](const char* name) { return idOf(doc.scene, named(doc.scene, name)); };
    const auto entity = [&](kast::EntityId id) { return kast::findById(doc.scene, id); };
    const kast::EntityId cube = id("Cube"), ground = id("Ground");

    checkUndoRedo(doc, "create", [&] { createObject(doc, ObjectKind::Sphere, entt::null); });
    check(doc.history.undoName() == "Create Sphere", "scene undo: the step is named after the edit");
    const kast::EntityId sphere = id("Sphere");
    undoScene(doc);
    check(doc.selected == entt::null, "scene undo: undoing a create clears the selection (the entity is gone)");
    redoScene(doc);
    check(selectedId(doc) == sphere, "scene undo: redoing a create selects it again, by id");

    checkUndoRedo(doc, "create model", [&] { createModelEntity(doc, "models/none.glb", entity(sphere)); });

    // Delete Ground (the second root, with a child): undo puts it back in place, with its ids.
    doc.scene.create("Pebble", entity(ground));
    doc.selected = entity(ground);
    checkUndoRedo(doc, "delete", [&] { deleteSelected(doc); });
    check(doc.selected == entt::null, "scene undo: redo of a delete leaves nothing selected");
    undoScene(doc);
    check(selectedId(doc) == ground && doc.scene.children(entt::null)[1] == doc.selected &&
              childNamed(doc.scene, doc.selected, "Pebble") != entt::null,
          "scene undo: undoing a delete reselects the entity, at its old place with its children");

    doc.selected = entity(ground);
    checkUndoRedo(doc, "duplicate", [&] { duplicateSelected(doc); });

    // Reparent (Hierarchy drag) keeps the world transform: undo puts back the parent, place and local transform.
    checkUndoRedo(doc, "reparent", [&] {
        beginSceneEdit(doc, "Move Cube", {.entities = {cube}, .deep = true});
        doc.scene.reparent(entity(cube), entity(id("Main Camera")));
        markDirty(doc);
        endSceneEdit(doc);
    });
    checkUndoRedo(doc, "rename", [&] { // Hierarchy F2
        beginSceneEdit(doc, "Rename Cube", {.entities = {cube}});
        doc.scene.registry().get<kast::Name>(entity(cube)).value = "Box";
        markDirty(doc);
        endSceneEdit(doc);
    });

    // An Inspector field (or gizmo) drag over several frames is one step, closed once it lets go.
    const auto frame = [&](const char* name, const SceneScope& scope, auto&& change) {
        const SceneState before = doc.recording ? SceneState() : captureScene(doc.scene, scope);
        change();
        sceneGesture(doc, name, scope, before);
        markDirty(doc);
    };
    checkUndoRedo(doc, "inspector drag", [&] {
        for (int i = 1; i <= 5; ++i)
            frame("Edit Box", {.entities = {cube}},
                  [&] { doc.scene.registry().get<kast::Transform>(entity(cube)).position.y += 0.25f; });
        check(doc.history.undoName() != "Edit Box", "scene undo: no step while the drag is on");
        endSceneGesture(doc);
    });
    check(doc.history.undoName() == "Edit Box", "scene undo: the drag is one step once it lets go");
    checkUndoRedo(doc, "add component", [&] {
        frame("Add Rotator", {.entities = {cube}}, [&] { doc.scene.registry().emplace<kast::Rotator>(entity(cube)); });
        endSceneGesture(doc);
    });
    checkUndoRedo(doc, "remove component", [&] {
        frame("Remove Mesh Renderer", {.entities = {cube}},
              [&] { doc.scene.registry().remove<kast::MeshRenderer>(entity(cube)); });
        endSceneGesture(doc);
    });
    doc.selected = entt::null;
    undoScene(doc);
    check(selectedId(doc) == cube && doc.scene.registry().all_of<kast::MeshRenderer>(doc.selected),
          "scene undo: undoing an Inspector edit reselects its entity");
    redoScene(doc);
    checkUndoRedo(doc, "gizmo drag", [&] {
        for (int i = 1; i <= 3; ++i)
            frame("Rotate Box", {.entities = {cube}}, [&] {
                doc.scene.registry().get<kast::Transform>(entity(cube)).rotation = kast::quatFromEulerDeg({0.0f, 10.0f * i, 0.0f});
            });
        endSceneGesture(doc);
    });

    // Scene settings (Lighting, Post Processing, Physics panels) and the Lua systems list.
    checkUndoRedo(doc, "lighting", [&] {
        frame("Edit Lighting", {.settings = true}, [&] { doc.scene.lighting.ambientIntensity = 0.7f; });
        frame("Edit Lighting", {.settings = true}, [&] { doc.scene.lighting.ambientIntensity = 0.8f; });
        endSceneGesture(doc);
    });
    std::string error;
    writeFile(dir / "scripts/a.lua", "return { update = function() end }");
    writeFile(dir / "scripts/b.lua", "return { update = function() end }");
    checkUndoRedo(doc, "add system", [&] { addSystem(doc, dir / "scripts/a.lua", error); });
    addSystem(doc, dir / "scripts/b.lua", error);
    checkUndoRedo(doc, "move system", [&] { moveSystem(doc, 1, -1); });
    checkUndoRedo(doc, "remove system", [&] { removeSystem(doc, 0); });

    // Edits that change nothing record nothing.
    size_t steps = doc.history.size();
    moveSystem(doc, 0, -1); // already at the top
    frame("Edit Box", {.entities = {cube}}, [] {});
    endSceneGesture(doc);
    beginSceneEdit(doc, "Rename Box", {.entities = {cube}});
    endSceneEdit(doc);
    doc.selected = entt::null;
    deleteSelected(doc);
    check(doc.history.size() == steps, "scene undo: no step for edits that change nothing");

    // Save point: undo back to it clears dirty ("*" goes), redo away sets it again.
    check(saveFile(doc, (dir / "undo.scene").string(), error) && !doc.dirty, "scene undo: save");
    createObject(doc, ObjectKind::Cube, entt::null);
    check(doc.dirty && contains(windowTitle(doc), "*"), "scene undo: an edit after saving is dirty");
    undoScene(doc);
    check(!doc.dirty && !contains(windowTitle(doc), "*"), "scene undo: undo back to the save point is clean");
    undoScene(doc);
    check(doc.dirty, "scene undo: undo past the save point is dirty");
    redoScene(doc);
    check(!doc.dirty, "scene undo: redo back to the save point is clean");
    redoScene(doc);
    check(doc.dirty, "scene undo: redo away from the save point is dirty");
    undoScene(doc);
    createObject(doc, ObjectKind::Plane, entt::null);
    check(doc.dirty && !doc.history.canRedo(), "scene undo: a new edit after undo discards the redo branch");
    undoScene(doc);
    check(!doc.dirty, "scene undo: the save point before the discarded branch is still reachable");
    doc.scene.registry().get<kast::Name>(entity(ground)).value = "Renamed by a script"; // an edit that is no step
    markDirty(doc);
    redoScene(doc);
    undoScene(doc);
    check(doc.dirty, "scene undo: after an edit that is no step, undo never gets back to clean");

    for (int i = 0; i < 101; ++i)
        createObject(doc, ObjectKind::Empty, entt::null);
    check(doc.history.size() == UndoStack::kMaxSteps, "scene undo: capped at 100 steps");

    // Play: undo / redo blocked, Play edits never recorded, history intact and usable after Stop.
    steps = doc.history.size();
    const nlohmann::ordered_json edited = kast::sceneToJson(doc.scene);
    const kast::EntityId last = selectedId(doc);
    startPlay(doc);
    undoScene(doc);
    createObject(doc, ObjectKind::Cube, entt::null);
    check(doc.history.size() == steps && !doc.history.canRedo(), "scene undo: Play blocks undo and records nothing");
    stopPlay(doc);
    check(kast::sceneToJson(doc.scene) == edited, "scene undo: Stop restores the scene the history matches");
    undoScene(doc);
    check(doc.history.canRedo() && entity(last) == entt::null && doc.selected == entt::null,
          "scene undo: undo works after Stop");

    // Cleared by New, Open and the ops that write files.
    resetToDefault(doc);
    check(doc.history.size() == 0 && !doc.recording, "scene undo: New clears the history");
    createObject(doc, ObjectKind::Cube, entt::null);
    check(openFile(doc, (dir / "undo.scene").string(), error) && doc.history.size() == 0 && doc.history.atSavePoint(),
          "scene undo: Open clears the history");
    createObject(doc, ObjectKind::Cube, entt::null);
    remapScenePaths(doc, "scripts/nothing.lua", "scripts/other.lua");
    check(doc.history.size() == 1, "scene undo: a remap that changes nothing keeps the history");
    remapScenePaths(doc, "scripts/a.lua", "scripts/c.lua");
    check(doc.history.size() == 0 && doc.dirty && !doc.history.atSavePoint(),
          "scene undo: an asset rename / move that remaps the scene clears it");
    createObject(doc, ObjectKind::Cube, entt::null);
    check(newLuaComponent(doc, doc.selected, "UndoTag", error) && doc.history.size() == 0,
          "scene undo: New Lua Component clears the history");
    createObject(doc, ObjectKind::Cube, entt::null);
    writeFile(dir / "components/undo_tag.lua", R"(return { name = "UndoTag", fields = { { name = "n", type = "int" } } })");
    refreshComponents(doc);
    check(doc.history.size() == 0, "scene undo: a Lua component hot reload that migrates instances clears the history");
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

void checkPrefabUndo()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_undo", "Prefabs");
    {
        kast::Scene scene;
        const entt::entity orc = makeOrc(scene, dir);
        scene.registry().get<kast::Collider>(childNamed(scene, orc, "Club")).radius = 0.95f; // an override
        scene.destroy(childNamed(scene, orc, "Cape"));                                      // a deleted child
        std::string error;
        check(kast::saveScene(scene, (dir / "main.scene").string(), error), "prefab undo: write main.scene");
    }
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "main.scene").string(), error), "prefab undo: open main.scene");
    const kast::EntityId orc = idOf(doc.scene, named(doc.scene, "Orc"));
    const kast::EntityId club = idOf(doc.scene, named(doc.scene, "Club"));
    using Kind = PrefabOp::Kind;
    checkUndoRedo(doc, "prefab revert field", [&] {
        runPrefabOp(doc, {.kind = Kind::RevertField, .entity = club, .component = "Collider", .field = "radius"}, error);
    });
    checkUndoRedo(doc, "prefab revert all", [&] { runPrefabOp(doc, {.kind = Kind::RevertAll, .entity = orc}, error); });
    checkUndoRedo(doc, "prefab unpack", [&] { runPrefabOp(doc, {.kind = Kind::Unpack, .entity = orc}, error); });
    checkUndoRedo(doc, "prefab drop", [&] { dropPrefab(doc, "prefabs/orc.prefab", entt::null, error); });
    undoScene(doc);
    undoScene(doc);
    check(kast::isPrefabRoot(doc.scene, kast::findById(doc.scene, orc)) && selectedId(doc) == orc,
          "prefab undo: undoing Unpack links the instance again and selects it");

    // Prefab edit mode has its own history; the scene's waits and comes back.
    const size_t sceneSteps = doc.history.size();
    check(sceneSteps > 0 && openPrefab(doc, "prefabs/orc.prefab", error) && doc.history.size() == 0,
          "prefab undo: opening a prefab starts its own empty history");
    createObject(doc, ObjectKind::Cube, doc.scene.children(entt::null).front());
    check(doc.history.size() == 1, "prefab undo: prefab edits are recorded on the prefab's history");
    closePrefab(doc);
    check(doc.history.size() == sceneSteps, "prefab undo: Back restores the scene's history");

    // Apply and Create Prefab write files: they clear it.
    selectById(doc, club);
    doc.scene.registry().get<kast::Collider>(doc.selected).friction = 0.1f;
    check(runPrefabOp(doc, {.kind = Kind::ApplyField, .entity = club, .component = "Collider", .field = "friction"}, error) &&
              doc.history.size() == 0,
          "prefab undo: Apply clears the history");
    createObject(doc, ObjectKind::Cube, entt::null);
    std::string path;
    check(createPrefabFrom(doc, doc.selected, "prefabs", path, error) && doc.history.size() == 0,
          "prefab undo: Create Prefab clears the history");
}

// Prefab edit mode: every scene edit on the prefab's own history, the scene's waiting behind it untouched.
void checkPrefabModeUndo()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_mode_undo", "Prefabs");
    std::string error;
    {
        kast::Scene scene;
        const entt::entity orc = makeOrc(scene, dir);
        scene.registry().get<kast::Collider>(childNamed(scene, orc, "Club")).radius = 0.95f; // an override
        const entt::entity gem = scene.create("Gem");
        scene.registry().emplace<kast::Collider>(scene.create("Shard", gem)).radius = 0.2f;
        check(kast::createPrefab(scene, gem, "prefabs/gem.prefab", dir, error), "prefab mode undo: create gem.prefab");
        scene.destroy(gem);
        check(kast::saveScene(scene, (dir / "main.scene").string(), error), "prefab mode undo: write main.scene");
    }
    Document doc;
    check(openFile(doc, (dir / "main.scene").string(), error), "prefab mode undo: open main.scene");
    const kast::EntityId orc = idOf(doc.scene, named(doc.scene, "Orc"));
    createObject(doc, ObjectKind::Cube, entt::null);
    doc.selected = kast::findById(doc.scene, orc);
    deleteSelected(doc); // the scene's 2 steps; the delete is undone after the prefab is saved below
    const nlohmann::ordered_json scene = kast::sceneToJson(doc.scene);

    check(openPrefab(doc, "prefabs/orc.prefab", error) && doc.history.size() == 0 && !doc.dirty &&
              doc.prefabEdit->history.size() == 2,
          "prefab mode undo: opening a prefab starts its own empty history, the scene's waits");
    const nlohmann::ordered_json opened = kast::sceneToJson(doc.scene);
    const auto id = [&](const char* name) { return idOf(doc.scene, named(doc.scene, name)); };
    const auto entity = [&](kast::EntityId id) { return kast::findById(doc.scene, id); };
    const kast::EntityId root = id("Orc"), club = id("Club"), hat = id("Hat");
    startPlay(doc);
    check(doc.play == PlayState::Stopped && doc.history.size() == 0, "prefab mode undo: no Play, nothing recorded");

    // The scene edits, on the prefab.
    checkUndoRedo(doc, "prefab mode: create", [&] { createObject(doc, ObjectKind::Sphere, entity(root)); });
    checkUndoRedo(doc, "prefab mode: create model", [&] { createModelEntity(doc, "models/none.glb", entity(root)); });
    doc.selected = entity(hat);
    checkUndoRedo(doc, "prefab mode: delete", [&] { deleteSelected(doc); });
    undoScene(doc);
    check(selectedId(doc) == hat, "prefab mode undo: undoing a delete reselects it by id");
    redoScene(doc);
    check(doc.selected == entt::null, "prefab mode undo: redoing a delete clears the selection");
    doc.selected = entity(club);
    checkUndoRedo(doc, "prefab mode: duplicate", [&] { duplicateSelected(doc); });
    checkUndoRedo(doc, "prefab mode: reparent", [&] {
        beginSceneEdit(doc, "Move Club", {.entities = {club}, .deep = true});
        doc.scene.reparent(entity(club), entity(id("Cape")));
        markDirty(doc);
        endSceneEdit(doc);
    });
    checkUndoRedo(doc, "prefab mode: rename", [&] {
        beginSceneEdit(doc, "Rename Club", {.entities = {club}});
        doc.scene.registry().get<kast::Name>(entity(club)).value = "Mace";
        markDirty(doc);
        endSceneEdit(doc);
    });
    const auto frame = [&](const char* name, const SceneScope& scope, auto&& change) {
        const SceneState before = doc.recording ? SceneState() : captureScene(doc.scene, scope);
        change();
        sceneGesture(doc, name, scope, before);
        markDirty(doc);
    };
    checkUndoRedo(doc, "prefab mode: inspector drag", [&] {
        for (int i = 1; i <= 4; ++i)
            frame("Edit Mace", {.entities = {club}}, [&] { doc.scene.registry().get<kast::Collider>(entity(club)).radius += 0.1f; });
        endSceneGesture(doc);
    });
    checkUndoRedo(doc, "prefab mode: gizmo drag", [&] {
        for (int i = 1; i <= 3; ++i)
            frame("Move Mace", {.entities = {club}}, [&] { doc.scene.registry().get<kast::Transform>(entity(club)).position.x += 1.0f; });
        endSceneGesture(doc);
    });
    checkUndoRedo(doc, "prefab mode: add component", [&] {
        frame("Add Mesh Renderer", {.entities = {club}}, [&] { doc.scene.registry().emplace<kast::MeshRenderer>(entity(club)); });
        endSceneGesture(doc);
    });
    checkUndoRedo(doc, "prefab mode: remove component", [&] {
        frame("Remove Rotator", {.entities = {club}}, [&] { doc.scene.registry().remove<kast::Rotator>(entity(club)); });
        endSceneGesture(doc);
    });
    // A nested instance: drop, Revert field / all, Unpack.
    checkUndoRedo(doc, "prefab mode: prefab drop", [&] { dropPrefab(doc, "prefabs/gem.prefab", entity(root), error); });
    const kast::EntityId gem = id("Gem"), shard = id("Shard");
    const auto overrideShard = [&] {
        frame("Edit Shard", {.entities = {shard}}, [&] { doc.scene.registry().get<kast::Collider>(entity(shard)).radius = 0.7f; });
        endSceneGesture(doc);
    };
    using Kind = PrefabOp::Kind;
    overrideShard();
    checkUndoRedo(doc, "prefab mode: prefab revert field", [&] {
        runPrefabOp(doc, {.kind = Kind::RevertField, .entity = shard, .component = "Collider", .field = "radius"}, error);
    });
    overrideShard();
    checkUndoRedo(doc, "prefab mode: prefab revert all", [&] { runPrefabOp(doc, {.kind = Kind::RevertAll, .entity = gem}, error); });
    checkUndoRedo(doc, "prefab mode: prefab unpack", [&] { runPrefabOp(doc, {.kind = Kind::Unpack, .entity = gem}, error); });
    const size_t steps = doc.history.size();
    beginSceneEdit(doc, "Rename Mace", {.entities = {club}});
    endSceneEdit(doc);
    check(doc.history.size() == steps, "prefab mode undo: no step for an edit that changes nothing");

    // Independent stacks: the prefab's undoes all the way back to the prefab as opened; the scene waits untouched.
    while (doc.history.canUndo())
        undoScene(doc);
    check(kast::sceneToJson(doc.scene) == opened && !doc.dirty, "prefab mode undo: undo all gets back the prefab as opened");
    check(kast::sceneToJson(doc.prefabEdit->scene) == scene && doc.prefabEdit->history.size() == 2 &&
              doc.prefabEdit->history.canUndo() && doc.prefabEdit->dirty,
          "prefab mode undo: the waiting scene and its history are untouched");

    // Save point inside prefab mode.
    const auto setFriction = [&](float friction) {
        beginSceneEdit(doc, "Edit Club", {.entities = {club}});
        doc.scene.registry().get<kast::Collider>(entity(club)).friction = friction;
        markDirty(doc);
        endSceneEdit(doc);
    };
    setFriction(0.3f);
    check(doc.dirty && contains(windowTitle(doc), "*"), "prefab mode undo: an edit marks the prefab dirty");
    check(saveFile(doc, doc.path, error) && !doc.dirty && !contains(windowTitle(doc), "*"), "prefab mode undo: save the prefab");
    setFriction(0.4f);
    undoScene(doc);
    check(!doc.dirty && !contains(windowTitle(doc), "*"), "prefab mode undo: undo back to the save point is clean");
    undoScene(doc);
    check(doc.dirty, "prefab mode undo: undo past the save point is dirty");
    redoScene(doc);
    check(!doc.dirty, "prefab mode undo: redo back to the save point is clean");
    redoScene(doc);
    check(doc.dirty, "prefab mode undo: redo away from the save point is dirty");
    undoScene(doc); // the prefab as saved

    // Switching prefabs drops the open prefab's history, keeps the scene's.
    check(openPrefab(doc, "prefabs/gem.prefab", error) && doc.history.size() == 0 && doc.prefabEdit->history.size() == 2,
          "prefab mode undo: switching prefabs starts an empty history, the scene's still waits");
    createObject(doc, ObjectKind::Cube, doc.scene.children(entt::null).front());
    check(openPrefab(doc, "prefabs/orc.prefab", error) && doc.history.size() == 0,
          "prefab mode undo: switching back drops the other prefab's history");

    // Create Prefab and Apply write files: they clear the prefab's history.
    createObject(doc, ObjectKind::Cube, entity(root));
    std::string path;
    check(doc.history.size() == 1 && createPrefabFrom(doc, doc.selected, "prefabs", path, error) && doc.history.size() == 0,
          "prefab mode undo: Create Prefab clears the prefab's history");
    dropPrefab(doc, "prefabs/gem.prefab", entity(root), error);
    doc.scene.registry().get<kast::Collider>(named(doc.scene, "Shard")).radius = 0.6f;
    check(doc.history.size() == 1 &&
              runPrefabOp(doc, {.kind = Kind::ApplyField, .entity = id("Shard"), .component = "Collider", .field = "radius"}, error) &&
              doc.history.size() == 0 && doc.prefabEdit->history.size() == 2,
          "prefab mode undo: Apply clears the prefab's history (the waiting scene has no instance to refresh)");

    // Back: the prefab's history goes, the scene's comes back.
    closePrefab(doc);
    check(doc.history.size() == 2 && doc.dirty && kast::sceneToJson(doc.scene) == scene,
          "prefab mode undo: Back restores the scene and its history");
    // The deleted Orc comes back as orc.prefab is now (saved above), its override kept.
    const auto restoredClub = [&] {
        const entt::entity instance = kast::findById(doc.scene, orc);
        const entt::entity child = instance == entt::null ? entt::null : childNamed(doc.scene, instance, "Club");
        return child == entt::null ? kast::Collider{.radius = -1.0f} : doc.scene.registry().get<kast::Collider>(child);
    };
    undoScene(doc);
    check(selectedId(doc) == orc && restoredClub().friction == 0.3f && restoredClub().radius == 0.95f,
          "prefab mode undo: a scene step from before a prefab save brings back the saved prefab, overrides kept");
    redoScene(doc);
    undoScene(doc);
    check(restoredClub().friction == 0.3f && kast::isPrefabRoot(doc.scene, kast::findById(doc.scene, orc)),
          "prefab mode undo: and again after a redo");
    undoScene(doc);
    check(!doc.dirty && !doc.history.canUndo(), "prefab mode undo: the scene's save point is still reachable");
    check(openPrefab(doc, "prefabs/orc.prefab", error) && doc.history.size() == 0 && doc.prefabEdit->history.size() == 2,
          "prefab mode undo: reopening a prefab starts empty again");
    closePrefab(doc);
}

// --- GUI Builder undo (editor/gui_undo.h): the steps GuiBuilder records, on a GuiDoc ---

// edit changes doc and records exactly one step: undo puts back the serialized document, redo the edited one.
template <typename Fn>
void checkGuiUndoRedo(GuiDoc& doc, UndoStack& history, const char* what, Fn&& edit)
{
    const std::string before = doc.serialize();
    const size_t steps = history.size();
    edit();
    const std::string after = doc.serialize();
    const std::string name = std::format("gui undo: {}", what);
    check(after != before, (name + ": changes the document").c_str());
    check(history.size() == steps + 1 && history.canUndo() && !history.canRedo(), (name + ": records one step").c_str());
    history.undo();
    check(doc.serialize() == before, (name + ": undo restores the exact prior state").c_str());
    history.redo();
    check(doc.serialize() == after, (name + ": redo restores the exact post state").c_str());
}

void checkGuiUndo()
{
    GuiDoc doc;
    std::string error;
    check(doc.parse(kGuiSample, error), "gui undo: parses the sample");
    const pugi::xml_node root = doc.body().root();
    UndoStack history;
    pugi::xml_node selected;
    const auto select = [&](pugi::xml_node node) { selected = node; };
    const auto selectedId = [&] { return GuiDoc::attribute(selected, "id"); };
    const auto record = [&](const char* name, std::optional<GuiElementCopy> before, std::optional<GuiElementCopy> after) {
        return pushGuiStep(history, root, name, std::move(before), std::move(after), select);
    };
    // As GuiBuilder::applyEdit: copy before, edit, copy after.
    const auto relocate = [&](const char* id, auto edit) {
        const pugi::xml_node node = guiNode(doc, id);
        GuiElementCopy before = copyGuiElement(node);
        edit(node);
        record("Move", std::move(before), copyGuiElement(node));
    };
    const auto setAttribute = [&](const char* id, const char* name, const char* value) {
        const pugi::xml_node node = guiNode(doc, id);
        GuiElementCopy before = copyGuiElement(node);
        GuiDoc::setAttribute(node, name, value);
        return record("Edit", std::move(before), copyGuiElement(node));
    };

    checkGuiUndoRedo(doc, history, "add", [&] {
        GuiDoc::setAttribute(doc.add(GuiKind::Text, guiNode(doc, "panel")), "id", "added");
        record("Add Text", std::nullopt, copyGuiElement(guiNode(doc, "added")));
    });
    history.undo();
    check(!selected && !guiNode(doc, "added"), "gui undo: undoing an add clears the selection");
    history.redo();
    check(selectedId() == "added", "gui undo: redoing an add selects the element");
    checkGuiUndoRedo(doc, history, "delete", [&] {
        const pugi::xml_node node = guiNode(doc, "title");
        GuiElementCopy before = copyGuiElement(node);
        doc.remove(node);
        record("Delete Text", std::move(before), std::nullopt);
    });
    history.undo();
    check(selectedId() == "title" && selected == guiNode(doc, "title"), "gui undo: undoing a delete selects the element again");
    history.redo();
    check(!selected, "gui undo: redoing a delete clears the selection");
    checkGuiUndoRedo(doc, history, "move up", [&] { relocate("go", [&](pugi::xml_node node) { doc.move(node, -1); }); });
    checkGuiUndoRedo(doc, history, "move past a comment", [&] {
        relocate("panel", [&](pugi::xml_node node) { doc.move(node, 1); }); // <!-- keep me --> stays first
    });
    checkGuiUndoRedo(doc, history, "reparent", [&] {
        relocate("quiet", [&](pugi::xml_node node) { doc.reparent(node, guiNode(doc, "panel")); });
    });
    history.undo();
    check(selectedId() == "quiet" && selected.parent() == doc.body(), "gui undo: undoing a reparent selects it where it was");
    history.redo();
    checkGuiUndoRedo(doc, history, "attribute", [&] { setAttribute("panel", "class", "box wide"); });
    checkGuiUndoRedo(doc, history, "remove attribute", [&] { setAttribute("go", "data-event", ""); });
    checkGuiUndoRedo(doc, history, "text", [&] {
        const pugi::xml_node node = guiNode(doc, "go");
        GuiElementCopy before = copyGuiElement(node);
        GuiDoc::setText(node, "Go & on"); // one step for the whole field edit, as the Inspector's gesture
        GuiDoc::setText(node, "Go & onward");
        record("Edit Button text", std::move(before), copyGuiElement(node));
    });
    history.undo();
    check(selectedId() == "go" && GuiDoc::text(selected) == "Go", "gui undo: undoing a text edit selects the element");
    history.redo();

    const size_t steps = history.size();
    const std::string unchanged = doc.serialize();
    check(!setAttribute("panel", "class", "box wide") && history.size() == steps, "gui undo: no step for an unchanged field");
    const pugi::xml_node last = GuiDoc::elements(guiNode(doc, "panel")).back();
    GuiElementCopy before = copyGuiElement(last);
    doc.reparent(last, guiNode(doc, "panel")); // already its last child
    check(!record("Reparent", std::move(before), copyGuiElement(last)) && history.size() == steps &&
              doc.serialize() == unchanged,
          "gui undo: no step for a reparent that moved nothing");

    history.clear(true);
    setAttribute("mixed", "class", "a");
    check(!history.atSavePoint(), "gui undo: an edit leaves the save point");
    history.undo();
    check(history.atSavePoint(), "gui undo: undo back to the save point");
    history.redo();
    history.markSaved();
    setAttribute("mixed", "class", "b");
    history.undo();
    check(history.atSavePoint() && GuiDoc::attribute(guiNode(doc, "mixed"), "class") == "a",
          "gui undo: undo back to the last save");
    setAttribute("mixed", "class", "c");
    check(!history.canRedo() && GuiDoc::attribute(guiNode(doc, "mixed"), "class") == "c",
          "gui undo: a new edit discards the redo branch");

    history.clear(true);
    const std::string start = doc.serialize();
    for (int i = 1; i <= 101; ++i)
        setAttribute("mixed", "class", std::format("c{}", i).c_str());
    check(history.size() == UndoStack::kMaxSteps, "gui undo: capped at 100 steps");
    while (history.canUndo())
        history.undo();
    check(GuiDoc::attribute(guiNode(doc, "mixed"), "class") == "c1" && doc.serialize() != start && !history.atSavePoint(),
          "gui undo: the 101st step dropped the oldest");

    Document scene;
    resetToDefault(scene);
    const nlohmann::ordered_json sceneBefore = kast::sceneToJson(scene.scene);
    history.redo();
    history.undo();
    setAttribute("mixed", "class", "independent");
    history.undo();
    check(kast::sceneToJson(scene.scene) == sceneBefore && scene.history.size() == 0 && !scene.dirty,
          "gui undo: GUI steps never touch the scene or its history");
}

void checkLuaInstantiate()
{
    const std::filesystem::path dir = projectTestDir("kast_prefab_lua", "Prefabs");
    {
        kast::Scene source;
        makeOrc(source, dir);
    }
    writeFile(dir / "scripts/spawn.lua", R"(return { init = function()
    local id = kast.instantiate("prefabs/orc.prefab", nil, {x = 1, y = 2, z = 3})
    assert(id ~= nil, "instantiate returns the root id")
    local t = kast.get(id, "Transform")
    assert(t.position.x == 1 and t.position.y == 2 and t.position.z == 3, "at position")
    local count = function() local n = 0; for e in kast.query() do n = n + 1 end; return n end
    local before = count()
    local child = kast.instantiate("prefabs/orc.prefab", id)
    assert(child ~= nil and child ~= id and count() == before + 4, "a second one, with its 3 children")
    assert(kast.instantiate("prefabs/nope.prefab") == nil, "a missing file returns nil")
    assert(kast.instantiate("../outside.prefab") == nil, "a path outside the project returns nil")
    kast.log("instantiate ok")
end, update = function() end })");
    writeFile(dir / "prefabs/broken.prefab", "{ nope");
    writeFile(dir / "scripts/broken.lua", R"(return { init = function()
    assert(kast.instantiate("prefabs/broken.prefab") == nil, "a broken file returns nil")
end, update = function() end })");
    kast::Scene scene;
    scene.systems = {"scripts/spawn.lua", "scripts/broken.lua"};
    kast::LuaSystems lua;
    lua.start(scene, dir, "", nullptr);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.instantiate: success cases in Lua pass");
    checkOk(lua, 1, "kast.instantiate: a broken file returns nil");
    const entt::entity first = named(scene, "Orc");
    check(scene.valid(first) && kast::isPrefabRoot(scene, first) && childNamed(scene, first, "Orc") != entt::null &&
              kast::isPrefabRoot(scene, childNamed(scene, first, "Orc")),
          "kast.instantiate: linked instances, the second under the first");
    check(logged("instantiate ok") && logged("kast.instantiate: Invalid prefab 'prefabs/nope.prefab'") &&
              logged("kast.instantiate: Invalid prefab 'prefabs/broken.prefab'"),
          "kast.instantiate: failures are logged with why");
    lua.stop();
}
// Runtime controls on the CPU emitter state (kast.particles, the Inspector) and the alive estimate.
void checkParticleControls()
{
    kast::ParticleEmitter emitter;
    emitter.rate = 0.0f;
    emitter.burstCount = 3;
    emitter.maxParticles = 50;
    emitter.lifetimeMin = 1.0f;
    emitter.lifetimeMax = 2.0f;
    kast::ParticleEmitterState state; // stopped
    check(kast::advanceEmitter(emitter, state, 0.1f) == 0 && kast::aliveParticles(emitter, state) == 0,
          "particles: a stopped emitter spawns nothing");
    check(kast::burstEmitter(emitter, state, 7) == 7 && state.spawned == 7 && kast::aliveParticles(emitter, state) == 7,
          "particles: burst spawns even when stopped");
    check(kast::burstEmitter(emitter, state, 500) == 50 && kast::burstEmitter(emitter, state, -4) == 0 &&
              kast::aliveParticles(emitter, state) == 50,
          "particles: burst clamps to 0..maxParticles; alive is at most maxParticles");
    kast::advanceEmitter(emitter, state, 0.1f);
    check(state.lastSpawn == 57, "particles: bursts since the last tick count as its spawns (for a renderer's first sight)");
    for (int i = 0; i < 18; ++i)
        kast::advanceEmitter(emitter, state, 0.1f);
    check(kast::aliveParticles(emitter, state) == 50, "particles: alive until lifetimeMax has passed");
    kast::advanceEmitter(emitter, state, 0.1f);
    check(kast::aliveParticles(emitter, state) == 0 && state.births.empty(), "particles: then none (an upper bound)");

    kast::playEmitter(state);
    check(state.playing && kast::advanceEmitter(emitter, state, 0.1f) == 3, "particles: play starts the cycle with its burst");
    state.paused = true;
    kast::playEmitter(state);
    check(!state.paused && state.playing && kast::advanceEmitter(emitter, state, 0.1f) == 0,
          "particles: play resumes a paused emitter without restarting its cycle");
    const uint32_t clears = state.clears;
    kast::stopEmitter(state, false);
    check(!state.playing && state.clears == clears && kast::aliveParticles(emitter, state) == 3,
          "particles: stop: no more spawns, the live ones finish their life");
    kast::playEmitter(state);
    check(kast::advanceEmitter(emitter, state, 0.1f) == 3, "particles: play after stop restarts the cycle (its burst again)");
    kast::stopEmitter(state, true);
    check(!state.playing && state.clears == clears + 1 && kast::aliveParticles(emitter, state) == 0,
          "particles: stop(clear) kills the live particles");
    state.paused = true;
    state.time = 0.5f;
    state.cycle = 4;
    kast::restartEmitter(state);
    check(state.playing && !state.paused && state.time == 0.0f && state.cycle == 0 && state.clears == clears + 2 &&
              kast::advanceEmitter(emitter, state, 0.1f) == 3,
          "particles: restart clears and plays from the start");
}

// The shape gizmo's lines: each shape's own outline, in local space.
void checkParticleGizmo()
{
    kast::ParticleEmitter emitter;
    emitter.radius = 2.0f;
    emitter.shape = kast::ParticleShape::Sphere;
    const auto sphere = kast::particleShapeLines(emitter);
    check(sphere.size() == 96 && std::ranges::all_of(sphere, [](const auto& line) {
              return near(bx::length(line.first), 2.0f) && near(bx::length(line.second), 2.0f);
          }),
          "particles gizmo: a sphere is three circles of its radius");
    emitter.shape = kast::ParticleShape::Box;
    emitter.boxSize = {2.0f, 4.0f, 6.0f};
    const auto box = kast::particleShapeLines(emitter);
    check(box.size() == 12 && std::ranges::all_of(box, [](const auto& line) {
              const float d = bx::length(bx::sub(line.second, line.first));
              return near(d, 2.0f) || near(d, 4.0f) || near(d, 6.0f);
          }) && near(box[0].first, {-1.0f, -2.0f, -3.0f}),
          "particles gizmo: a box is its twelve edges, centered");
    emitter.shape = kast::ParticleShape::Cone;
    emitter.radius = 0.5f;
    emitter.coneAngle = 0.0f;
    const auto beam = kast::particleShapeLines(emitter);
    check(beam.size() == 68 && near(beam[32].first, {0.5f, 1.0f, 0.0f}) && near(beam.back().second, {0.0f, 1.0f, -0.5f}),
          "particles gizmo: cone angle 0 is a cylinder one metre up");
    emitter.coneAngle = 45.0f;
    const auto cone = kast::particleShapeLines(emitter);
    const float spread = 0.5f + std::sqrt(0.5f);
    check(near(cone[0].first, {0.5f, 0.0f, 0.0f}) && near(cone[32].first, {spread, std::sqrt(0.5f), 0.0f}),
          "particles gizmo: a cone's base disc and its spread at coneAngle");
}

// Editor paths: GameObject > Particle System, undo/redo of its creation and of a field edit (the preview follows),
// Duplicate, save -> reopen and prefabs keep the emitter.
void checkParticleEditor()
{
    const std::filesystem::path dir = projectTestDir("kast_particle_editor", "Particles");
    check(std::ranges::find(kObjectKinds, ObjectKind::ParticleSystem) != std::end(kObjectKinds) &&
              std::string(objectKindLabel(ObjectKind::ParticleSystem)) == "Particle System",
          "particles: the GameObject menu has Particle System");
    Document doc;
    resetToDefault(doc);
    doc.project = kast::Project{.root = dir, .name = "Particles"};
    checkUndoRedo(doc, "create particle system", [&] { createObject(doc, ObjectKind::ParticleSystem, entt::null); });
    check(doc.history.undoName() == "Create Particle System", "particles: the create step is named");
    const entt::entity made = named(doc.scene, "Particle System");
    const auto* emitter = made == entt::null ? nullptr : doc.scene.registry().try_get<kast::ParticleEmitter>(made);
    check(emitter && doc.selected == made && doc.scene.registry().all_of<kast::Transform>(made) &&
              kast::componentToJson(entt::meta_any{*emitter}) == kast::componentToJson(entt::meta_any{kast::ParticleEmitter{}}),
          "particles: Particle System is a selected entity with a Transform and a default emitter");
    const kast::EntityId id = idOf(doc.scene, made);
    undoScene(doc);
    check(named(doc.scene, "Particle System") == entt::null, "particles: undo removes the created emitter");
    updatePlay(doc, 0.1f); // the preview's state goes with it
    check(doc.scene.registry().view<kast::ParticleEmitterState>().empty(), "particles: no state left after undo");
    redoScene(doc);
    const auto entity = [&] { return kast::findById(doc.scene, id); };
    check(doc.scene.registry().all_of<kast::ParticleEmitter>(entity()), "particles: redo brings it back by id");

    // A field edit (an Inspector drag) and its undo: the preview spawns at the rate in the scene right now.
    updatePlay(doc, 0.1f);
    const auto spawnedIn = [&](float seconds) {
        const uint64_t before = doc.scene.registry().get<kast::ParticleEmitterState>(entity()).spawned;
        for (int i = 0; i < static_cast<int>(seconds * 10.0f + 0.5f); ++i)
            updatePlay(doc, 0.1f);
        return doc.scene.registry().get<kast::ParticleEmitterState>(entity()).spawned - before;
    };
    const uint64_t defaultRate = spawnedIn(1.0f);
    const SceneScope scope{.entities = {id}};
    const SceneState before = captureScene(doc.scene, scope);
    doc.scene.registry().get<kast::ParticleEmitter>(entity()).rate = 100.0f;
    sceneGesture(doc, "Edit Particle System", scope, before);
    markDirty(doc);
    endSceneGesture(doc);
    const uint64_t edited = spawnedIn(1.0f);
    const uint64_t stateId = doc.scene.registry().get<kast::ParticleEmitterState>(entity()).id;
    undoScene(doc);
    const uint64_t undone = spawnedIn(1.0f);
    redoScene(doc);
    const uint64_t redone = spawnedIn(1.0f);
    check(defaultRate == 10 && edited == 100 && undone == 10 && redone == 100,
          std::format("particles: undo/redo of a field edit changes the live preview (spawned per second: {}, {}, {}, {})",
                      defaultRate, edited, undone, redone).c_str());
    check(doc.scene.registry().get<kast::ParticleEmitterState>(entity()).id == stateId,
          "particles: undoing a field edit keeps the running preview (no restart)");

    // Restart / Pause / the count are runtime only: no undo step, not dirty.
    const size_t steps = doc.history.size();
    const bool dirty = doc.dirty;
    kast::restartEmitter(doc.scene.registry().get<kast::ParticleEmitterState>(entity()));
    doc.scene.registry().get<kast::ParticleEmitterState>(entity()).paused = true;
    check(doc.history.size() == steps && doc.dirty == dirty, "particles: Restart / Pause add no undo step, dirty nothing");

    // Duplicate copies the emitter (not its runtime state).
    kast::ParticleEmitter& source = doc.scene.registry().get<kast::ParticleEmitter>(entity());
    source.shape = kast::ParticleShape::Box;
    source.blend = kast::ParticleBlend::Additive;
    source.texture = "textures/spark.png";
    const nlohmann::ordered_json fields = kast::componentToJson(entt::meta_any{source});
    doc.selected = entity();
    duplicateSelected(doc);
    const entt::entity copy = doc.selected;
    const auto* copied = doc.scene.registry().try_get<kast::ParticleEmitter>(copy);
    check(copy != entity() && copied && kast::componentToJson(entt::meta_any{*copied}) == fields &&
              !doc.scene.registry().all_of<kast::ParticleEmitterState>(copy),
          "particles: Duplicate copies the emitter, not its runtime state");

    // Save -> reopen.
    std::string error;
    const std::string path = (dir / "emitters.scene").string();
    check(kast::saveScene(doc.scene, path, error), "particles: save the scene");
    Document reopened;
    check(openFile(reopened, path, error), "particles: reopen the scene");
    const entt::entity loaded = kast::findById(reopened.scene, id);
    const auto* loadedEmitter = loaded == entt::null ? nullptr : reopened.scene.registry().try_get<kast::ParticleEmitter>(loaded);
    check(loadedEmitter && kast::componentToJson(entt::meta_any{*loadedEmitter}) == fields,
          "particles: the emitter round-trips through save -> reopen");

    // Prefab: create from the emitter, instantiate, and the instance has the same emitter; the file has it too.
    check(kast::createPrefab(doc.scene, entity(), "prefabs/sparks.prefab", dir, error), "particles: create a prefab");
    std::shared_ptr<const kast::PrefabAsset> asset;
    check(kast::loadPrefab(dir, "prefabs/sparks.prefab", asset, error), "particles: load the prefab");
    const entt::entity instance = asset ? kast::instantiatePrefab(doc.scene, asset, entt::null, error) : entt::null;
    const auto* instanced = instance == entt::null ? nullptr : doc.scene.registry().try_get<kast::ParticleEmitter>(instance);
    check(instanced && kast::componentToJson(entt::meta_any{*instanced}) == fields,
          "particles: the emitter round-trips through a prefab");
    const nlohmann::ordered_json prefabFile = readJson(dir / "prefabs/sparks.prefab");
    check(prefabFile["entities"][0]["components"]["ParticleEmitter"]["shape"] == "box", "particles: the prefab file has it");
}

// kast.particles from Lua: argument errors, and play / stop / burst / alive on the emitters' CPU state.
void checkParticleLua()
{
    const std::filesystem::path dir = luaTestDir();
    Document doc;
    doc.path = (dir / "particles.scene").string();
    doc.project = kast::Project{.root = dir, .name = "Particles"};
    entt::registry& registry = doc.scene.registry();
    const entt::entity running = doc.scene.create("Running");
    registry.emplace<kast::ParticleEmitter>(running).rate = 100.0f;
    const entt::entity idle = doc.scene.create("Idle");
    kast::ParticleEmitter& idleEmitter = registry.emplace<kast::ParticleEmitter>(idle);
    idleEmitter.playOnStart = false;
    idleEmitter.rate = 0.0f;
    idleEmitter.burstCount = 7;
    idleEmitter.maxParticles = 100;
    const entt::entity plain = doc.scene.create("Plain");
    writeFile(dir / "scripts/particles.lua", std::format(R"lua(local RUNNING, IDLE, PLAIN = {}, {}, {}
local function fails(part, fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok, "expected an error mentioning " .. part)
    assert(string.find(err, part, 1, true), "error '" .. tostring(err) .. "' does not mention " .. part)
end
local frame = 0
return {{ update = function(dt)
    frame = frame + 1
    local p = kast.particles
    if frame == 1 then
        assert(p.alive(IDLE) == 0, "a stopped emitter has none")
        assert(p.burst(IDLE, 5) == 5 and p.alive(IDLE) == 5, "burst while stopped")
        assert(p.burst(IDLE, 1000) == 100 and p.alive(IDLE) == 100, "burst and alive clamp to maxParticles")
        p.stop(IDLE, true)
        assert(p.alive(IDLE) == 0, "stop(clear) kills them")
        p.play(IDLE)
        p.stop(RUNNING)
        p.stop(RUNNING, false)
        fails("invalid entity id 999999", p.play, 999999)
        fails("invalid entity id nil", p.alive)
        fails("kast.particles.play: entity " .. PLAIN .. " has no ParticleEmitter", p.play, PLAIN)
        fails("kast.particles.alive: entity " .. PLAIN .. " has no ParticleEmitter", p.alive, PLAIN)
        fails("kast.particles.stop: clear must be true, false or nil, got 1", p.stop, RUNNING, 1)
        fails("kast.particles.burst: count must be a whole number >= 0, got -1", p.burst, IDLE, -1)
        fails("kast.particles.burst: count must be a whole number >= 0, got 1.5", p.burst, IDLE, 1.5)
        fails("kast.particles.burst: count must be a whole number >= 0", p.burst, IDLE, "3")
        fails("kast.particles.burst: count must be a whole number >= 0", p.burst, IDLE)
        fails("particles.lua:", function() p.burst(PLAIN, 1) end) -- raised at the calling line
        fails("kast is read-only", function() p.play = nil end)
    elseif frame == 2 then
        assert(p.alive(RUNNING) > 0, "stopped without clear: the live particles finish their life")
        assert(p.alive(IDLE) >= 7, "play started the cycle with its burst")
        kast.log("particles lua checks done")
    end
end }}
)lua",
                                                         idOf(doc.scene, running), idOf(doc.scene, idle), idOf(doc.scene, plain)));
    doc.scene.systems = {"scripts/particles.lua"};
    startPlay(doc);
    updatePlay(doc, 0.1f);
    const auto* runningState = registry.try_get<kast::ParticleEmitterState>(running);
    const auto* idleState = registry.try_get<kast::ParticleEmitterState>(idle);
    check(runningState && !runningState->playing && idleState && idleState->playing && idleState->clears == 1 &&
              idleState->spawned == 105,
          "kast.particles: stop / play / burst / stop(clear) change the emitters' state");
    const uint64_t spawned = runningState ? runningState->spawned : 0;
    updatePlay(doc, 0.1f);
    check(registry.get<kast::ParticleEmitterState>(running).spawned == spawned, "kast.particles: a stopped emitter spawns no more");
    checkOk(doc.lua, 0, "kast.particles asserts pass");
    check(logged("particles lua checks done"), "kast.particles checks ran");
    stopPlay(doc);
}

int main(int argc, char** argv)
{
    kast::disableCrashDialogsIfRequested();
    kast::Audio::setDefaultOutput(kast::Audio::Output::Offline); // no sound from the checks; render() pulls the mix
    kast::models().setImporter(importModel); // as the editor, without the cache (no files in tests/project/.kast)
    if (argc >= 3 && std::string_view(argv[1]) == "--probe-png")
        return probePng(argc, argv);
    if ((argc == 5 || argc == 6) && std::string_view(argv[1]) == "--diff-png")
        return diffPng(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--bench-physics")
        return benchPhysics();
    if (argc == 2 && std::string_view(argv[1]) == "--undo") { // only the undo / redo checks
        checkUndoStack();
        checkSceneUndo();
        checkPrefabUndo();
        checkPrefabModeUndo();
        checkGuiUndo();
        removeTree(testRoot());
        kast::log::info(g_failures == 0 ? "All undo checks passed" : std::format("{} undo check(s) failed", g_failures));
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--particles") { // only the particle checks
        checkParticleJson();
        checkParticleClock();
        checkParticleBounds();
        checkParticlesNoop();
        checkParticleControls();
        checkParticleGizmo();
        checkParticleEditor();
        checkParticleLua();
        removeTree(testRoot());
        kast::log::info(g_failures == 0 ? "All particle checks passed" : std::format("{} particle check(s) failed", g_failures));
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--shaders") { // only the material and .kshader checks
        checkMaterialFiles();
        checkShaders();
        checkLuaMaterials();
        checkShaderParamEditing();
        checkMaterialFeatureImport();
        checkPrimitiveTangents();
        removeTree(testRoot());
        kast::log::info(g_failures == 0 ? "All shader checks passed" : std::format("{} shader check(s) failed", g_failures));
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--textures") { // only the texture decode, KTX / DDS and cook checks
        checkTextureDecode();
        checkGpuImages();
        checkTextureCooks();
        kast::log::info(g_failures == 0 ? "All texture checks passed" : std::format("{} texture check(s) failed", g_failures));
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--net") // only the networking checks
        return netSelfCheck() == 0 ? 0 : 1;
    if (argc == 4 && std::string_view(argv[1]) == "--live-auth") // against a running services/world stack
        return liveAuthCheck(argv[2], argv[3]);
    if (argc == 6 && std::string_view(argv[1]) == "--live-world") // the same stack, plus its world server
        return liveWorldCheck(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 6 && std::string_view(argv[1]) == "--live-replication") // the same stack: two replicated players
        return liveReplicationCheck(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 5 && std::string_view(argv[1]) == "--live-online") // the editor's Online panel model, same stack
        return liveOnlineCheck(argv[2], argv[3], argv[4]);
    checkRotations();
    checkPicking();
    checkMirroredReparent();
    checkDeepHierarchy();
    checkHierarchy();
    checkComponentRegistry();
    checkComponentJson();
    checkDuplicateComponents();
    checkLoadValidation();
    checkPointLights();
    checkShadows();
    checkFrustumCulling();
    checkPostProcessing();
    checkSky();
    checkMaterialFiles();
    checkShaders();
    checkPrimitiveTangents();
    checkMaterialFeatureImport();
    checkShaderClock();
    checkMaterialLibrary();
    checkTextureDecode();
    checkGpuImages();
    checkTextureCooks();
    checkMaterialScenes();
    checkLuaMaterials();
    checkShaderParamEditing();
    checkCameras();
    checkStatsOverlay();
    checkInput();
    checkRotatorSystem();
    checkSnapshot();
    checkPlayMode();
    checkSystemsFormat();
    checkLuaPlayMode();
    checkLuaApi();
    checkPrefabFiles();
    checkPrefabOverrides();
    checkPrefabNesting();
    checkMissingPrefabs();
    checkPrefabRemap();
    checkPrefabEditMode();
    checkUndoStack();
    checkSceneUndo();
    checkPrefabUndo();
    checkPrefabModeUndo();
    checkGuiUndo();
    checkLuaInstantiate();
    checkLuaGui();
    checkGuiEffectCompile();
    checkLuaItems();
    checkLuaSandbox();
    checkLuaStringMetatable();
    checkLuaDepthCap();
    checkLuaErrors();
    checkLuaHotReload();
    checkUtf8Paths();
    checkProjectFiles();
    checkProjectDocument();
    checkRecentProjects();
    checkNoProjectLua();
    checkSystemEdits();
    checkSceneSwitchErrors();
    checkSceneSwitch();
    checkEventBus();
    checkLuaEvents();
    checkLuaEventsFromCpp();
    checkLuaEventErrors();
    checkLuaEventLifetime();
    checkLuaEventCoroutine();
    checkLuaEventSceneSwitch();
    checkLuaEventPause();
    checkLuaComponentDefinitions();
    checkLuaComponentRegistry();
    checkLuaComponentApi();
    checkLuaComponentDrift();
    checkMissingComponents();
    checkComponentHotReload();
    checkComponentTemplate();
    checkNewLuaComponent();
    checkPak();
    checkFileWrites();
    checkBuild();
    checkBuildSettings();
    checkWindowPos();
    checkOnlineSettings();
    checkStateMachineDefinitions();
    checkStateMachineRuntime();
    checkLuaStateMachineApi();
    checkStateMachineSceneSwitch();
    checkEntityStateMachines();
    checkStateMachineExitChanges();
    checkLuaWatchdog();
    checkComponentWatchdog();
    checkStateMachineHotReload();
    checkStateMachineFiles();
    checkStateMachineEditor();
    checkAssets();
    checkGui();
    checkGuiClipboard();
    checkExternalEditor();
    checkModelLoading();
    checkModelCache();
    checkAnimationSampling();
    checkAnimatorSystem();
    checkModelEditor();
    checkPhysicsComponents();
    checkPhysicsFall();
    checkPhysicsPlayStop();
    checkPhysicsTimestep();
    checkPhysicsEvents();
    checkPhysicsBodyControl();
    checkPhysicsRuntimeSync();
    checkPhysicsCharacter();
    checkPhysicsLua();
    checkTerrainHeights();
    checkTilesAround();
    checkTerrainPhysics();
    checkTerrainStreaming();
    checkTerrainPlay();
    checkZoneObjects();
    checkZoneObjectsPlay();
    checkColliderWireframe();
    checkAudioDecode();
    checkAudioMix();
    checkAudioLooping();
    checkAudioVoices();
    checkAudioHandles();
    checkAudioNullDevice();
    checkAudioSourceComponent();
    checkAudioProjectSettings();
    checkAudioPlaySession();
    checkParticleJson();
    checkParticleClock();
    checkParticleBounds();
    checkParticlesNoop();
    checkParticleControls();
    checkParticleGizmo();
    checkParticleEditor();
    checkParticleLua();
    g_failures += netSelfCheck(); // tests/net_self_check.cpp
    checkMountedPak(); // last: the mounted pak hides the disk for the rest of the run
    removeTree(testRoot());
    std::error_code ec;
    check(!std::filesystem::exists(testRoot(), ec) && !ec, "removes its temp folder");
    if (g_failures > 0) {
        kast::log::error(std::format("{} self-check(s) failed", g_failures));
        return 1;
    }
    kast::log::info("All self-checks passed");
    return 0;
}
