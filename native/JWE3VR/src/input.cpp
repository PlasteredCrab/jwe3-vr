#include "input.h"
#include "config.h"
#include "hooks.h"
#include "log.h"
#include "rig.h"
#include "xr.h"

#include <Xinput.h>

#include <cstdio>

namespace jvr::input {

using namespace vm;

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);

// The game may read the pad through any XInput DLL in the process: JWE3 imports xinput1_4 but also has xinput9_1_0
// loaded (its own implementation, not a forwarder), and DirectInput 8. Each DLL gets the same hooks; the call
// counts per DLL (DebugSummary) tell which one the game polls.
constexpr int kXInputDlls = 3;
static const wchar_t* const kXInputNames[kXInputDlls] = {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"};
static const char* const kXInputShort[kXInputDlls] = {"1_4", "9_1_0", "1_3"};
static GetStateFn o_GetState[kXInputDlls];
static GetCapsFn o_GetCaps[kXInputDlls];
static GetKeystrokeFn o_GetKeystroke[kXInputDlls];
static std::atomic<unsigned> g_dllStateCalls[kXInputDlls], g_dllCapsCalls[kXInputDlls], g_dllKeyCalls[kXInputDlls];
static std::atomic<ULONGLONG> g_lastState0{0}, g_lastAnnounce{0}; // the game read slot 0 / we announced it

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

// Debug aid for remote tests: a file JWE3VR_pad.txt next to the DLL is played step by step, one per line:
//   <actions> <ms>    actions joined with '+', held for <ms>:
//     pad 0 buttons     A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS LT RT, sticks LX=/LY=/RX=/RY= -1..1
//     window messages   K=ENTER|ESC|UP|DOWN|LEFT|RIGHT|SPACE|TAB|BACK|0xNN (key down at the start, up at the end),
//                       CLICK=x,y (left click at x,y as fractions of the client area, down then up)
//     FOCUS             post the window-activation messages (the game ignores input while it thinks it is
//                       in the background; real focus is not changed)
//     '-'               nothing
// The file is consumed when read. Steps advance on the render thread (DebugTick), so the script runs even when
// the game does not poll pad 0; while it runs pad 0 reports itself connected. Keys and clicks are posted to the
// game's own window from inside the process (no SendInput).
struct PadStep {
    XINPUT_GAMEPAD pad{};
    std::vector<WPARAM> keys;
    float clickX = -1, clickY = -1;
    bool focus = false;
    ULONGLONG ms = 0;
};
static std::vector<PadStep> g_script;
static size_t g_scriptStep = 0;
static ULONGLONG g_scriptStepAt = 0;
static ULONGLONG g_scriptCheck = 0;
static DWORD g_scriptPacket = 0x40000000;
static std::atomic<unsigned> g_getStateCalls[4]{}, g_getCapsCalls{0};

static bool ParsePadStep(const std::string& line, PadStep& s) {
    char what[256] = {};
    unsigned ms = 0;
    if (sscanf_s(line.c_str(), "%255s %u", what, (unsigned)sizeof(what), &ms) != 2) return false;
    s.ms = ms;
    std::string all = what;
    size_t at = 0;
    while (at <= all.size()) {
        size_t plus = all.find('+', at);
        std::string t = all.substr(at, plus == std::string::npos ? std::string::npos : plus - at);
        at = plus == std::string::npos ? all.size() + 1 : plus + 1;
        float v = 0, w = 0;
        unsigned vk = 0;
        static const struct { const char* name; WORD bit; } kButtons[] = {
            {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
            {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK}, {"UP", XINPUT_GAMEPAD_DPAD_UP},
            {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
            {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"LS", XINPUT_GAMEPAD_LEFT_THUMB},
            {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}};
        static const struct { const char* name; WPARAM vk; } kKeys[] = {
            {"K=ENTER", VK_RETURN}, {"K=ESC", VK_ESCAPE}, {"K=UP", VK_UP}, {"K=DOWN", VK_DOWN}, {"K=LEFT", VK_LEFT},
            {"K=RIGHT", VK_RIGHT}, {"K=SPACE", VK_SPACE}, {"K=TAB", VK_TAB}, {"K=BACK", VK_BACK}};
        bool known = t == "-" || t.empty();
        for (auto& b : kButtons)
            if (t == b.name) s.pad.wButtons |= b.bit, known = true;
        for (auto& k : kKeys)
            if (t == k.name) s.keys.push_back(k.vk), known = true;
        if (!known && sscanf_s(t.c_str(), "K=0x%x", &vk) == 1) s.keys.push_back(vk), known = true;
        if (t == "LT") s.pad.bLeftTrigger = 255, known = true;
        if (t == "RT") s.pad.bRightTrigger = 255, known = true;
        if (sscanf_s(t.c_str(), "LX=%f", &v) == 1) s.pad.sThumbLX = Axis(v), known = true;
        if (sscanf_s(t.c_str(), "LY=%f", &v) == 1) s.pad.sThumbLY = Axis(v), known = true;
        if (sscanf_s(t.c_str(), "RX=%f", &v) == 1) s.pad.sThumbRX = Axis(v), known = true;
        if (sscanf_s(t.c_str(), "RY=%f", &v) == 1) s.pad.sThumbRY = Axis(v), known = true;
        if (sscanf_s(t.c_str(), "CLICK=%f,%f", &v, &w) == 2) s.clickX = v, s.clickY = w, known = true;
        if (t == "FOCUS") s.focus = true, known = true;
        if (!known) return false;
    }
    return true;
}

static LPARAM KeyLParam(WPARAM vk, bool up) {
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LPARAM l = 1 | ((LPARAM)scan << 16);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) l |= (LPARAM)1 << 24; // extended key
    if (up) l |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    return l;
}

static LPARAM ClientPoint(float fx, float fy) {
    RECT r{};
    GetClientRect(g_hwnd, &r);
    int x = (int)(fx * (r.right - r.left)), y = (int)(fy * (r.bottom - r.top));
    return MAKELPARAM(x, y);
}

// Window messages of a step: at its start (down) and at its end (up). Caller holds g_lock.
static void StepMessages(const PadStep& s, bool start) {
    if (!g_hwnd) return;
    if (start && s.focus) {
        PostMessageW(g_hwnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageW(g_hwnd, WM_NCACTIVATE, TRUE, 0);
        PostMessageW(g_hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageW(g_hwnd, WM_SETFOCUS, 0, 0);
    }
    for (WPARAM vk : s.keys) PostMessageW(g_hwnd, start ? WM_KEYDOWN : WM_KEYUP, vk, KeyLParam(vk, !start));
    if (s.clickX >= 0) {
        LPARAM pt = ClientPoint(s.clickX, s.clickY);
        if (start) {
            PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, pt);
            PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt);
        } else {
            PostMessageW(g_hwnd, WM_LBUTTONUP, 0, pt);
        }
    }
}

static void LoadPadScript(ULONGLONG now) {
    std::wstring path = ModuleDir() + L"JWE3VR_pad.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return;
    std::vector<PadStep> steps;
    char buf[512];
    int lineNo = 0, bad = 0;
    while (fgets(buf, sizeof(buf), f)) {
        ++lineNo;
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        PadStep s;
        if (ParsePadStep(line, s)) steps.push_back(s);
        else ++bad, LOGW("debug: pad script line %d not understood: %s", lineNo, line.c_str());
    }
    fclose(f);
    DeleteFileW(path.c_str());
    if (!g_script.empty() && g_scriptStep < g_script.size()) StepMessages(g_script[g_scriptStep], false);
    g_script = std::move(steps);
    g_scriptStep = 0;
    g_scriptStepAt = now;
    if (!g_script.empty()) StepMessages(g_script[0], true);
    LOGI("debug: pad script loaded, %zu steps (%d lines skipped)", g_script.size(), bad);
}

void DebugTick() {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> l(g_lock);
    if (now - g_scriptCheck > 500) {
        g_scriptCheck = now;
        LoadPadScript(now);
    }
    while (g_scriptStep < g_script.size() && now - g_scriptStepAt >= g_script[g_scriptStep].ms) {
        StepMessages(g_script[g_scriptStep], false);
        g_scriptStepAt += g_script[g_scriptStep].ms;
        ++g_scriptStep;
        ++g_scriptPacket;
        if (g_scriptStep == g_script.size()) {
            LOGI("debug: pad script finished");
            g_script.clear();
            g_scriptStep = 0;
        } else {
            StepMessages(g_script[g_scriptStep], true);
        }
    }
}

std::string DebugSummary() {
    char buf[256];
    HWND fg = GetForegroundWindow();
    snprintf(buf, sizeof(buf), "XInputGetState calls %u/%u/%u/%u, GetCapabilities %u (by DLL, state/caps/keystroke: ", g_getStateCalls[0].load(),
             g_getStateCalls[1].load(), g_getStateCalls[2].load(), g_getStateCalls[3].load(), g_getCapsCalls.load());
    std::string s = buf;
    for (int i = 0; i < kXInputDlls; ++i) {
        snprintf(buf, sizeof(buf), "%s%s %u/%u/%u", i ? ", " : "", kXInputShort[i], g_dllStateCalls[i].load(), g_dllCapsCalls[i].load(),
                 g_dllKeyCalls[i].load());
        s += buf;
    }
    s += std::string("); game window ") + (!g_hwnd ? "unknown" : (fg == g_hwnd ? "in the foreground" : "NOT in the foreground"));
    return s;
}

// Fills pad 0 from the running script; false when no script is playing.
static bool ScriptedPad(XINPUT_STATE* state) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_script.empty()) return false;
    if (state) {
        state->dwPacketNumber = g_scriptPacket;
        state->Gamepad = g_script[g_scriptStep].pad;
    }
    return true;
}

static DWORD PadState(DWORD index, XINPUT_STATE* state, GetStateFn original) {
    if (index < 4) ++g_getStateCalls[index];
    if (index == 0) g_lastState0 = GetTickCount64();
    if (index == 0 && ScriptedPad(state)) return ERROR_SUCCESS;
    if (index == 0 && config::Get().virtualPad) {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_padLive && state) {
            // Connected whenever the controllers are: the pad's input only while Lua passes it through, else at
            // rest (a pad that came and went would be dropped and found again by the game every second).
            bool through = g_flags.pad == PadMode::Passthrough;
            state->dwPacketNumber = through ? g_packet : 1;
            state->Gamepad = through ? g_pad : XINPUT_GAMEPAD{};
            return ERROR_SUCCESS;
        }
    }
    return original(index, state);
}

static bool PadWanted() {
    std::lock_guard<std::mutex> l(g_lock);
    return !g_script.empty() || (config::Get().virtualPad && g_padLive);
}

// JWE3 finds an Xbox pad by its keystroke events: every frame it drains XInputGetKeystroke(XUSER_INDEX_ANY), and a
// keystroke from a user whose slot is empty makes it probe that slot (XInputGetCapabilities) and read it with
// XInputGetState from then on (0x1417ff9c0 / 0x1417ffeb0 in build 1792088119). It never polls an empty slot, so
// while the virtual pad should be connected but slot 0 is not being read, one keystroke announces it: no virtual
// key (the game handles only VK_PAD_A..VK_PAD_RTHUMB_PRESS), key up, user 0. At most once a second.
static DWORD PadKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE ks, GetKeystrokeFn original) {
    if (ks && (index == XUSER_INDEX_ANY || index == 0) && PadWanted()) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastState0.load() > 500 && now - g_lastAnnounce.load() > 1000) {
            g_lastAnnounce = now;
            memset(ks, 0, sizeof(*ks));
            ks->Flags = XINPUT_KEYSTROKE_KEYUP;
            ks->UserIndex = 0;
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4) LOGI("virtual pad: announced on XInput slot 0 (the game finds pads by their keystrokes)");
            return ERROR_SUCCESS;
        }
    }
    return original(index, reserved, ks);
}

static DWORD PadCaps(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps, GetCapsFn original) {
    ++g_getCapsCalls;
    bool scripted = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        scripted = !g_script.empty();
    }
    if (index == 0 && (scripted || (config::Get().virtualPad && g_padLive)) && caps) {
        DWORD r = original(index, flags, caps);
        if (r == ERROR_SUCCESS) return r;
        memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        return ERROR_SUCCESS;
    }
    return original(index, flags, caps);
}

template <int N> static DWORD WINAPI Hk_GetState(DWORD index, XINPUT_STATE* state) {
    ++g_dllStateCalls[N];
    return PadState(index, state, o_GetState[N]);
}

template <int N> static DWORD WINAPI Hk_GetCaps(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps) {
    ++g_dllCapsCalls[N];
    return PadCaps(index, flags, caps, o_GetCaps[N]);
}

template <int N> static DWORD WINAPI Hk_GetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE ks) {
    ++g_dllKeyCalls[N];
    return PadKeystroke(index, reserved, ks, o_GetKeystroke[N]);
}

template <int N> static bool HookXInput(HMODULE m) {
    std::string name = "XInputGetState (xinput" + std::string(kXInputShort[N]) + ")";
    bool ok = hooks::Create(name.c_str(), (void*)GetProcAddress(m, "XInputGetState"), (void*)&Hk_GetState<N>, (void**)&o_GetState[N]);
    name = "XInputGetCapabilities (xinput" + std::string(kXInputShort[N]) + ")";
    ok &= hooks::Create(name.c_str(), (void*)GetProcAddress(m, "XInputGetCapabilities"), (void*)&Hk_GetCaps<N>, (void**)&o_GetCaps[N]);
    if (void* key = (void*)GetProcAddress(m, "XInputGetKeystroke")) { // not in xinput9_1_0
        name = "XInputGetKeystroke (xinput" + std::string(kXInputShort[N]) + ")";
        hooks::Create(name.c_str(), key, (void*)&Hk_GetKeystroke<N>, (void**)&o_GetKeystroke[N]);
    }
    return ok;
}

bool InstallHooks() {
    // System DLLs only (System32): load the two Windows ships so their exports are hooked even if the game loads
    // them later; xinput1_3 (DirectX redistributable) only if the game already has it.
    bool any = false;
    HMODULE m[kXInputDlls];
    for (int i = 0; i < kXInputDlls; ++i) {
        m[i] = GetModuleHandleW(kXInputNames[i]);
        if (!m[i] && i < 2) {
            wchar_t path[MAX_PATH];
            GetSystemDirectoryW(path, MAX_PATH);
            m[i] = LoadLibraryW((std::wstring(path) + L"\\" + kXInputNames[i]).c_str());
        }
    }
    if (m[0]) any |= HookXInput<0>(m[0]);
    if (m[1]) any |= HookXInput<1>(m[1]);
    if (m[2]) any |= HookXInput<2>(m[2]);
    if (!any) LOGE("no XInput DLL could be hooked; no virtual pad");
    return any;
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
