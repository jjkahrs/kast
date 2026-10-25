#include "camera.h"

#include <scene_io.h>

#include <bgfx/bgfx.h>

#include <cmath>

namespace {

constexpr float kLookDegreesPerPixel = 0.25f;
constexpr float kMaxPitch = 89.0f;
constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 5000.0f;
constexpr float kDollyStep = 0.85f; // distance multiplier per wheel notch
constexpr float kFocusMargin = 1.2f;
constexpr bx::Vec3 kWorldUp{0.0f, 1.0f, 0.0f};

bool readVec3(const nlohmann::ordered_json& json, bx::Vec3& out)
{
    if (!json.is_array() || json.size() != 3 || !json[0].is_number() || !json[1].is_number() || !json[2].is_number())
        return false;
    out = {json[0].get<float>(), json[1].get<float>(), json[2].get<float>()};
    return true;
}

} // namespace

bx::Vec3 EditorCamera::forward() const
{
    const float y = bx::toRad(yaw);
    const float p = bx::toRad(pitch);
    return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
}

bx::Vec3 EditorCamera::right() const { return bx::normalize(bx::cross(forward(), kWorldUp)); }

bx::Vec3 EditorCamera::up() const { return bx::cross(right(), forward()); }

bx::Vec3 EditorCamera::position() const { return bx::sub(pivot, bx::mul(forward(), distance)); }

void EditorCamera::viewMatrix(float* out) const
{
    bx::mtxLookAt(out, position(), pivot, kWorldUp, bx::Handedness::Right);
}

void EditorCamera::projMatrix(float* out, float aspect) const
{
    const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
    if (orthographic) {
        // The planes as in perspective, from the camera's position (the pivot sits distance in front of it).
        const float h = orthoSize(), w = h * aspect;
        bx::mtxOrtho(out, -w, w, -h, h, nearPlane, farPlane, 0.0f, homogeneousDepth, bx::Handedness::Right);
    } else {
        bx::mtxProj(out, kFovY, aspect, nearPlane, farPlane, homogeneousDepth, bx::Handedness::Right);
    }
}

float EditorCamera::orthoSize() const { return distance * std::tan(bx::toRad(kFovY) * 0.5f); }

void EditorCamera::look(float dx, float dy)
{
    const bx::Vec3 eye = position();
    orbit(dx, dy);
    pivot = bx::add(eye, bx::mul(forward(), distance));
}

void EditorCamera::orbit(float dx, float dy)
{
    yaw = std::fmod(yaw - dx * kLookDegreesPerPixel, 360.0f);
    pitch = bx::clamp(pitch - dy * kLookDegreesPerPixel, -kMaxPitch, kMaxPitch);
}

void EditorCamera::pan(float dx, float dy, float viewportHeight)
{
    // World units per pixel at the pivot's depth, so the point under the cursor follows it.
    const float scale = 2.0f * orthoSize() / bx::max(viewportHeight, 1.0f); // the same orthographic
    pivot = bx::add(pivot, bx::add(bx::mul(right(), -dx * scale), bx::mul(up(), dy * scale)));
}

void EditorCamera::move(const bx::Vec3& offset) { pivot = bx::add(pivot, offset); }

void EditorCamera::dolly(float wheel)
{
    distance = bx::clamp(distance * std::pow(kDollyStep, wheel), kMinDistance, kMaxDistance);
}

void EditorCamera::focus(const kast::Aabb& bounds)
{
    pivot = bx::mul(bx::add(bounds.min, bounds.max), 0.5f);
    const float radius = bx::max(bx::length(bx::sub(bounds.max, bounds.min)) * 0.5f, 0.25f);
    // Perspective: the bounding sphere fits the FOV. Orthographic: orthoSize() fits the sphere's radius.
    const float fit = orthographic ? std::tan(bx::toRad(kFovY) * 0.5f) : std::sin(bx::toRad(kFovY) * 0.5f);
    distance = bx::clamp(kFocusMargin * radius / fit, kMinDistance, kMaxDistance);
}

nlohmann::ordered_json EditorCamera::toJson() const
{
    using kast::jsonNumber;
    nlohmann::ordered_json json = {{"pivot", {jsonNumber(pivot.x), jsonNumber(pivot.y), jsonNumber(pivot.z)}},
                                   {"yaw", jsonNumber(yaw)},
                                   {"pitch", jsonNumber(pitch)},
                                   {"distance", jsonNumber(distance)}};
    if (orthographic) // only then: perspective scenes save as before
        json["orthographic"] = true;
    return json;
}

bool EditorCamera::fromJson(const nlohmann::ordered_json& json)
{
    bx::Vec3 newPivot(bx::InitNone);
    if (!json.is_object() || !json.contains("pivot") || !readVec3(json["pivot"], newPivot))
        return false;
    for (const char* key : {"yaw", "pitch", "distance"})
        if (!json.contains(key) || !json[key].is_number())
            return false;
    if (json.contains("orthographic") && !json["orthographic"].is_boolean())
        return false;
    pivot = newPivot;
    yaw = json["yaw"].get<float>();
    pitch = bx::clamp(json["pitch"].get<float>(), -kMaxPitch, kMaxPitch);
    distance = bx::clamp(json["distance"].get<float>(), kMinDistance, kMaxDistance);
    orthographic = json.value("orthographic", false); // missing in older files: perspective
    return true;
}
