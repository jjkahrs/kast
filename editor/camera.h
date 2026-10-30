#pragma once

#include <math3d.h>

#include <nlohmann/json.hpp>

// Unity-style editor camera: orbits a pivot at a distance, looking along yaw/pitch (degrees).
// Yaw 0 looks down -Z; positive pitch looks up. Orthographic: half the view's height is orthoSize(), from distance, so
// toggling keeps what is at the pivot framed the same and dolly zooms.
struct EditorCamera {
    static constexpr float kFovY = 60.0f; // degrees
    static constexpr float kNear = 0.1f;
    static constexpr float kFar = 1000.0f;

    bx::Vec3 pivot{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = -20.0f;
    float distance = 10.0f;
    float nearPlane = kNear; // main.cpp widens both while the Scene view shows a zone
    float farPlane = kFar;
    bool orthographic = false;

    bx::Vec3 forward() const;
    bx::Vec3 right() const;
    bx::Vec3 up() const;
    bx::Vec3 position() const;
    void viewMatrix(float* out) const;
    void projMatrix(float* out, float aspect) const;
    float orthoSize() const; // distance * tan(kFovY / 2): the perspective view's half height at the pivot

    void look(float dx, float dy);   // mouse-look around the camera position (fly mode)
    void orbit(float dx, float dy);  // rotate around the pivot
    void pan(float dx, float dy, float viewportHeight); // drag the view by pixels
    void move(const bx::Vec3& offset); // translate camera and pivot together
    void dolly(float wheel);         // positive = closer
    void focus(const kast::Aabb& bounds); // frame a world-space box

    nlohmann::ordered_json toJson() const;
    bool fromJson(const nlohmann::ordered_json& json); // false (and unchanged) if missing or malformed
};
