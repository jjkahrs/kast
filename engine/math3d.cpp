#include "math3d.h"

#include <cmath>
#include <limits>
#include <utility>

namespace kast {

void mtxFromQuat(float* out, const bx::Quaternion& q)
{
    // bx builds the transposed (inverse) rotation; rotating by the conjugate gives the standard one.
    bx::mtxFromQuaternion(out, bx::invert(q));
}

void mtxFromTrs(float* out, const bx::Vec3& translation, const bx::Quaternion& rotation, const bx::Vec3& scale)
{
    mtxFromQuat(out, rotation);
    const float s[3] = {scale.x, scale.y, scale.z};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            out[row * 4 + col] *= s[row];
    out[12] = translation.x;
    out[13] = translation.y;
    out[14] = translation.z;
}

void decomposeTrs(const float* m, bx::Vec3& translation, bx::Quaternion& rotation, bx::Vec3& scale)
{
    translation = {m[12], m[13], m[14]};
    float s[3];
    float r[3][3]; // r[i][j]: column-vector rotation matrix, so r[i][j] = m[j * 4 + i] / scale_j
    for (int row = 0; row < 3; ++row) {
        s[row] = bx::length(bx::Vec3{m[row * 4], m[row * 4 + 1], m[row * 4 + 2]});
        const float inv = s[row] > 0.0f ? 1.0f / s[row] : 0.0f;
        for (int col = 0; col < 3; ++col)
            r[col][row] = m[row * 4 + col] * inv;
    }
    scale = {s[0], s[1], s[2]};

    const float trace = r[0][0] + r[1][1] + r[2][2];
    bx::Quaternion q(bx::InitIdentity);
    if (trace > 0.0f) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        q = {(r[2][1] - r[1][2]) * k, (r[0][2] - r[2][0]) * k, (r[1][0] - r[0][1]) * k, 0.25f / k};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]);
        q = {0.25f * k, (r[0][1] + r[1][0]) / k, (r[0][2] + r[2][0]) / k, (r[2][1] - r[1][2]) / k};
    } else if (r[1][1] > r[2][2]) {
        const float k = 2.0f * std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]);
        q = {(r[0][1] + r[1][0]) / k, 0.25f * k, (r[1][2] + r[2][1]) / k, (r[0][2] - r[2][0]) / k};
    } else {
        const float k = 2.0f * std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]);
        q = {(r[0][2] + r[2][0]) / k, (r[1][2] + r[2][1]) / k, 0.25f * k, (r[1][0] - r[0][1]) / k};
    }
    rotation = bx::normalize(q);
}

bx::Quaternion quatFromEulerDeg(const bx::Vec3& degrees)
{
    const bx::Quaternion qx = bx::rotateX(bx::toRad(degrees.x));
    const bx::Quaternion qy = bx::rotateY(bx::toRad(degrees.y));
    const bx::Quaternion qz = bx::rotateZ(bx::toRad(degrees.z));
    return bx::mul(bx::mul(qy, qx), qz); // Hamilton product: qz applied first
}

bx::Vec3 eulerDegFromQuat(const bx::Quaternion& q)
{
    // R = Ry * Rx * Rz; elements of the column-vector rotation matrix.
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r00 = 1.0f - 2.0f * (y * y + z * z);
    const float r02 = 2.0f * (x * z + y * w);
    const float r10 = 2.0f * (x * y + z * w);
    const float r11 = 1.0f - 2.0f * (x * x + z * z);
    const float r12 = 2.0f * (y * z - x * w);
    const float r20 = 2.0f * (x * z - y * w);
    const float r22 = 1.0f - 2.0f * (x * x + y * y);

    const float sinPitch = bx::clamp(-r12, -1.0f, 1.0f);
    const bx::Vec3 radians = std::abs(sinPitch) > 0.99999f // gimbal lock: fold roll into yaw
        ? bx::Vec3{std::asin(sinPitch), std::atan2(-r20, r00), 0.0f}
        : bx::Vec3{std::asin(sinPitch), std::atan2(r02, r22), std::atan2(r10, r11)};
    // + 0.0f turns -0 into 0 so the Inspector never shows "-0.000"
    return {bx::toDeg(radians.x) + 0.0f, bx::toDeg(radians.y) + 0.0f, bx::toDeg(radians.z) + 0.0f};
}

bx::Vec3 transformDir(const bx::Vec3& v, const float* m)
{
    return bx::mulXyz0(v, m);
}

Ray screenRay(const float* viewProj, float x, float y, float width, float height, bool homogeneousDepth)
{
    float inv[16];
    bx::mtxInverse(inv, viewProj);
    const float ndcX = 2.0f * x / width - 1.0f;
    const float ndcY = 1.0f - 2.0f * y / height;
    const bx::Vec3 nearPoint = bx::mulH({ndcX, ndcY, homogeneousDepth ? -1.0f : 0.0f}, inv);
    const bx::Vec3 farPoint = bx::mulH({ndcX, ndcY, 1.0f}, inv);
    return {nearPoint, bx::normalize(bx::sub(farPoint, nearPoint))};
}

float intersect(const Ray& ray, const Aabb& box)
{
    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    float tNear = 0.0f;
    float tFar = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-8f) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
                return -1.0f;
            continue;
        }
        float t0 = (lo[axis] - origin[axis]) / dir[axis];
        float t1 = (hi[axis] - origin[axis]) / dir[axis];
        if (t0 > t1)
            std::swap(t0, t1);
        tNear = bx::max(tNear, t0);
        tFar = bx::min(tFar, t1);
        if (tNear > tFar)
            return -1.0f;
    }
    return tNear;
}

} // namespace kast
