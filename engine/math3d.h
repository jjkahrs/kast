#pragma once

#include <bx/math.h>

// Small math helpers on top of bx. Conventions (see README):
// - Right-handed, Y-up, 1 unit = 1 meter. Cameras and lights look down their local -Z.
// - Matrices are bgfx/bx layout: float[16], row vectors (p' = p * M), translation in [12..14].
// - Quaternions are standard Hamilton rotations (x, y, z, w), the same convention as glTF.
//   bx::mtxFromQuaternion / bx::mul(Vec3, Quaternion) rotate the opposite way, so use these helpers instead.
// - Euler angles are degrees (pitch X, yaw Y, roll Z), applied Z then X then Y.
namespace kast {

struct Aabb {
    bx::Vec3 min;
    bx::Vec3 max;
};

struct Ray {
    bx::Vec3 origin;
    bx::Vec3 direction; // normalized
};

// Rotation matrix for q.
void mtxFromQuat(float* out, const bx::Quaternion& q);

// out = scale, then rotate, then translate.
void mtxFromTrs(float* out, const bx::Vec3& translation, const bx::Quaternion& rotation, const bx::Vec3& scale);

// Inverse of mtxFromTrs for matrices without shear. ponytail: negative scale comes back as a rotation flip.
void decomposeTrs(const float* m, bx::Vec3& translation, bx::Quaternion& rotation, bx::Vec3& scale);

bx::Quaternion quatFromEulerDeg(const bx::Vec3& degrees);
bx::Vec3 eulerDegFromQuat(const bx::Quaternion& q);

// Transforms a direction (ignores translation).
bx::Vec3 transformDir(const bx::Vec3& v, const float* m);

// Ray through pixel (x, y) of a width x height viewport, y down. viewProj = view * proj.
Ray screenRay(const float* viewProj, float x, float y, float width, float height, bool homogeneousDepth);

// Distance along the ray to the box, or a negative value on a miss. A ray starting inside the box hits at 0.
float intersect(const Ray& ray, const Aabb& box);

} // namespace kast
