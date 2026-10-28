// The play-space rig: turns the game camera's anchor (from Lua, once per game tick) plus the
// predicted OpenXR poses (from the render thread, once per frame) into the camera the game renders.
//
// Threads: the render thread (Present hook) publishes poses with PublishXr(); the game's Lua
// thread calls Tick() through the bridge. Everything shared is guarded by one mutex.
#pragma once

#include "vrmath.h"

#include <cstdint>

namespace jvr::rig {

// How Lua wants the anchor interpreted. Values match JWE3VR's Lua side (jwe3vr.bridge.lua).
enum class AnchorKind : int {
    Off = 0,       // VR camera off: the game camera is left alone, the frame shows on a flat screen
    Overview = 1,  // park overview: giant scale, recentred head sits at the game camera
    Ground = 2,    // guest walk: anchor is the guest eye (1.67 m); your real eye height replaces it
    Seat = 3,      // vehicle seat: anchor is the seat eye point
    Gunner = 4,    // gun seat: like Seat; the gun follows the aim hand
    Screen = 5,    // menus / loading / cinematics: flat screen, no camera override
};

struct TrackedPose {
    vm::Pose pose;
    bool valid = false;
};

// Poses for one predicted display time, in recentred tracking space (metres).
struct XrSnapshot {
    int64_t displayTime = 0;
    TrackedPose head;
    TrackedPose eye[2];
    vm::Fov fov[2];
    TrackedPose grip[2];
    TrackedPose aim[2];
    vm::Pose rawEye[2]; // un-recentred stage-space eye poses (what xrEndFrame wants)
    vm::Pose rawHead;
    bool valid = false;
};

// One camera the game was asked to render (AER: one eye per tick).
struct RenderRequest {
    int eye = -1;              // 0 left, 1 right, -1 centre (mono / sync)
    vm::Pose stagePose;        // stage-space pose of the rendered view (for xrEndFrame)
    float fovY = 1.f;          // symmetric vertical FOV given to the game
    vm::GameCamera camera;     // game-space camera (for discovery and projection)
    vm::Pose eyeStage[2];      // sync mode: each eye at its own position with the head's orientation
    uint64_t tick = 0;
};

struct TickInput {
    AnchorKind kind = AnchorKind::Off;
    vm::Vec3 anchorPos;
    vm::Basis anchorBasis;
    float dt = 0.f;
};

struct TickOutput {
    bool overrideCamera = false;
    vm::GameCamera camera;   // what Lua passes to SetTransform
    float fovY = 1.f;        // what Lua passes to SetFOVY
    vm::Vec3 eyeOffset[2];   // sync mode: game-space offset of each eye from `camera.pos`
    int eye = -1;            // AER: the eye `camera` is (0/1); -1 for a centre camera
};

// Render thread: new predicted poses after xrWaitFrame + xrLocateViews.
void PublishXr(const XrSnapshot& s);
// Render thread: aspect (width/height) of the frame the game renders.
void SetRenderAspect(float aspect);

// Lua thread: once per game tick.
TickOutput Tick(const TickInput& in);

// Render thread: the request that matches the frame being presented now (honours AERPresentLag).
bool TakeRequestForPresent(RenderRequest& out);
// Any thread: the eye (0/1) of the request the next Present will take, i.e. the eye of the frame being
// recorded now, with the same pairing the user calibrated with AERPresentLag. -1 if unknown.
int UpcomingEye();

// Game-space ray of a hand's aim pose using the last anchor. Returns false if not tracked.
bool HandRay(int hand, vm::Vec3& origin, vm::Basis& basis);
// Tracking-space (recentred) grip pose relative to the current seat/play origin, for gestures.
bool HandLocal(int hand, vm::Pose& out);

// Game-space point -> raw stage space (inverse of the camera placement), for world-placed layers
// such as the pointer dot. Returns false before the first placement.
bool GameToStage(vm::Vec3 game, vm::Vec3& stage);

void Recenter();
void AddYaw(float rad);          // snap / smooth turn
void SetOverviewGrab(bool active, int handMask); // tabletop grab-move: bit 0 left grip, bit 1 right grip
void SetHasFloor(bool hasFloor);                 // false when the runtime only offers a LOCAL space
float ViewHeading();                             // radians the view is turned right of the anchor (walking)
void OverviewPan(float right, float forward, float dt); // stick flight over the model (-1..1)
// Off-hand trigger in the overview: fly toward where that hand points (amount = trigger, 0..1).
void OverviewFly(int hand, float amount, float dt);
AnchorKind CurrentKind();
vm::GameCamera LastCamera(float& fovY);
float CurrentScale();

} // namespace jvr::rig
