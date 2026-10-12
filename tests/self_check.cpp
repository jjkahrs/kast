// Assert-style checks for the non-trivial engine logic: rotation conventions, matrix decomposition, picking rays,
// hierarchy edits, component registry, scene-file validation, cameras, input, systems, Lua systems and editor Play mode.
// Exit 0 if every check passes. Run by CTest as engine_self_check.
#include "document.h" // editor Play mode; links editor/document.cpp, no window needed

#include <components.h>
#include <input.h>
#include <log.h>
#include <lua_systems.h>
#include <math3d.h>
#include <scene.h>
#include <scene_io.h>
#include <systems.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <source_location>
#include <string>

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
    check(typeNames() == std::vector<std::string>{"Transform", "MeshRenderer", "DirectionalLight", "Camera", "Rotator"},
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
    const auto file = [](const std::string& entities) { return R"({"version": 2, "entities": [)" + entities + "]}"; };
    const std::tuple<const char*, std::string, const char*> bad[] = {
        // what, file contents, text the error must mention
        {"invalid JSON", "{ \"version\": 2, ", ""},
        {"wrong version", R"({"version": 99, "entities": []})", "version"},
        {"version 1 rejected",
         R"({"version": 1, "entities": [{"id": 1, "parent": null, "name": "E", "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}]})",
         "version"},
        {"unknown component", file(entity(1, "null", R"(, "Teleporter": {})")), "Teleporter"},
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
    check(read(first) == read(second) && contains(read(first), "\"version\": 3") && contains(read(first), "\"systems\""),
          "version 3 systems round trip is byte-identical");

    const std::string entity =
        R"({"id": 1, "parent": null, "name": "E", "components": {"Transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]}}})";
    const auto file = [&](int version, const std::string& systems) {
        return std::format(R"({{"version": {}, "entities": [{}]{}}})", version, entity, systems);
    };
    for (int version : {2, 3}) {
        writeFile(dir / "old.scene", file(version, ""));
        loaded.systems = {"stale.lua"};
        check(kast::loadScene((dir / "old.scene").string(), loaded, error) && loaded.systems.empty(),
              "version 2 and 3 files without \"systems\" load with none");
    }
    const std::tuple<const char*, std::string, const char*> bad[] = {
        {"systems not an array", file(3, R"(, "systems": "a.lua")"), "\"systems\" must be an array"},
        {"systems entry not a string", file(3, R"(, "systems": ["a.lua", 7])"), "systems[1]: must be a non-empty string"},
        {"systems entry empty", file(3, R"(, "systems": [""])"), "systems[0]: must be a non-empty string"},
        {"version 4", file(4, ""), "version"},
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
    lua.start(scene, dir, &input);
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
    lua.start(scene, dir, nullptr);
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
    lua.start(scene, dir, nullptr);
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
    lua.start(scene, dir, nullptr);
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

// Systems panel operations on a Document (the panel only adds ImGui widgets on top).
void checkSystemEdits()
{
    const std::filesystem::path dir = luaTestDir();
    Document doc;
    resetToDefault(doc);
    std::string error;
    check(!newSystemFromTemplate(doc, error) && error == "Save the scene first" && doc.scene.systems.empty(),
          "template needs a saved scene");

    doc.path = (dir / "edit.scene").string();
    check(newSystemFromTemplate(doc, error) && newSystemFromTemplate(doc, error), "template written");
    check((doc.scene.systems == std::vector<std::string>{"scripts/new_system.lua", "scripts/new_system_2.lua"}) &&
              std::filesystem::exists(dir / "scripts/new_system_2.lua") && doc.dirty,
          "template files get unique names, are added relative to the scene and set dirty");
    doc.dirty = false;
    writeFile(dir / "other.lua", "return { update = function(dt) end }");
    addSystem(doc, dir / "other.lua");
    check(doc.scene.systems.back() == "other.lua" && doc.dirty, "add stores the path relative to the scene");
    moveSystem(doc, 2, -1);
    moveSystem(doc, 0, -1); // past the top: no-op
    check((doc.scene.systems == std::vector<std::string>{"scripts/new_system.lua", "other.lua", "scripts/new_system_2.lua"}),
          "move up / down");
    removeSystem(doc, 0);
    check((doc.scene.systems == std::vector<std::string>{"other.lua", "scripts/new_system_2.lua"}), "remove");

    check(canEditSystems(doc), "systems editable when stopped");
    startPlay(doc);
    check(!canEditSystems(doc), "systems not editable during Play");
    checkOk(doc.lua, 1, "the template is a working system");
    updatePlay(doc, 0.1f);
    checkOk(doc.lua, 1, "the template's update runs");
    stopPlay(doc);

    // Save As into another directory keeps the paths pointing at the same files.
    std::filesystem::create_directories(dir / "sub");
    check(saveFile(doc, (dir / "sub/moved.scene").string(), error), "Save As");
    check((doc.scene.systems == std::vector<std::string>{"../other.lua", "../scripts/new_system_2.lua"}),
          "Save As rebases system paths");
    startPlay(doc);
    checkOk(doc.lua, 0, "rebased paths still load");
    stopPlay(doc);
    std::filesystem::remove_all(dir);
}

} // namespace

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
    checkSystemEdits();
    if (g_failures > 0) {
        kast::log::error(std::format("{} self-check(s) failed", g_failures));
        return 1;
    }
    kast::log::info("All self-checks passed");
    return 0;
}
