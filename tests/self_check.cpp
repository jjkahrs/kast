// Assert-style checks for the non-trivial engine logic: rotation conventions, matrix decomposition, picking rays,
// hierarchy edits, component registry, scene-file validation, cameras, input, systems, Lua systems, projects, scene
// switching, editor Play mode, Lua components, game.pak, Build, state machines, Assets panel file operations and the
// game GUI (builder document model, GUI files, UIDocument, button click events) and the external editor setting.
// Exit 0 if every check passes. Run by CTest as engine_self_check.
#include "assets.h"   // Assets panel file logic; links editor/assets.cpp
#include "build.h"    // editor Build pipeline; links editor/build.cpp
#include "document.h" // editor Play mode; links editor/document.cpp, no window needed
#include "external_editor.h" // external editor command; links editor/external_editor.cpp
#include "gui_doc.h"  // GUI Builder document model; links editor/gui_doc.cpp
#include "recent_projects.h" // Project Manager list; links editor/recent_projects.cpp

#include <components.h>
#include <event_bus.h>
#include <gui_events.h>
#include <files.h>
#include <input.h>
#include <log.h>
#include <lua_components.h>
#include <lua_systems.h>
#include <math3d.h>
#include <pak.h>
#include <project.h>
#include <scene.h>
#include <scene_io.h>
#include <state_machines.h>
#include <systems.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

int g_failures = 0;

void check(bool ok, const char* what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED line {}: {}", where.line(), what));
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
    check(typeNames() == std::vector<std::string>{"Transform", "MeshRenderer", "DirectionalLight", "Camera", "Rotator",
                                                 "StateMachine", "UIDocument"},
          "built-in component types in registration order");
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
    const kast::MeshRenderer m = roundTrip(kast::MeshRenderer{kast::Primitive::Sphere, {0.25f, 0.5f, 1}},
                                           R"({"primitive":"sphere","color":[0.25,0.5,1.0]})");
    check(m.primitive == kast::Primitive::Sphere && near(m.color, {0.25f, 0.5f, 1}), "MeshRenderer values (enum field)");
    const kast::DirectionalLight l = roundTrip(kast::DirectionalLight{{1, 0.9f, 0.8f}, 2.5f},
                                               R"({"color":[1.0,0.9,0.8],"intensity":2.5})");
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
    registry.emplace<kast::MeshRenderer>(e, kast::Primitive::Plane, bx::Vec3{0.1f, 0.2f, 0.3f});
    registry.emplace<kast::Rotator>(e, bx::Vec3{1, 0, 0}, 12.0f);
    registry.emplace<TestComponent>(e, 5, false, "copy me");
    registry.emplace<kast::Camera>(e, 40.0f, 0.3f, 50.0f);
    registry.get<kast::Transform>(e).position = {4, 5, 6};
    const entt::entity copy = scene.duplicate(e);
    const auto* mesh = registry.try_get<kast::MeshRenderer>(copy);
    const auto* rotator = registry.try_get<kast::Rotator>(copy);
    const auto* test = registry.try_get<TestComponent>(copy);
    check(mesh && mesh->primitive == kast::Primitive::Plane && near(mesh->color, {0.1f, 0.2f, 0.3f}), "duplicate copies MeshRenderer");
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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kast_self_check";
    std::filesystem::create_directories(dir);
    const std::string transform = R"("Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]})";
    const auto entity = [&](int id, const char* parent, const std::string& more = "") {
        return std::format(R"({{"id": {}, "parent": {}, "name": "E", "components": {{{}{}}}}})", id, parent, transform, more);
    };
    const auto file = [](const std::string& entities) { return R"({"version": 4, "entities": [)" + entities + "]}"; };
    const std::tuple<const char*, std::string, const char*> bad[] = {
        // what, file contents, text the error must mention
        {"invalid JSON", "{ \"version\": 4, ", ""},
        {"wrong version", R"({"version": 99, "entities": []})", "version"},
        {"version 1 rejected",
         R"({"version": 1, "entities": [{"id": 1, "parent": null, "name": "E", "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}]})",
         "version"},
        {"missing Transform", file(R"({"id": 1, "parent": null, "name": "E", "components": {}})"), "Transform"},
        {"missing components", file(R"({"id": 1, "parent": null, "name": "E"})"), "components"},
        {"bad enum", file(entity(1, "null", R"(, "MeshRenderer": {"primitive": "torus", "color": [1,1,1]})")), "torus"},
        {"mistyped field", file(entity(1, "null", R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": "fast"})")),
         "degreesPerSecond"},
        {"unknown field", file(entity(1, "null", R"(, "Rotator": {"axis": [0,1,0], "degreesPerSecond": 1, "rpm": 2})")), "rpm"},
        {"missing field", file(entity(1, "null", R"(, "DirectionalLight": {"color": [1,1,1]})")), "intensity"},
        {"zero quaternion",
         file(R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,0], "scale": [1,1,1]}}})"),
         "rotation"},
        {"camera fov 0", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 0, "nearPlane": 0.1, "farPlane": 10})")),
         "verticalFovDegrees"},
        {"camera fov 180", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 180, "nearPlane": 0.1, "farPlane": 10})")),
         "verticalFovDegrees"},
        {"camera near 0", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 60, "nearPlane": 0, "farPlane": 10})")),
         "nearPlane"},
        {"camera near >= far", file(entity(1, "null", R"(, "Camera": {"verticalFovDegrees": 60, "nearPlane": 10, "farPlane": 10})")),
         "farPlane"},
        {"dangling parent", file(entity(1, "7")), "parent"},
        {"cycle", file(entity(1, "2") + ", " + entity(2, "1")), "cycle"},
        {"self parent", file(entity(1, "1")), "cycle"},
        {"duplicate id", file(entity(1, "null") + ", " + entity(1, "null")), "duplicate"},
    };
    kast::Scene scene;
    scene.create("Keep me");
    for (const auto& [what, text, mention] : bad) {
        const std::string path = (dir / "bad.scene").string();
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        std::string error;
        const bool loaded = kast::loadScene(path, scene, error);
        check(!loaded && !error.empty() && error.find(mention) != std::string::npos, what);
        if (!loaded)
            kast::log::info(std::format("expected error ({}): {}", what, error));
    }
    check(scene.children(entt::null).size() == 1, "failed loads leave the scene untouched");

    // Child listed before its parent still loads, keeping file order among siblings.
    const std::string path = (dir / "good.scene").string();
    std::ofstream(path, std::ios::binary | std::ios::trunc) << file(entity(5, "9") + ", " + entity(9, "null"));
    std::string error;
    check(kast::loadScene(path, scene, error), "child-before-parent file loads");
    check(scene.children(entt::null).size() == 1 && scene.children(scene.children(entt::null)[0]).size() == 1,
          "child-before-parent hierarchy rebuilt");
    check(scene.registry().get<kast::PersistentId>(scene.create("New")).value == 10, "new ids continue after loaded ids");
    std::filesystem::remove_all(dir);
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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kast_lua_check";
    std::filesystem::remove_all(dir);
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
    check(read(first) == read(second) && contains(read(first), "\"version\": 4") && contains(read(first), "\"systems\""),
          "version 4 systems round trip is byte-identical");

    const std::string entity =
        R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}})";
    const auto file = [&](int version, const std::string& systems) {
        return std::format(R"({{"version": {}, "entities": [{}]{}}})", version, entity, systems);
    };
    writeFile(dir / "nosystems.scene", file(4, ""));
    loaded.systems = {"stale.lua"};
    check(kast::loadScene((dir / "nosystems.scene").string(), loaded, error) && loaded.systems.empty(),
          "a version 4 file without \"systems\" loads with none");
    const std::tuple<const char*, std::string, const char*> bad[] = {
        {"systems not an array", file(4, R"(, "systems": "a.lua")"), "\"systems\" must be an array"},
        {"systems entry not a string", file(4, R"(, "systems": ["a.lua", 7])"), "systems[1]: must be a non-empty string"},
        {"systems entry empty", file(4, R"(, "systems": [""])"), "systems[0]: must be a non-empty string"},
        {"version 1 rejected", file(1, ""), "unsupported \"version\" 1"},
        {"version 2 rejected", file(2, ""), "unsupported \"version\" 2"},
        {"version 3 rejected", file(3, R"(, "systems": [])"), "unsupported \"version\" 3"},
        {"version 5 rejected", file(5, ""), "unsupported \"version\" 5"},
    };
    for (const auto& [what, text, mention] : bad) {
        writeFile(dir / "bad.scene", text);
        const bool ok = kast::loadScene((dir / "bad.scene").string(), loaded, error);
        check(!ok && contains(error, mention), what);
        if (!ok)
            kast::log::info(std::format("expected error ({}): {}", what, error));
    }
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    fails('unknown key name "NotAKey"', function() kast.input.keyDown("NotAKey") end)
    fails("kast is read-only", function() kast.get = nil end)
    fails("kast is read-only", function() kast.input.keyDown = nil end)

    assert(kast.input.keyDown("W") and not kast.input.keyDown("S"), "keyDown by SDL name")
    local dx, dy = kast.input.mouseDelta()
    assert(dx == 3 and dy == -2, "mouseDelta")

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
    scene.systems = {"api.lua"};
    kast::Input input;
    input.keysDown[SDL_SCANCODE_W] = true;
    input.mouseDeltaX = 3.0f;
    input.mouseDeltaY = -2.0f;

    kast::LuaSystems lua;
    lua.start(scene, dir, "", &input);
    lua.tick(0.1f);
    checkOk(lua, 0, "kast.* API asserts in Lua pass");
    check(near(registry.get<kast::Transform>(cube).position, {2, 3, 4}) &&
              near(registry.get<kast::Transform>(cube).scale, {1, 1, 1}),
          "set wrote Transform; rejected sets left it unchanged");
    check(registry.get<kast::MeshRenderer>(cube).primitive == kast::Primitive::Sphere, "set wrote an enum by name");
    check(registry.get<kast::Camera>(cam).verticalFovDegrees == 60.0f && registry.get<kast::Camera>(cam).farPlane == 1000.0f,
          "rejected Camera sets left it unchanged");
    check(named(scene, "Parent") == entt::null && named(scene, "Child") == entt::null, "destroy removes the subtree");
    const entt::entity late = named(scene, "Late");
    check(scene.valid(late) && registry.all_of<kast::MeshRenderer>(late) && scene.parent(late) == entt::null,
          "create adds a root entity through Scene");
    check(logged("[info] api.lua: hello") && logged("[warn] api.lua: careful") && logged("[error] api.lua: oops"),
          "kast.log/warn/error prefix the file");
    lua.stop();
    check(!lua.running() && lua.status().empty(), "stop ends the session");
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "scripts");
    writeFile(dir / kast::kProjectFileName, std::format(R"({{"version": 1, "name": "{}", "startScene": null}})", projectName));
    return dir;
}

std::filesystem::path looseTestDir(const char* name) // no project.kast
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(outside);
    std::filesystem::remove_all(parent);
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
    std::filesystem::remove_all(root);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
    std::filesystem::remove_all(other);
    std::filesystem::remove_all(loose);
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
    std::filesystem::remove_all(dir);
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

    stopPlay(doc);
    check(logged("sw c shutdown") && countLogged("sw a init") == 1, "Stop: shutdown's loadScene is ignored");
    check(kast::sceneToJson(doc.scene).dump(2) == before, "Stop after two switches restores the byte-identical pre-Play scene");
    check(doc.path == path && doc.project && doc.project->root == root && doc.dirty &&
              doc.scene.valid(doc.selected) && doc.scene.registry().get<kast::PersistentId>(doc.selected).value == selectedId &&
              windowTitle(doc) == "Kast Editor - Switch - a*",
          "Stop restores path, project, dirty flag, selection by id and the title");

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
    std::filesystem::remove_all(dir);
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

    // file, contents, text the error must mention (besides the path)
    const std::tuple<const char*, const char*, const char*> bad[] = {
        {"not_table.lua", "return 5", "must return exactly one table"},
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
}

constexpr const char* kSceneHead = R"({"version": 4, "entities": [{"id": 1, "parent": null, "name": "Drifty", "components": {)"
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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

    // The last output folder is editor-local: .kast/editor.json, project.kast untouched.
    const std::string projectFile = readText(root / "project.kast");
    check(!loadBuildFolder(root / "scripts"), "no remembered Build folder without .kast/editor.json");
    check(saveBuildFolder(root, out, error) && loadBuildFolder(root) == std::filesystem::path(out.generic_string()),
          "remembered Build folder round trip");
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
              std::vector<std::string>{"events.scene", "fsm.scene", "health.scene", "scene_a.scene", "scene_b.scene",
                                       "ui.scene"},
          "tests/project lists events.scene, fsm.scene, health.scene, scene_a.scene, scene_b.scene, ui.scene");

    const std::filesystem::path root = looseTestDir("kast_build_settings_check") / "game";
    std::filesystem::copy(KAST_TEST_PROJECT_DIR, root, std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(root / ".hidden");
    std::filesystem::create_directories(root / "levels");
    std::filesystem::copy_file(root / "scene_b.scene", root / ".hidden/x.scene");
    std::filesystem::copy_file(root / "scene_b.scene", root / ".dot.scene");
    std::filesystem::copy_file(root / "scene_b.scene", root / "levels/c.scene");
    writeFile(root / "broken.scene", "{oops");
    check(projectScenes(root) == std::vector<std::string>{"broken.scene", "events.scene", "fsm.scene", "health.scene",
                                                          "levels/c.scene", "scene_a.scene", "scene_b.scene",
                                                          "ui.scene"},
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

// Last: mounting a pak lasts for the rest of the process, so later disk reads would fail.
void checkMountedPak()
{
    const std::filesystem::path dir = looseTestDir("kast_mount_check");
    std::filesystem::create_directories(dir / "src/components/sub");
    writeFile(dir / "src/project.kast", R"({"version": 1, "name": "Packed", "startScene": "a.scene"})");
    writeFile(dir / "src/components/sub/c.lua", "return 1");
    writeFile(dir / "src/components/d.lua", "return 2");
    std::string error;
    check(kast::writePak(dir / "src", {"project.kast", "components/sub/c.lua", "components/d.lua"}, dir / "game.pak", error),
          "write a pak to mount");
    kast::Pak pak;
    check(pak.open(dir / "game.pak", error), "open the pak to mount");
    const std::filesystem::path root = dir / "game"; // not on disk: everything comes from the pak
    kast::mountPak(std::move(pak), root);
    kast::Project project;
    check(kast::loadProject(root / "project.kast", project, error) && project.name == "Packed", "project.kast from the pak");
    std::string text;
    check(kast::readFile(root / "components/d.lua", text) && text == "return 2", "readFile from the pak");
    check(!kast::readFile(dir / "src/components/d.lua", text), "with a pak mounted, the disk is not read");
    check(kast::listFiles(root / "components") ==
              std::vector<std::filesystem::path>{root / "components/d.lua", root / "components/sub/c.lua"},
          "listFiles from the pak");
    check(kast::listFiles(root).size() == 3 && kast::listFiles(root / "nothing").empty(),
          "listFiles of the root and of a missing folder");
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    std::filesystem::remove_all(dir);
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
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "kast_assets_paths";
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

void checkGui()
{
    checkGuiDocRoundTrip();
    checkGuiDocEdits();
    checkGuiFiles();
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

    const std::filesystem::path file = looseTestDir("kast_editor_settings_check") / "Kast/Editor/editor_settings.json";
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
    ExternalEditor editor(file);
    check(editor.command() == kDefaultEditorCommand && editor.setCommand("vim {file}", error) &&
              editor.command() == "vim {file}" && loadEditorCommand(file) == "vim {file}",
          "setCommand saves and is used from then on");
}

int main()
{
    checkRotations();
    checkPicking();
    checkHierarchy();
    checkComponentRegistry();
    checkComponentJson();
    checkDuplicateComponents();
    checkLoadValidation();
    checkCameras();
    checkInput();
    checkRotatorSystem();
    checkSnapshot();
    checkPlayMode();
    checkSystemsFormat();
    checkLuaPlayMode();
    checkLuaApi();
    checkLuaSandbox();
    checkLuaErrors();
    checkLuaHotReload();
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
    checkBuild();
    checkBuildSettings();
    checkStateMachineDefinitions();
    checkStateMachineRuntime();
    checkLuaStateMachineApi();
    checkStateMachineSceneSwitch();
    checkEntityStateMachines();
    checkStateMachineHotReload();
    checkStateMachineFiles();
    checkStateMachineEditor();
    checkAssets();
    checkGui();
    checkExternalEditor();
    checkMountedPak(); // last: the mounted pak hides the disk for the rest of the run
    if (g_failures > 0) {
        kast::log::error(std::format("{} self-check(s) failed", g_failures));
        return 1;
    }
    kast::log::info("All self-checks passed");
    return 0;
}
