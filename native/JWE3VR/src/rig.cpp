#include "rig.h"
#include "config.h"
#include "log.h"

#include <deque>
#include <mutex>
#include <chrono>

namespace jvr::rig {

using namespace vm;

static std::mutex g_lock;
static XrSnapshot g_snap;            // recentred
static XrSnapshot g_raw;             // as published (stage space)
static Pose g_recenter;              // stage <- recentred
static float g_neutralY = 1.6f;      // head height at the last recenter (m)
static bool g_recentered = false;
static float g_yaw = 0.f;            // snap / smooth turn accumulator (rad)
static float g_aspect = 16.f / 9.f;
static uint64_t g_tick = 0;
static std::deque<RenderRequest> g_requests;
static RenderRequest g_lastRequest;
// When the rig stops ticking (no park: the VR manager is gone, a loading screen), the last request must not keep
// the eyes immersive with a frozen camera: older than this and Present gets none (the flat screen takes over).
static std::chrono::steady_clock::time_point g_lastPushTime{};
constexpr auto kRequestStale = std::chrono::milliseconds(1500);

// Last placement, so hand rays use the same frame as the camera.
static AnchorKind g_kind = AnchorKind::Off;
static Vec3 g_origin;
static Basis g_basis;
static float g_scale = 1.f;
static GameCamera g_camera;
static float g_fovY = 1.f;

// Overview tabletop grab: an extra offset, yaw and scale on top of the game camera.
static Vec3 g_ovOffset;
static float g_ovYaw = 0.f, g_ovScaleMul = 1.f;
static bool g_grab = false;
static int g_grabHands = 0; // bit 0 = left grip, bit 1 = right grip
static bool g_hasFloor = true;
static Vec3 g_grabStart[2];
static Vec3 g_grabOffsetStart;
static float g_grabYawStart = 0.f, g_grabScaleStart = 1.f, g_grabDist = 1.f, g_grabAngle = 0.f;
static float g_grabHandYaw0 = 0.f; // one-grip grab: the gripping hand's yaw when it closed

static Pose Recentred(const Pose& raw) { return Compose(Inverse(g_recenter), raw); }

static void RecenterLocked() {
    if (!g_raw.head.valid) return;
    const Pose& h = g_raw.rawHead;
    float yaw = YawOf(h.q);
    g_recenter.q = AxisAngle({0, 1, 0}, yaw);
    g_recenter.p = {h.p.x, 0.f, h.p.z};
    g_neutralY = h.p.y;
    g_recentered = true;
    LOGI("recentred: yaw %.1f deg, head height %.2f m", yaw * 180.f / kPi, g_neutralY);
}

void Recenter() {
    std::lock_guard<std::mutex> l(g_lock);
    RecenterLocked();
}

void PublishXr(const XrSnapshot& s) {
    std::lock_guard<std::mutex> l(g_lock);
    g_raw = s;
    if (!g_recentered && s.head.valid) RecenterLocked();
    XrSnapshot r = s;
    auto fix = [](TrackedPose& t) {
        if (t.valid) t.pose = Recentred(t.pose);
    };
    fix(r.head);
    for (int i = 0; i < 2; ++i) {
        fix(r.eye[i]);
        fix(r.grip[i]);
        fix(r.aim[i]);
    }
    g_snap = r;
}

void SetRenderAspect(float aspect) {
    std::lock_guard<std::mutex> l(g_lock);
    if (aspect > 0.1f) g_aspect = aspect;
}

void AddYaw(float rad) {
    std::lock_guard<std::mutex> l(g_lock);
    g_yaw += rad;
}

AnchorKind CurrentKind() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_kind;
}

GameCamera LastCamera(float& fovY) {
    std::lock_guard<std::mutex> l(g_lock);
    fovY = g_fovY;
    return g_camera;
}

float CurrentScale() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_scale;
}

// Position of a recentred tracking-space point relative to the play-space origin for this kind.
static Vec3 Relative(AnchorKind kind, Vec3 p) {
    const Settings& cfg = config::Get();
    switch (kind) {
    case AnchorKind::Ground:
        // Floor-relative: real eye height when standing; calibrated game eye height when seated or
        // when the runtime has no floor-level (stage) space.
        if (cfg.seated || !g_hasFloor) return {p.x, p.y - g_neutralY + cfg.standingEyeHeight, p.z};
        return p;
    default:
        // Recentred head sits exactly at the anchor.
        return {p.x, p.y - g_neutralY, p.z};
    }
}

static void UpdateGrabLocked() {
    if (!g_grab || g_kind != AnchorKind::Overview) return;
    const TrackedPose* h = g_snap.grip;
    if (g_grabHands == 1 || g_grabHands == 2) {
        int i = g_grabHands == 1 ? 0 : 1;
        if (!h[i].valid) return;
        Vec3 d = h[i].pose.p - g_grabStart[i];
        g_ovOffset = g_grabOffsetStart - ToGame(g_basis, {d.x, 0.f, d.z}) * g_scale;
        // Grab and twist (first headset test: the user asked to turn the park with one hand too): turning the
        // gripping hand left or right turns the park the same way, as the two-hand turn does. A 10 degree dead
        // zone keeps an ordinary drag from turning it.
        float turn = std::remainder(YawOf(h[i].pose.q) - g_grabHandYaw0, 2.f * kPi);
        const float dead = 10.f * kPi / 180.f;
        turn = std::fabs(turn) <= dead ? 0.f : turn - std::copysign(dead, turn);
        g_ovYaw = g_grabYawStart + turn;
    } else if (g_grabHands == 3 && h[0].valid && h[1].valid) {
        Vec3 a = h[0].pose.p, b = h[1].pose.p;
        Vec3 d = b - a;
        float dist = std::max(Length({d.x, 0, d.z}), 0.05f);
        float angle = std::atan2(d.z, d.x);
        g_ovScaleMul = std::clamp(g_grabScaleStart * g_grabDist / dist, 0.1f, 20.f);
        // Hands turning one way turn the model the same way, i.e. the view the other way.
        g_ovYaw = g_grabYawStart - (angle - g_grabAngle);
        Vec3 mid = (a + b) * 0.5f, mid0 = (g_grabStart[0] + g_grabStart[1]) * 0.5f;
        Vec3 dm = mid - mid0;
        g_ovOffset = g_grabOffsetStart - ToGame(g_basis, {dm.x, 0.f, dm.z}) * g_scale;
    }
}

void OverviewPan(float right, float forward, float dt) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_kind != AnchorKind::Overview || g_grab) return;
    // Flying with the off-hand stick, toward where you look (first headset test: along the rig's facing at 1.5 m/s
    // it was too slow and pointed the wrong way once the view had turned). Squared for fine control near the
    // centre; a full push is 3 m/s over the model (90 m/s of park at 1:30).
    const float realMetresPerSecond = 3.f;
    Vec3 f{0, 0, -1}, r{1, 0, 0};
    if (g_snap.head.valid) {
        f = Rotate(g_snap.head.pose.q, {0, 0, -1});
        r = Rotate(g_snap.head.pose.q, {1, 0, 0});
    }
    f.y = r.y = 0;
    if (Length(f) < 1e-3f || Length(r) < 1e-3f) return;
    f = Normalize(f);
    r = Normalize(r);
    float sr = right * std::fabs(right), sf = forward * std::fabs(forward);
    Vec3 realMove = r * sr + f * sf;
    g_ovOffset = g_ovOffset + ToGame(g_basis, realMove) * (realMetresPerSecond * g_scale * dt);
}

void SetHasFloor(bool hasFloor) {
    std::lock_guard<std::mutex> l(g_lock);
    g_hasFloor = hasFloor;
}

float ViewHeading() {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_snap.head.valid) return g_yaw;
    // Positive = view turned toward the anchor's right (snap turns plus where the head points).
    return g_yaw - YawOf(g_snap.head.pose.q);
}

void SetOverviewGrab(bool active, int hands) {
    std::lock_guard<std::mutex> l(g_lock);
    if (active && (!g_grab || hands != g_grabHands)) {
        for (int i = 0; i < 2; ++i) g_grabStart[i] = g_snap.grip[i].pose.p;
        g_grabOffsetStart = g_ovOffset;
        g_grabYawStart = g_ovYaw;
        g_grabScaleStart = g_ovScaleMul;
        Vec3 d = g_grabStart[1] - g_grabStart[0];
        g_grabDist = std::max(Length({d.x, 0, d.z}), 0.05f);
        g_grabAngle = std::atan2(d.z, d.x);
        if (hands == 1 || hands == 2) g_grabHandYaw0 = YawOf(g_snap.grip[hands == 1 ? 0 : 1].pose.q);
    }
    g_grab = active;
    g_grabHands = active ? hands : 0;
}

TickOutput Tick(const TickInput& in) {
    std::lock_guard<std::mutex> l(g_lock);
    const Settings& cfg = config::Get();
    TickOutput out;
    ++g_tick;
    if (in.kind != g_kind && in.kind == AnchorKind::Overview) {
        // Entering the overview starts from the game's own view each time.
        g_ovOffset = {};
        g_ovYaw = 0.f;
        g_ovScaleMul = 1.f;
    }
    g_kind = in.kind;

    if (!cfg.enabled || !g_snap.valid || !g_snap.head.valid || in.kind == AnchorKind::Off || in.kind == AnchorKind::Screen) {
        g_requests.clear();
        return out;
    }

    bool seat = in.kind == AnchorKind::Seat || in.kind == AnchorKind::Gunner;
    Basis basis = (seat && !cfg.horizonLock) ? in.anchorBasis : Flatten(in.anchorBasis, {0, 1, 0});
    if (!seat) basis = Yawed(basis, g_yaw + (in.kind == AnchorKind::Overview ? g_ovYaw : 0.f));

    float scale = in.kind == AnchorKind::Overview ? cfg.overviewScale * g_ovScaleMul : 1.f;
    Vec3 origin = in.anchorPos;
    if (in.kind == AnchorKind::Ground) origin = origin - Vec3{0, 1, 0} * cfg.standingEyeHeight;
    if (in.kind == AnchorKind::Overview) origin = origin + g_ovOffset;

    g_origin = origin;
    g_basis = basis;
    g_scale = scale;
    UpdateGrabLocked();
    origin = g_origin = (in.kind == AnchorKind::Overview) ? in.anchorPos + g_ovOffset : origin;

    // Which view to render this tick.
    int eye = -1;
    Pose local = g_snap.head.pose;
    Pose stage = g_raw.rawHead;
    if (cfg.stereo == StereoMode::AER && g_snap.eye[0].valid && g_snap.eye[1].valid) {
        eye = (int)(g_tick & 1) ^ (cfg.swapEyes ? 1 : 0);
        Pose e = g_snap.eye[eye].pose;
        e.p = local.p + (e.p - local.p) * cfg.ipdScale;
        // The tick a new IPD scale first applies (pairs with JWE3VR_aer.req to check the present lag in game).
        static float s_lastIpd = -1.f;
        if (cfg.ipdScale != s_lastIpd) {
            if (s_lastIpd >= 0.f) LOGI("rig: IPD scale %.1f from tick %llu (eye %d)", cfg.ipdScale, (unsigned long long)g_tick, eye);
            s_lastIpd = cfg.ipdScale;
        }
        local = e;
        stage = Compose(g_recenter, e);
    }

    Pose rel = local;
    rel.p = Relative(in.kind, local.p);
    GameCamera cam = Place(origin, basis, scale, rel);

    float fovY = EnclosingFovY(g_snap.fov, 2, g_aspect, cfg.cullMarginDeg * kPi / 180.f);
    if (!Finite(cam.pos) || !Finite(cam.basis.f) || !std::isfinite(fovY)) {
        JVR_LOG_ONCE("rig: non-finite camera, skipping override");
        return out;
    }

    out.overrideCamera = true;
    out.camera = cam;
    out.fovY = fovY;
    out.eye = eye;
    g_camera = cam;
    g_fovY = fovY;

    RenderRequest req;
    req.eye = eye;
    req.stagePose = stage;
    req.fovY = fovY;
    req.camera = cam;
    req.tick = g_tick;
    if (eye < 0) {
        // Sync / mono: both eyes keep the head's orientation (that is what the game renders) at their own positions.
        for (int i = 0; i < 2; ++i) {
            Vec3 off = g_snap.eye[i].valid ? (g_snap.eye[i].pose.p - local.p) * cfg.ipdScale : Vec3{};
            req.eyeStage[i] = Compose(g_recenter, Pose{local.q, local.p + off});
            out.eyeOffset[i] = ToGame(basis, off) * scale;
        }
    }
    g_requests.push_back(req);
    g_lastPushTime = std::chrono::steady_clock::now();
    while (g_requests.size() > 8) g_requests.pop_front();

    if (cfg.logPoses && (g_tick % 90) == 0)
        LOGI("tick %llu kind %d eye %d cam (%.2f %.2f %.2f) fwd (%.2f %.2f %.2f) fovY %.1f scale %.1f",
             (unsigned long long)g_tick, (int)in.kind, eye, cam.pos.x, cam.pos.y, cam.pos.z, cam.basis.f.x,
             cam.basis.f.y, cam.basis.f.z, fovY * 180.f / kPi, scale);
    return out;
}

bool TakeRequestForPresent(RenderRequest& out) {
    std::lock_guard<std::mutex> l(g_lock);
    int lag = config::Get().aerPresentLag;
    // After a burst of ticks between two presents, drop the stale ones so the queue length returns to
    // the configured lag; otherwise every later frame would be paired with a request one tick too old.
    while ((int)g_requests.size() > lag + 1) g_requests.pop_front();
    if ((int)g_requests.size() > lag) {
        g_lastRequest = g_requests.front();
        g_requests.pop_front();
        out = g_lastRequest;
        return true;
    }
    if (g_lastRequest.tick == 0 || std::chrono::steady_clock::now() - g_lastPushTime > kRequestStale) return false;
    out = g_lastRequest;
    return true;
}

int UpcomingEye() {
    std::lock_guard<std::mutex> l(g_lock);
    int lag = config::Get().aerPresentLag;
    // Mirrors TakeRequestForPresent: after draining to lag + 1, the front is what the next Present takes.
    size_t n = g_requests.size();
    if ((int)n > lag) {
        size_t skip = (int)n > lag + 1 ? n - (size_t)(lag + 1) : 0;
        return g_requests[skip].eye;
    }
    return g_lastRequest.tick ? g_lastRequest.eye : -1;
}

bool HandRay(int hand, Vec3& origin, Basis& basis) {
    std::lock_guard<std::mutex> l(g_lock);
    if (hand < 0 || hand > 1 || !g_snap.aim[hand].valid || g_kind == AnchorKind::Off) return false;
    Pose p = g_snap.aim[hand].pose;
    p.p = Relative(g_kind, p.p);
    GameCamera c = Place(g_origin, g_basis, g_scale, p);
    origin = c.pos;
    basis = c.basis;
    return true;
}

bool GameToStage(Vec3 game, Vec3& stage) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_kind == AnchorKind::Off || g_kind == AnchorKind::Screen || g_scale <= 0.f) return false;
    Vec3 rel = FromGame(g_basis, game - g_origin) * (1.f / g_scale);
    // Inverse of Relative(): back to recentred tracking space.
    const Settings& cfg = config::Get();
    Vec3 local = rel;
    if (g_kind == AnchorKind::Ground) {
        if (cfg.seated || !g_hasFloor) local.y = rel.y + g_neutralY - cfg.standingEyeHeight;
    } else {
        local.y = rel.y + g_neutralY;
    }
    stage = Compose(g_recenter, Pose{Quat{}, local}).p;
    return Finite(stage);
}

bool HandLocal(int hand, Pose& out) {
    std::lock_guard<std::mutex> l(g_lock);
    if (hand < 0 || hand > 1 || !g_snap.grip[hand].valid) return false;
    out = g_snap.grip[hand].pose;
    out.p = Relative(g_kind == AnchorKind::Off ? AnchorKind::Seat : g_kind, out.p);
    return true;
}

} // namespace jvr::rig
