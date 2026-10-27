// Small vector/quaternion/FOV math for the rig. Header-only so the offline tests use the same code.
//
// Conventions:
//  - OpenXR tracking space: +X right, +Y up, -Z forward (right-handed), metres.
//  - Game space: whatever the engine uses. The rig never assumes its handedness: it builds every
//    game-space vector as R*x + U*y + F*(-z) from the game's own right/up/forward vectors
//    (TransformQ:GetR/GetU/GetF on the Lua side), so the result is correct in either convention.
#pragma once

#include <algorithm>
#include <cmath>

namespace jvr::vm {

constexpr float kPi = 3.14159265358979f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
    float l = Length(a);
    return l > 1e-8f ? a * (1.f / l) : Vec3{0, 0, 0};
}
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};
inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Vec3 Rotate(Quat q, Vec3 v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = Cross(u, v) * 2.f;
    return v + t * q.w + Cross(u, t);
}
inline Quat AxisAngle(Vec3 axis, float rad) {
    Vec3 a = Normalize(axis);
    float s = std::sin(rad * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f)};
}

// Rotation whose local +X/+Y/+Z axes map to the given orthonormal right/up/back vectors.
inline Quat QuatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0.f) {
        float s = std::sqrt(tr + 1.f) * 2.f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}

// A pose in OpenXR tracking space.
struct Pose {
    Quat q;
    Vec3 p;
};
inline Pose Compose(Pose parent, Pose child) { return {parent.q * child.q, parent.p + Rotate(parent.q, child.p)}; }
inline Pose Inverse(Pose a) {
    Quat c = Conjugate(a.q);
    return {c, Rotate(c, a.p * -1.f)};
}

// Yaw-only part of a tracking-space rotation (about +Y).
inline float YawOf(Quat q) {
    Vec3 f = Rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

// Right/up/forward basis of a camera or anchor, in game space.
struct Basis {
    Vec3 r{1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
};

// Tracking-space vector -> game-space vector, using the anchor basis.
inline Vec3 ToGame(const Basis& b, Vec3 t) { return b.r * t.x + b.u * t.y + b.f * (-t.z); }

// Game-space vector -> tracking-space vector (inverse of ToGame for an orthonormal basis).
inline Vec3 FromGame(const Basis& b, Vec3 g) { return {Dot(g, b.r), Dot(g, b.u), -Dot(g, b.f)}; }

// Removes pitch and roll from an anchor: forward and right flattened onto the plane normal to `up`.
// Falls back to the given basis if the anchor looks straight up or down.
inline Basis Flatten(const Basis& in, Vec3 up) {
    Basis out;
    out.u = Normalize(up);
    Vec3 f = in.f - out.u * Dot(in.f, out.u);
    Vec3 r = in.r - out.u * Dot(in.r, out.u);
    if (Length(f) < 1e-3f || Length(r) < 1e-3f) return in;
    out.f = Normalize(f);
    out.r = Normalize(r);
    return out;
}

// Rotates a basis about its own up axis by `rad` (snap/smooth turn). Handedness-free: uses the
// basis' own right vector, so a positive angle always turns toward +right.
inline Basis Yawed(const Basis& b, float rad) {
    float c = std::cos(rad), s = std::sin(rad);
    Basis o = b;
    o.f = Normalize(b.f * c + b.r * s);
    o.r = Normalize(b.r * c - b.f * s);
    return o;
}

// Game-space camera: position plus basis, built from an anchor and a tracking-space pose.
struct GameCamera {
    Vec3 pos;
    Basis basis;
};

// anchor: game-space origin of the play space (at real floor level or seat reference),
// scale: game metres per real metre, local: tracking-space pose relative to the play-space origin.
inline GameCamera Place(Vec3 anchorPos, const Basis& anchor, float scale, Pose local) {
    GameCamera c;
    c.pos = anchorPos + ToGame(anchor, local.p) * scale;
    c.basis.r = Normalize(ToGame(anchor, Rotate(local.q, {1, 0, 0})));
    c.basis.u = Normalize(ToGame(anchor, Rotate(local.q, {0, 1, 0})));
    c.basis.f = Normalize(ToGame(anchor, Rotate(local.q, {0, 0, -1})));
    return c;
}

// OpenXR field of view (angles in radians; left/down negative).
struct Fov {
    float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;
};

// Symmetric vertical FOV (radians) for a camera with the given aspect (width/height) that
// covers every eye FOV in `eyes`, plus `marginRad` on each side. This is what the game gets
// through SetFOVY: it drives culling, LOD and the projection of AER frames.
inline float EnclosingFovY(const Fov* eyes, int count, float aspect, float marginRad) {
    float tanV = 0.f, tanH = 0.f;
    for (int i = 0; i < count; ++i) {
        tanV = std::max(tanV, std::max(std::tan(eyes[i].up), std::tan(-eyes[i].down)));
        tanH = std::max(tanH, std::max(std::tan(eyes[i].right), std::tan(-eyes[i].left)));
    }
    float halfV = std::atan(tanV) + marginRad;
    float halfH = std::atan(tanH) + marginRad;
    float tanNeededFromH = std::tan(halfH) / std::max(aspect, 0.1f);
    halfV = std::max(halfV, std::atan(tanNeededFromH));
    return std::min(2.f * halfV, kPi * 0.95f);
}

// The symmetric FOV the game actually rendered, as an OpenXR Fov (for xrEndFrame).
inline Fov SymmetricFov(float fovY, float aspect) {
    float tv = std::tan(fovY * 0.5f);
    float th = tv * aspect;
    Fov f;
    f.up = std::atan(tv);
    f.down = -f.up;
    f.right = std::atan(th);
    f.left = -f.right;
    return f;
}

// Projects a game-space point into a camera's screen UV (0..1, v down). Returns false if behind.
inline bool ProjectUV(const GameCamera& cam, float fovY, float aspect, Vec3 p, float& u, float& v) {
    Vec3 d = p - cam.pos;
    float z = Dot(d, cam.basis.f);
    if (z <= 1e-4f) return false;
    float tv = std::tan(fovY * 0.5f), th = tv * aspect;
    u = 0.5f + 0.5f * (Dot(d, cam.basis.r) / z) / th;
    v = 0.5f - 0.5f * (Dot(d, cam.basis.u) / z) / tv;
    return true;
}

// Ray vs. rectangle centred at `c` with half extents along `ax` (right) and `ay` (up).
// Returns UV (0..1, v down) of the hit.
inline bool RayRect(Vec3 o, Vec3 dir, Vec3 c, Vec3 ax, Vec3 ay, float halfW, float halfH, float& u, float& v, float& t) {
    Vec3 n = Cross(ax, ay);
    float den = Dot(dir, n);
    if (std::fabs(den) < 1e-6f) return false;
    t = Dot(c - o, n) / den;
    if (t <= 0.f) return false;
    Vec3 hit = o + dir * t - c;
    float x = Dot(hit, ax), y = Dot(hit, ay);
    if (std::fabs(x) > halfW || std::fabs(y) > halfH) return false;
    u = 0.5f + 0.5f * x / halfW;
    v = 0.5f - 0.5f * y / halfH;
    return true;
}

// Exponential smoothing factor for a rate (1/s) over dt.
inline float SmoothAlpha(float rate, float dt) { return 1.f - std::exp(-rate * std::max(dt, 0.f)); }

// Steering angle (radians, +right) of a wheel held with both hands, from their positions in the
// seat's tracking frame. Uses the angle of the line between the hands in the wheel plane (x/y).
inline float WheelAngle(Vec3 left, Vec3 right) {
    Vec3 d = right - left;
    return std::atan2(-d.y, d.x);
}

} // namespace jvr::vm
