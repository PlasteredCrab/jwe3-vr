// OpenXR session on the game's own D3D12 device and queue.
// All functions run on the render thread (inside the Present hook) unless noted.
#pragma once

#include "common.h"
#include "vrmath.h"

namespace jvr::xr {

// Controller state after xrSyncActions. Hand 0 = left, 1 = right.
struct Controls {
    float trigger[2]{}, grip[2]{};
    float stickX[2]{}, stickY[2]{};
    bool stickClick[2]{}, primary[2]{}, secondary[2]{}, menu = false;
    bool active[2]{};
};

enum class SwapId { Left = 0, Right = 1, Screen = 2, Hud = 3, Dot = 4 };

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
void Shutdown();
bool Running();      // session running and the runtime wants frames
int SessionState();  // XrSessionState (5 = focused, 4 = visible, 3 = synchronized)
bool NeedsReinit();  // runtime exited or the session was lost: Shutdown() and Init() again later

// Waits for the next frame (paces the game to the headset), begins it and publishes poses
// (rig::PublishXr) and controls. Returns false if no frame was begun.
bool BeginFrame();

// Creates the eye and screen swapchains to match the game's backbuffer (no-op if they match).
bool EnsureSwapchains(UINT width, UINT height, DXGI_FORMAT backbufferFormat);

// The in-world HUD panel's swapchain: the size and format of the texture the game's UI is drawn into (or its
// sRGB/UNORM twin), so that texture can be copied in as is. False if the runtime has neither format.
bool EnsureHudSwapchain(UINT width, UINT height, DXGI_FORMAT format);

// Acquire/wait/release around a copy into a swapchain image. Image state is RENDER_TARGET.
ID3D12Resource* AcquireImage(SwapId id);
void ReleaseImage(SwapId id);
DXGI_FORMAT SwapFormat();

// Ends the frame begun by BeginFrame. Projection layer from the eye images with the poses and
// FOVs they were rendered with; or the screen quad when showScreen is set (menus, loading).
struct EyeView {
    vm::Pose stagePose;
    vm::Fov fov;
    bool valid = false;
};
struct Layers {
    bool projection = false; // the eye images
    bool screen = false;     // flat screen (menus, tablet)
    bool hud = false;        // in-world HUD panel (UI texture, premultiplied alpha)
    bool dot = false;        // pointer dot at dotPos (stage space)
    vm::Vec3 dotPos;
};
void EndFrame(const EyeView eyes[2], const Layers& layers);
// The pointer dot image (DXGI format of the dot swapchain) and its size in pixels.
DXGI_FORMAT DotFormat();
constexpr UINT kDotSize = 64;
bool HudRect(vm::Vec3& centre, vm::Vec3& right, vm::Vec3& up, float& halfW, float& halfH);

Controls GetControls();          // any thread (copy)
void Haptic(int hand, float amplitude, float seconds, float frequency); // any thread (queued)
// Places the flat screen in front of the user at the configured distance (on next EndFrame).
void PlaceScreen();
// Raw (un-recentred) stage-space aim pose of a hand from the last frame. Any thread.
bool RawAim(int hand, vm::Pose& out);
// Stage-space screen rectangle (for laser picking on the flat screen).
bool ScreenRect(vm::Vec3& centre, vm::Vec3& right, vm::Vec3& up, float& halfW, float& halfH);

} // namespace jvr::xr
