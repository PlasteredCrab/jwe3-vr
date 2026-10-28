#include "bridge.h"
#include "common.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "passes.h"
#include "render.h"
#include "rig.h"
#include "scan.h"
#include "viewcb.h"
#include "xr.h"

#include <map>

namespace jvr::bridge {

using namespace vm;

enum Op : int {
    OpStatus = 0,      // () -> protocol, xrRunning, enabled, stereoMode
    OpTick = 1,        // (kind, dt, pos3, R3, U3, F3) -> override, pos3, F3, U3, fovY
    OpHand = 2,        // (hand) -> valid, origin3, forward3, up3
    OpControls = 3,    // () -> trigL, trigR, gripL, gripR, sxL, syL, sxR, syR, buttonBits, seconds (wall clock)
    OpFlags = 4,       // (padMode, turn, laser, grab, tablet, fire)
    OpPad = 5,         // (mask, lx, ly, rx, ry, lt, rt, buttons)
    OpHaptic = 6,      // (hand, amplitude, seconds, frequency)
    OpMouse = 7,       // (button, down)
    OpKey = 8,         // (virtualKey)
    OpConfig = 9,      // (index) -> value
    OpRecenter = 10,   // ()
    OpWheel = 11,      // () -> valid, steeringAngleRad
    OpHandLocal = 12,  // (hand) -> valid, x, y, z   (seat/play-space metres; x right, y up, z back)
    OpLog = 13,        // (code, a, b, c)
    OpPlaceScreen = 14, // ()
    OpHudStatus = 15,  // () -> worldHudActive (the game's UI is being shown as a panel in the world)
    OpPointer = 16,    // (visible, x, y, z)  game-space pointer dot (laser hit / rifle sight)
    OpLogText = 17,    // (final, c1..c30)  text from Lua as character codes; logged with the lua_State when final
    OpDebugCommand = 18 // () -> command (render::TakeDebugCommand: 0 none, 1 drive the nearest jeep, 2 helicopter)
};

constexpr int kProtocol = 1;
constexpr size_t kSlot = 0x18;
constexpr int kTagFloat = 3;
constexpr int kTagInt = 0x13;
constexpr int kTagBool = 1;
constexpr int kTagLightC = 0x16;

static volatile int* g_counter = nullptr;

void Init() {
    // mov ecx,[rip+a] ; inc ecx ; mov [rip+a],ecx -- the most common target is the counter.
    std::map<uintptr_t, int> hits;
    scan::ForEachPattern("8B 0D ?? ?? ?? ?? FF C1 89 0D ?? ?? ?? ??", [&](uint8_t* p) {
        uintptr_t a = (uintptr_t)(p + 6) + *(int32_t*)(p + 2);
        uintptr_t b = (uintptr_t)(p + 14) + *(int32_t*)(p + 10);
        if (a == b) ++hits[a];
    });
    int best = 0;
    for (auto& [addr, n] : hits)
        if (n > best) best = n, g_counter = (volatile int*)addr;
    if (g_counter) LOGI("Lua stack counter at %p (%d uses)", (void*)g_counter, best);
    else LOGW("Lua stack counter not found; pushing results without it");
}

struct Stack {
    uint8_t* L;
    uint8_t* func;
    int nargs;
    int pushed = 0;

    float Num(int i) const {
        if (i < 1 || i > nargs) return 0.f;
        uint8_t* s = func + kSlot * i;
        int tag = *(int*)(s + 0x10);
        if (tag == kTagFloat) return *(float*)s;
        if (tag == kTagInt) return (float)*(int32_t*)s;
        if (tag == kTagBool) return *(int32_t*)s ? 1.f : 0.f;
        return 0.f;
    }
    int Int(int i) const { return (int)std::lround(Num(i)); }
    Vec3 V3(int i) const { return {Num(i), Num(i + 1), Num(i + 2)}; }

    void Push(float v) {
        uint8_t*& top = *(uint8_t**)(L + 0x10);
        memset(top, 0, kSlot);
        *(float*)top = v;
        *(int*)(top + 0x10) = kTagFloat;
        top += kSlot;
        if (g_counter) {
            int c = ++*g_counter;
            *(int*)(L - 0x10) = c;
        }
        ++pushed;
    }
    void Push(Vec3 v) {
        Push(v.x);
        Push(v.y);
        Push(v.z);
    }
};

static bool Valid(uint8_t* L, Stack& s) {
    uint8_t* top = *(uint8_t**)(L + 0x10);
    uint8_t* ci = *(uint8_t**)(L + 0x20);
    if (!top || !ci) return false;
    uint8_t* func = *(uint8_t**)ci;
    if (!func || top <= func || (size_t)(top - func) % kSlot != 0) return false;
    // Layout self-check: the func slot must hold this very function as a light C function
    // (what package.loadlib pushes). If the game's Lua layout differs, nothing is written.
    if (*(int*)(func + 0x10) != kTagLightC || *(void**)func != (void*)&jwe3vr_lua) return false;
    s.L = L;
    s.func = func;
    s.nargs = (int)((top - func) / kSlot) - 1;
    return s.nargs >= 1 && s.nargs <= 32;
}

// Milestone codes the Lua side reports through OpLog (jwe3vr.bridge.lua Bridge.M), so a run can be
// followed in JWE3VR.log without the game's own Lua trace.
static const char* MilestoneName(int code) {
    switch (code) {
    case 100: return "hooks installed (require wrapped)";
    case 101: return "environment loader patched";
    case 102: return "VR manager added to the park environment";
    case 103: return "camera manager patched";
    case 104: return "drive mode patched";
    case 105: return "gunner mode patched";
    case 106: return "VR manager Init (park loaded)";
    case 107: return "camera mode kind (a)";
    case 108: return "gameplay init done";
    case 109: return "lua error (a = where)";
    default: return nullptr;
    }
}

static int Dispatch(uint8_t* L) {
    Stack s{};
    if (!Valid(L, s)) {
        JVR_LOG_ONCE("bridge: unexpected Lua stack layout; the Lua side gets no VR data");
        return 0;
    }
    JVR_LOG_ONCE("bridge: first call from Lua (op %d); stack layout OK", s.Int(1));
    switch ((Op)s.Int(1)) {
    case OpStatus:
        s.Push((float)kProtocol);
        s.Push(xr::Running() ? 1.f : 0.f);
        s.Push(config::Get().enabled ? 1.f : 0.f);
        s.Push((float)(int)config::Get().stereo);
        break;
    case OpTick: {
        rig::TickInput in;
        in.kind = (rig::AnchorKind)s.Int(2);
        in.dt = s.Num(3);
        in.anchorPos = s.V3(4);
        in.anchorBasis.r = Normalize(s.V3(7));
        in.anchorBasis.u = Normalize(s.V3(10));
        in.anchorBasis.f = Normalize(s.V3(13));
        rig::TickOutput out = rig::Tick(in);
        if (out.overrideCamera) viewcb::SetCameraTarget(out.camera.pos, out.eyeOffset[0], out.eyeOffset[1], out.eye);
        s.Push(out.overrideCamera ? 1.f : 0.f);
        s.Push(out.camera.pos);
        s.Push(out.camera.basis.f);
        s.Push(out.camera.basis.u);
        s.Push(out.fovY);
        break;
    }
    case OpHand: {
        Vec3 o;
        Basis b;
        bool ok = rig::HandRay(s.Int(2), o, b);
        s.Push(ok ? 1.f : 0.f);
        s.Push(o);
        s.Push(b.f);
        s.Push(b.u);
        break;
    }
    case OpControls: {
        xr::Controls c = xr::GetControls();
        int bits = (c.primary[0] ? 1 : 0) | (c.secondary[0] ? 2 : 0) | (c.primary[1] ? 4 : 0) | (c.secondary[1] ? 8 : 0) |
                   (c.stickClick[0] ? 16 : 0) | (c.stickClick[1] ? 32 : 0) | (c.menu ? 64 : 0) | (c.active[0] ? 128 : 0) |
                   (c.active[1] ? 256 : 0);
        s.Push(c.trigger[0]);
        s.Push(c.trigger[1]);
        s.Push(c.grip[0]);
        s.Push(c.grip[1]);
        s.Push(c.stickX[0]);
        s.Push(c.stickY[0]);
        s.Push(c.stickX[1]);
        s.Push(c.stickY[1]);
        s.Push((float)bits);
        // Wall-clock seconds since the DLL started: the game's time step is clamped to 1/15 s on slow frames, so
        // hold timers measured with it ran slow (run 106: a 3 s hold counted as 0.6 s at 3 fps).
        static const ULONGLONG start = GetTickCount64();
        s.Push((float)((GetTickCount64() - start) / 1000.0));
        break;
    }
    case OpFlags: {
        input::LuaFlags f;
        f.pad = (input::PadMode)s.Int(2);
        f.turn = s.Int(3) != 0;
        f.laser = s.Int(4) != 0;
        f.grab = s.Int(5) != 0;
        f.fire = s.nargs >= 7 && s.Int(7) != 0;
        input::SetFlags(f);
        render::SetTabletVisible(s.Int(6) != 0);
        break;
    }
    case OpPad:
        input::SetPadOverride((uint32_t)s.Int(2), s.Num(3), s.Num(4), s.Num(5), s.Num(6), s.Num(7), s.Num(8), (uint32_t)s.Int(9));
        break;
    case OpHaptic:
        xr::Haptic(s.Int(2), s.Num(3), s.Num(4), s.Num(5));
        break;
    case OpMouse:
        input::MouseButton(s.Int(2), s.Int(3) != 0);
        break;
    case OpKey:
        input::KeyTap((WORD)s.Int(2));
        break;
    case OpConfig: {
        static const char* keys[] = {"", "Rig.OverviewScale", "Rig.HorizonLock", "Input.GunnerAimGain", "Input.SteeringWheel",
                                     "UI.HideHudImmersive", "Input.LeftHanded", "Rig.SnapTurnDeg", "Rig.Seated"};
        int i = s.Int(2);
        s.Push(i > 0 && i < (int)(sizeof(keys) / sizeof(keys[0])) ? config::Number(keys[i], 0.f) : 0.f);
        break;
    }
    case OpRecenter:
        rig::Recenter();
        xr::PlaceScreen();
        break;
    case OpWheel: {
        Pose l, r;
        bool ok = rig::HandLocal(0, l) && rig::HandLocal(1, r);
        s.Push(ok ? 1.f : 0.f);
        s.Push(ok ? WheelAngle(l.p, r.p) : 0.f);
        break;
    }
    case OpHandLocal: {
        Pose p;
        bool ok = rig::HandLocal(s.Int(2), p);
        s.Push(ok ? 1.f : 0.f);
        s.Push(p.p);
        break;
    }
    case OpLog:
        if (const char* name = MilestoneName(s.Int(2)))
            LOGI("lua: %s  a=%.3f b=%.3f c=%.3f", name, s.Num(3), s.Num(4), s.Num(5));
        else
            LOGI("lua: %d %.3f %.3f %.3f", s.Int(2), s.Num(3), s.Num(4), s.Num(5));
        break;
    case OpLogText: {
        // Lua sends long text in chunks of up to 30 characters; the state pointer tells Lua VMs apart.
        static thread_local std::string text;
        for (int i = 3; i <= s.nargs; ++i) {
            int c = s.Int(i);
            text.push_back(c >= 32 && c < 127 ? (char)c : '?');
        }
        if (s.Int(2) != 0) {
            LOGI("lua[%p]: %s", (void*)L, text.c_str());
            text.clear();
        }
        break;
    }
    case OpPlaceScreen:
        xr::PlaceScreen();
        break;
    case OpHudStatus:
        s.Push(passes::WorldHudActive() ? 1.f : 0.f);
        break;
    case OpDebugCommand:
        s.Push((float)render::TakeDebugCommand());
        break;
    case OpPointer: {
        Vec3 stage;
        bool visible = s.Int(2) != 0 && rig::GameToStage(s.V3(3), stage);
        render::SetPointer(visible, stage);
        break;
    }
    default:
        JVR_LOG_ONCE("bridge: unknown op %d", s.Int(1));
        break;
    }
    return s.pushed;
}

static int SafeDispatch(uint8_t* L) {
    __try {
        return Dispatch(L);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace jvr::bridge

extern "C" __declspec(dllexport) int jwe3vr_lua(void* L) {
    int n = jvr::bridge::SafeDispatch((uint8_t*)L);
    if (n < 0) {
        JVR_LOG_ONCE("bridge: exception inside a Lua call; returning nothing");
        return 0;
    }
    return n;
}
