// Offline checks of the rig math (no game, no headset). Exit code 0 = all passed.
#include "../src/vrmath.h"

#include <cstdio>

using namespace jvr::vm;

static int g_fail = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_fail;                                                  \
        }                                                              \
    } while (0)

static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }
static bool Near(Vec3 a, Vec3 b, float e = 1e-4f) { return Near(a.x, b.x, e) && Near(a.y, b.y, e) && Near(a.z, b.z, e); }

int main() {
    // Two game handedness conventions: the rig must give the same physical result in both.
    Basis lh;                       // +X right, +Y up, +Z forward
    Basis rh{{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}; // +X right, +Y up, -Z forward
    for (const Basis& b : {lh, rh}) {
        // Looking straight ahead in tracking space = the anchor's forward.
        GameCamera c = Place({10, 0, 0}, b, 1.f, Pose{});
        CHECK(Near(c.basis.f, b.f));
        CHECK(Near(c.basis.r, b.r));
        // Head 0.5 m to the right in tracking space = 0.5 m along the anchor's right.
        Pose p;
        p.p = {0.5f, 0, 0};
        c = Place({10, 0, 0}, b, 2.f, p);
        CHECK(Near(c.pos, Vec3{10, 0, 0} + b.r * 1.f));
        // Turning the head right (negative rotation about +Y in OpenXR) points forward toward +right.
        Pose t;
        t.q = AxisAngle({0, 1, 0}, -kPi / 2);
        c = Place({}, b, 1.f, t);
        CHECK(Near(c.basis.f, b.r));
        // Yawed with a positive angle also turns toward +right, in both conventions.
        Basis y = Yawed(b, kPi / 2);
        CHECK(Near(y.f, b.r));
    }

    // Flatten removes pitch: a camera pitched down 45 degrees keeps its heading.
    Basis pitched{{1, 0, 0}, Normalize({0, 1, 1}), Normalize({0, -1, 1})};
    Basis flat = Flatten(pitched, {0, 1, 0});
    CHECK(Near(flat.f, Vec3{0, 0, 1}));
    CHECK(Near(flat.u, Vec3{0, 1, 0}));

    // Culling FOV covers both eyes plus margin, and the horizontal half-angle too.
    Fov eyes[2] = {{-0.9f, 0.7f, 0.8f, -0.85f}, {-0.7f, 0.9f, 0.8f, -0.85f}};
    float aspect = 1.0f;
    float fovY = EnclosingFovY(eyes, 2, aspect, 0.f);
    CHECK(std::tan(fovY * 0.5f) >= std::tan(0.85f) - 1e-4f);
    CHECK(std::tan(fovY * 0.5f) * aspect >= std::tan(0.9f) - 1e-4f);
    float fovMargin = EnclosingFovY(eyes, 2, aspect, 0.1f);
    CHECK(fovMargin > fovY);
    // A wide frame needs no extra vertical FOV for the horizontal extent.
    CHECK(Near(EnclosingFovY(eyes, 2, 2.0f, 0.f), 2.f * 0.85f, 1e-3f));

    // SymmetricFov reproduces the FOV handed to the game.
    Fov s = SymmetricFov(1.2f, 16.f / 9.f);
    CHECK(Near(s.up - s.down, 1.2f));
    CHECK(Near(std::tan(s.right), std::tan(0.6f) * 16.f / 9.f));

    // Projection: a point straight ahead lands at the screen centre; right of it, u > 0.5.
    GameCamera cam;
    cam.pos = {0, 0, 0};
    float u, v;
    CHECK(ProjectUV(cam, 1.f, 1.5f, {0, 0, 10}, u, v) && Near(u, 0.5f) && Near(v, 0.5f));
    CHECK(ProjectUV(cam, 1.f, 1.5f, {1, 1, 10}, u, v) && u > 0.5f && v < 0.5f);
    CHECK(!ProjectUV(cam, 1.f, 1.5f, {0, 0, -10}, u, v));

    // Ray vs. screen rectangle.
    float t;
    CHECK(RayRect({0, 0, 0}, {0, 0, -1}, {0, 0, -2}, {1, 0, 0}, {0, 1, 0}, 1.f, 0.5f, u, v, t) && Near(u, 0.5f) && Near(t, 2.f));
    CHECK(RayRect({0.5f, 0.25f, 0}, {0, 0, -1}, {0, 0, -2}, {1, 0, 0}, {0, 1, 0}, 1.f, 0.5f, u, v, t) && Near(u, 0.75f) && Near(v, 0.25f));
    CHECK(!RayRect({0, 0, 0}, {0, 0, 1}, {0, 0, -2}, {1, 0, 0}, {0, 1, 0}, 1.f, 0.5f, u, v, t));

    // Steering wheel: right hand lower than the left = turning right (positive).
    CHECK(WheelAngle({-0.2f, 0.1f, 0}, {0.2f, -0.1f, 0}) > 0.f);
    CHECK(Near(WheelAngle({-0.2f, 0, 0}, {0.2f, 0, 0}), 0.f));

    // FromGame undoes Place's position mapping (pointer dot: game hit point -> tracking space).
    for (const Basis& b : {lh, rh}) {
        Basis yb = Yawed(b, 0.8f);
        Pose p;
        p.p = {0.3f, 1.5f, -0.7f};
        GameCamera c = Place({5, 2, -3}, yb, 30.f, p);
        Vec3 back = FromGame(yb, c.pos - Vec3{5, 2, -3}) * (1.f / 30.f);
        CHECK(Near(back, p.p, 1e-3f));
    }

    // QuatFromBasis reproduces the axes it was built from.
    for (float ang : {0.3f, 2.0f, -2.9f}) {
        Quat q0 = AxisAngle(Normalize({0.3f, 1.f, -0.2f}), ang);
        Quat q1 = QuatFromBasis(Rotate(q0, {1, 0, 0}), Rotate(q0, {0, 1, 0}), Rotate(q0, {0, 0, 1}));
        CHECK(Near(Rotate(q1, {1, 0, 0}), Rotate(q0, {1, 0, 0}), 1e-3f));
        CHECK(Near(Rotate(q1, {0, 0, 1}), Rotate(q0, {0, 0, 1}), 1e-3f));
    }

    // Pose algebra.
    Pose a{AxisAngle({0, 1, 0}, 0.7f), {1, 2, 3}};
    Pose id = Compose(Inverse(a), a);
    CHECK(Near(id.p, Vec3{0, 0, 0}) && Near(std::fabs(id.q.w), 1.f));
    CHECK(Near(YawOf(AxisAngle({0, 1, 0}, 0.5f)), 0.5f));

    if (g_fail == 0) printf("math checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
