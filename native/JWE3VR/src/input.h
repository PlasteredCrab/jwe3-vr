// VR controllers -> what the game reads: a virtual Xbox pad (XInput slot 0), the mouse cursor
// (laser on the flat screen or into the park), snap/smooth turn and the overview grab-move.
// Lua decides the mode per game tick through the bridge; OnFrame runs on the render thread.
#pragma once

#include "common.h"
#include "vrmath.h"

namespace jvr::input {

enum class PadMode : int { Off = 0, Passthrough = 1 };

enum : uint32_t {
    OvLX = 1 << 0, OvLY = 1 << 1, OvRX = 1 << 2, OvRY = 1 << 3, OvLT = 1 << 4, OvRT = 1 << 5, OvButtons = 1 << 6,
};

struct LuaFlags {
    PadMode pad = PadMode::Off;
    bool turn = false;    // right stick X turns the play space (ground walking)
    bool laser = false;   // right-hand laser drives the mouse cursor into the 3D park (overview)
    bool grab = false;    // grips move/scale/rotate the overview model
    bool fire = false;    // gun seat: the dominant trigger is the left mouse button (JWE3's "Use Equipment")
};

bool InstallHooks();
void SetGameWindow(HWND hwnd);

// Lua thread
void SetFlags(const LuaFlags& f);
// Override pad fields this tick (values: sticks -1..1, triggers 0..1, buttons = XINPUT_GAMEPAD_* bits).
void SetPadOverride(uint32_t mask, float lx, float ly, float rx, float ry, float lt, float rt, uint32_t buttons);
void MouseButton(int button, bool down); // 0 left, 1 right
void KeyTap(WORD vk);

// Render thread, once per frame after xrEndFrame. screenVisible: the flat screen quad is shown;
// hudVisible: the in-world HUD panel is shown (pointing at it drives the cursor over the UI).
void OnFrame(bool screenVisible, bool hudVisible);
// Where the dominant laser met the flat screen or the HUD panel in the last frame (raw stage space), for the
// pointer dot. False when it is on neither. Render thread.
bool ScreenDot(vm::Vec3& stagePos);

// Debug aids for remote tests (render thread, every Present): plays JWE3VR_pad.txt (see input.cpp), and a
// one-line summary of the game's XInput polling and window focus for the log.
void DebugTick();
std::string DebugSummary();

} // namespace jvr::input
