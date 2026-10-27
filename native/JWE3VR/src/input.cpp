#include "input.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "rig.h"
#include "xr.h"

#include <Xinput.h>

namespace jvr::input {

using namespace vm;

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
static GetStateFn o_GetState;
static GetCapsFn o_GetCaps;

static std::mutex g_lock;
static LuaFlags g_flags;
static XINPUT_GAMEPAD g_pad{};
static DWORD g_packet = 1;
static bool g_padLive = false;
static HWND g_hwnd = nullptr;

struct Override {
    uint32_t mask = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    uint32_t buttons = 0;
    ULONGLONG time = 0;
};
static Override g_override;

static bool g_turnLatched = false;
static ULONGLONG g_lastFrame = 0;
static bool g_mouseDown[2]{};
static Vec3 g_cursorSmooth;
static bool g_cursorInit = false;

static SHORT Axis(float v) { return (SHORT)std::clamp(v * 32767.f, -32767.f, 32767.f); }
static BYTE Trig(float v) { return (BYTE)std::clamp(v * 255.f, 0.f, 255.f); }

static DWORD WINAPI Hk_GetState(DWORD index, XINPUT_STATE* state) {
    if (index == 0 && config::Get().virtualPad) {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_padLive && g_flags.pad == PadMode::Passthrough && state) {
            state->dwPacketNumber = g_packet;
            state->Gamepad = g_pad;
            return ERROR_SUCCESS;
        }
    }
    return o_GetState(index, state);
}

static DWORD WINAPI Hk_GetCaps(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps) {
    if (index == 0 && config::Get().virtualPad && g_padLive && caps) {
        DWORD r = o_GetCaps(index, flags, caps);
        if (r == ERROR_SUCCESS) return r;
        memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        return ERROR_SUCCESS;
    }
    return o_GetCaps(index, flags, caps);
}

bool InstallHooks() {
    HMODULE x = GetModuleHandleW(L"xinput1_4.dll");
    if (!x) x = LoadLibraryW(L"xinput1_4.dll");
    if (!x) {
        LOGE("xinput1_4.dll not found; no virtual pad");
        return false;
    }
    bool ok = hooks::Create("XInputGetState", (void*)GetProcAddress(x, "XInputGetState"), (void*)Hk_GetState, (void**)&o_GetState);
    ok &= hooks::Create("XInputGetCapabilities", (void*)GetProcAddress(x, "XInputGetCapabilities"), (void*)Hk_GetCaps,
                        (void**)&o_GetCaps);
    return ok;
}

void SetGameWindow(HWND hwnd) { g_hwnd = hwnd; }

void SetFlags(const LuaFlags& f) {
    std::lock_guard<std::mutex> l(g_lock);
    g_flags = f;
}

void SetPadOverride(uint32_t mask, float lx, float ly, float rx, float ry, float lt, float rt, uint32_t buttons) {
    std::lock_guard<std::mutex> l(g_lock);
    g_override = {mask, lx, ly, rx, ry, lt, rt, buttons, GetTickCount64()};
}

// Simulated clicks and cursor moves only while the game has focus (SteamVR or a browser can take it).
static bool GameFocused() {
    if (!g_hwnd) return false;
    HWND fg = GetForegroundWindow();
    return fg && (fg == g_hwnd || fg == GetAncestor(g_hwnd, GA_ROOT));
}

static void SendMouse(DWORD flags, int wheel = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = (DWORD)wheel;
    SendInput(1, &in, sizeof(in));
}

void MouseButton(int button, bool down) {
    button &= 1;
    if (g_mouseDown[button] == down) return;
    if (down && !GameFocused()) return; // a release always goes out, a press only into the game
    g_mouseDown[button] = down;
    if (button == 0) SendMouse(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    else SendMouse(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
}

void KeyTap(WORD vk) {
    if (!GameFocused()) return;
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// Moves the OS cursor to a UV position of the game window's client area.
static void CursorToUV(float u, float v) {
    if (!GameFocused()) return;
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return;
    POINT p{(LONG)(std::clamp(u, 0.f, 1.f) * (rc.right - 1)), (LONG)(std::clamp(v, 0.f, 1.f) * (rc.bottom - 1))};
    ClientToScreen(g_hwnd, &p);
    SetCursorPos(p.x, p.y);
}

static void SmoothCursor(float u, float v, float dt) {
    Vec3 uv{u, v, 0};
    if (!g_cursorInit) g_cursorSmooth = uv, g_cursorInit = true;
    g_cursorSmooth = Lerp(g_cursorSmooth, uv, SmoothAlpha(config::Get().laserSmoothing, dt));
    CursorToUV(g_cursorSmooth.x, g_cursorSmooth.y);
}

// Laser from a hand onto the flat screen or the HUD panel (both live in raw stage space).
static bool ScreenPointer(int hand, float dt, bool hud = false) {
    Vec3 centre, right, up;
    float hw, hh, u, v, t;
    Pose aim;
    bool rect = hud ? xr::HudRect(centre, right, up, hw, hh) : xr::ScreenRect(centre, right, up, hw, hh);
    if (!rect || !xr::RawAim(hand, aim)) return false;
    Vec3 dir = Rotate(aim.q, {0, 0, -1});
    if (!RayRect(aim.p, dir, centre, right, up, hw, hh, u, v, t)) return false;
    SmoothCursor(u, v, dt);
    return true;
}

void OnFrame(bool screenVisible, bool hudVisible) {
    const Settings& cfg = config::Get();
    ULONGLONG now = GetTickCount64();
    float dt = g_lastFrame ? std::min((now - g_lastFrame) / 1000.f, 0.1f) : 0.f;
    g_lastFrame = now;

    xr::Controls c = xr::GetControls();
    int dom = cfg.leftHanded ? 0 : 1, off = 1 - dom;
    LuaFlags f;
    Override ov;
    {
        std::lock_guard<std::mutex> l(g_lock);
        f = g_flags;
        ov = g_override;
    }

    // Turning the play space with the dominant stick (ground walking).
    float turnX = c.stickX[dom];
    if (f.turn) {
        if (cfg.snapTurnDeg > 0) {
            if (!g_turnLatched && std::fabs(turnX) > 0.7f) {
                rig::AddYaw((turnX > 0 ? 1.f : -1.f) * cfg.snapTurnDeg * kPi / 180.f);
                g_turnLatched = true;
            } else if (std::fabs(turnX) < 0.3f) {
                g_turnLatched = false;
            }
        } else if (std::fabs(turnX) > 0.15f) {
            rig::AddYaw(turnX * cfg.smoothTurnDegPerSec * kPi / 180.f * dt);
        }
    }

    // Overview: grips move the model.
    if (f.grab) {
        bool l = c.grip[0] > 0.6f, r = c.grip[1] > 0.6f;
        rig::SetOverviewGrab(l || r, (l ? 1 : 0) | (r ? 2 : 0));
        float sx = c.stickX[off], sy = c.stickY[off];
        if (std::fabs(sx) > 0.15f || std::fabs(sy) > 0.15f) rig::OverviewPan(sx, sy, dt);
    } else {
        rig::SetOverviewGrab(false, 0);
    }

    // Pointer: flat screen (menus, tablet) or into the park (overview laser).
    bool pointing = false;
    if (screenVisible) pointing = ScreenPointer(dom, dt);
    // The in-world HUD takes the pointer when the laser is on (park overview) and pointing at it: UI first,
    // then the park. In seats, guns and walking the trigger belongs to the game, so the HUD is look-only.
    if (!pointing && hudVisible && f.laser) pointing = ScreenPointer(dom, dt, true);
    if (f.laser && !screenVisible && !pointing) {
        // The game picks with its own cursor ray; Lua replaces that ray with the hand's, and the OS cursor
        // follows a far point on the laser so 2D UI under it reacts too.
        Vec3 o;
        Basis b;
        float fovY = 1.f;
        GameCamera cam = rig::LastCamera(fovY);
        if (rig::HandRay(dom, o, b)) {
            Vec3 target = o + b.f * (50.f * rig::CurrentScale());
            float u, v, aspect = 16.f / 9.f;
            RECT rc;
            if (g_hwnd && GetClientRect(g_hwnd, &rc) && rc.bottom > 0) aspect = (float)rc.right / (float)rc.bottom;
            if (ProjectUV(cam, fovY, aspect, target, u, v)) {
                SmoothCursor(u, v, dt);
                pointing = true;
            }
        }
    }
    if (pointing) {
        MouseButton(0, c.trigger[dom] > 0.6f);
        MouseButton(1, c.grip[dom] > 0.8f && !f.grab);
    } else {
        MouseButton(0, false);
        MouseButton(1, false);
    }

    // Virtual pad: the VR controllers mirrored onto an Xbox layout, then Lua's overrides.
    XINPUT_GAMEPAD p{};
    if (c.active[0] || c.active[1]) {
        float lx = c.stickX[off], ly = c.stickY[off];
        if (f.turn) {
            // Walking: the game moves along its own (frozen) camera yaw, so turn the stick into where
            // you are actually looking (snap turns + head yaw).
            float a = rig::ViewHeading(), ca = std::cos(a), sa = std::sin(a);
            float ax = lx * ca + ly * sa, ay = -lx * sa + ly * ca;
            lx = ax;
            ly = ay;
        }
        p.sThumbLX = Axis(lx);
        p.sThumbLY = Axis(ly);
        p.sThumbRX = Axis(f.turn ? 0.f : c.stickX[dom]);
        p.sThumbRY = Axis(c.stickY[dom]);
        p.bLeftTrigger = Trig(c.trigger[off]);
        p.bRightTrigger = Trig(c.trigger[dom]);
        WORD b = 0;
        if (c.primary[dom]) b |= XINPUT_GAMEPAD_A;
        if (c.secondary[dom]) b |= XINPUT_GAMEPAD_B;
        if (c.primary[off]) b |= XINPUT_GAMEPAD_X;
        if (c.secondary[off]) b |= XINPUT_GAMEPAD_Y;
        if (c.grip[off] > 0.6f) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (c.grip[dom] > 0.6f) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (c.stickClick[off]) b |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (c.stickClick[dom]) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
        if (c.menu) b |= (c.grip[off] > 0.6f) ? XINPUT_GAMEPAD_BACK : XINPUT_GAMEPAD_START;
        p.wButtons = b;
    }
    if (ov.mask && now - ov.time < 250) {
        if (ov.mask & OvLX) p.sThumbLX = Axis(ov.lx);
        if (ov.mask & OvLY) p.sThumbLY = Axis(ov.ly);
        if (ov.mask & OvRX) p.sThumbRX = Axis(ov.rx);
        if (ov.mask & OvRY) p.sThumbRY = Axis(ov.ry);
        if (ov.mask & OvLT) p.bLeftTrigger = Trig(ov.lt);
        if (ov.mask & OvRT) p.bRightTrigger = Trig(ov.rt);
        if (ov.mask & OvButtons) p.wButtons = (WORD)ov.buttons;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (memcmp(&p, &g_pad, sizeof(p)) != 0) ++g_packet;
        g_pad = p;
        g_padLive = cfg.virtualPad && (c.active[0] || c.active[1]);
    }
}

} // namespace jvr::input
