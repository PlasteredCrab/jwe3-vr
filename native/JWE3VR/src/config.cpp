#include "config.h"
#include "common.h"
#include "log.h"

#include <cstdio>

namespace jvr::config {

// Readers on other threads hold `const Settings&` across a reload, so a reload publishes a new object
// and the old one is never freed (a few hundred bytes per saved edit).
static std::atomic<const Settings*> g_settings{new Settings()};
static FILETIME g_lastWrite{};
static std::mutex g_lock;

static const char* kDefaultIni = R"(; JWE3VR settings. Saved changes apply live (within a second).

[General]
Enabled = 1

[Stereo]
; Mode: mono | aer | sync
;   mono = both eyes get the same image every frame (default: smooth, no stereo depth; the first headset test
;          found the alternating eyes jittery at the game's ~40 fps, and this the one that looked right)
;   aer  = alternate eye per frame (stereo depth, but each eye only gets every other frame)
;   sync = both eyes every frame by replaying the frame (experimental; needs the view CB to be found)
Mode = mono
; Frames between the Lua camera update and that frame being presented. If the world looks wrong
; in stereo (eyes swapped or one frame apart), try 0, 1 or 2, or flip SwapEyes.
AERPresentLag = 1
SwapEyes = 0
; Extra degrees added to the game's culling FOV so fast head turns never reveal culled edges.
CullMarginDeg = 3
; Sharpening of the eye images, 0 (off) to 1 (strongest); contrast-adaptive, so flat areas stay clean.
Sharpen = 0.5
; Tallest view the game renders, in degrees (0: the whole lens). The game's one frame is spread over this, so a
; smaller view is sharper and looks bigger to the game's level-of-detail choice; past it the lens edge is black.
; 90 keeps the full width of a Quest 2 (the 16:10 frame is then ~116 degrees wide) and loses a few degrees at the
; top and bottom.
MaxFovDeg = 90
; Park overview: while you move the park (grab, fly, stick), both eyes get the same image, so the alternating eyes
; never disagree about where the park is; stereo comes back when you stop. 0 keeps stereo all the time.
MonoWhileMoving = 1
; Search for the main view constant buffer and log the result (needed before sync mode can work).
SyncDiscovery = 1
; Sync: state of the backbuffer right after the replayed submission: rt (render target) or present.
SyncCaptureState = rt
; The headset paces the game; turning the monitor's v-sync off avoids halving the frame rate.
ForceVsyncOff = 1

[Rig]
; Park overview scale: world metres per real metre (30 = the park as a 1:30 model).
OverviewScale = 30
IPDScale = 1.0
HorizonLock = 1
Seated = 0
SeatedEyeHeight = 1.20
StandingEyeHeight = 1.67
; Snap turn in degrees, 0 for smooth turning.
SnapTurnDeg = 30
SmoothTurnDegPerSec = 90

[Input]
LeftHanded = 0
VirtualPad = 1
GunnerAimGain = 6.0
SteeringWheel = 1
LaserSmoothing = 12

[UI]
HideHudImmersive = 1
ScreenDistance = 2.0
ScreenWidth = 2.4
; In-world HUD: the game's UI is drawn into its own texture and shown as a floating panel instead of being
; painted onto both eyes. The log says "in-world HUD active" when it works, otherwise the tablet is used.
WorldHud = 1
; How UI drawing is found: "texture" (default) finds the texture JWE3 draws its UI into and moves those draws to
; the panel; "backbuffer" treats render-target bindings of the backbuffer as UI, after the first
; UISkipBackbufferBinds of each frame; "markers" uses the pass names in UIMarkers.
UIDetect = texture
UISkipBackbufferBinds = 0
UIMarkers = Gameface,Renoir,Scaleform,UIRender,HUD
HudDistance = 1.4
HudWidth = 1.6
HudDropDeg = 12
HudFollowDeg = 40

[Performance]
; Render passes (marker substrings) that run once per frame and are skipped for the second eye in sync mode.
SharedPassMarkers = DirectionalShadow,ReflectiveShadowMap,LPV,EnvironmentCapture,MegaTextureBake,SkinningProcessor,HeightVariance
; Log every render pass marker name once (use this to tune SharedPassMarkers, UIMarkers, TAAMarkers).
MarkerDump = 0
; Fixed foveated rendering (D3D12 variable rate shading tier 2): 0 off, 1 HDR scene passes, 2 all frame-sized colour
; passes (not the UI, ID or motion-vector targets). JWE3 shades most of the frame in compute, which this cannot reach.
; Angles from the eye centre: full rate inside Inner, half rate to Mid, quarter rate to Outer, 4x4 beyond.
Foveation = 1
FoveaInnerDeg = 20
FoveaMidDeg = 35
FoveaOuterDeg = 50
; Alternate-eye mode: keep a separate TAA history per eye so neither eye blends in the other's image.
; (Records the game's constant-buffer binds to fix up the previous-camera row; costs a little CPU.)
PerEyeTAA = 1
TAAMarkers = TemporalAntiAliasing,TemporalUpscale

[Debug]
LogPoses = 0
)";

static std::wstring IniPath() { return ModuleDir() + L"JWE3VR.ini"; }

static std::string Str(const wchar_t* sec, const wchar_t* key, const char* def) {
    wchar_t buf[1024];
    std::wstring wdef(def, def + strlen(def));
    GetPrivateProfileStringW(sec, key, wdef.c_str(), buf, 1024, IniPath().c_str());
    char out[1024];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    std::string s = out;
    // GetPrivateProfileString keeps trailing comments out but not trailing spaces.
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

static float Num(const wchar_t* sec, const wchar_t* key, float def) {
    char d[32];
    snprintf(d, sizeof(d), "%g", def);
    std::string s = Str(sec, key, d);
    char* end = nullptr;
    float v = strtof(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

static bool Bool(const wchar_t* sec, const wchar_t* key, bool def) { return Num(sec, key, def ? 1.f : 0.f) != 0.f; }

static bool FileTime(FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &d)) return false;
    out = d.ftLastWriteTime;
    return true;
}

void Load() {
    std::lock_guard<std::mutex> l(g_lock);
    if (GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, IniPath().c_str(), L"w") == 0 && f) {
            fputs(kDefaultIni, f);
            fclose(f);
            LOGI("wrote default settings to JWE3VR.ini");
        }
    }
    Settings s;
    s.enabled = Bool(L"General", L"Enabled", s.enabled);

    std::string mode = Str(L"Stereo", L"Mode", "mono");
    s.stereo = mode == "mono" ? StereoMode::Mono : mode == "sync" ? StereoMode::Sync : StereoMode::AER;
    s.aerPresentLag = (int)Num(L"Stereo", L"AERPresentLag", (float)s.aerPresentLag);
    s.swapEyes = Bool(L"Stereo", L"SwapEyes", s.swapEyes);
    s.cullMarginDeg = Num(L"Stereo", L"CullMarginDeg", s.cullMarginDeg);
    s.sharpen = Num(L"Stereo", L"Sharpen", s.sharpen);
    s.maxFovDeg = Num(L"Stereo", L"MaxFovDeg", s.maxFovDeg);
    s.monoWhileMoving = Num(L"Stereo", L"MonoWhileMoving", s.monoWhileMoving ? 1.f : 0.f) != 0.f;
    s.syncDiscovery = Bool(L"Stereo", L"SyncDiscovery", s.syncDiscovery);
    s.syncCaptureRT = Str(L"Stereo", L"SyncCaptureState", "rt") != "present";
    s.forceVsyncOff = Bool(L"Stereo", L"ForceVsyncOff", s.forceVsyncOff);

    s.overviewScale = Num(L"Rig", L"OverviewScale", s.overviewScale);
    s.ipdScale = Num(L"Rig", L"IPDScale", s.ipdScale);
    s.horizonLock = Bool(L"Rig", L"HorizonLock", s.horizonLock);
    s.seated = Bool(L"Rig", L"Seated", s.seated);
    s.seatedEyeHeight = Num(L"Rig", L"SeatedEyeHeight", s.seatedEyeHeight);
    s.standingEyeHeight = Num(L"Rig", L"StandingEyeHeight", s.standingEyeHeight);
    s.snapTurnDeg = (int)Num(L"Rig", L"SnapTurnDeg", (float)s.snapTurnDeg);
    s.smoothTurnDegPerSec = Num(L"Rig", L"SmoothTurnDegPerSec", s.smoothTurnDegPerSec);

    s.leftHanded = Bool(L"Input", L"LeftHanded", s.leftHanded);
    s.virtualPad = Bool(L"Input", L"VirtualPad", s.virtualPad);
    s.gunnerAimGain = Num(L"Input", L"GunnerAimGain", s.gunnerAimGain);
    s.steeringWheel = Bool(L"Input", L"SteeringWheel", s.steeringWheel);
    s.laserSmoothing = Num(L"Input", L"LaserSmoothing", s.laserSmoothing);

    s.hideHudImmersive = Bool(L"UI", L"HideHudImmersive", s.hideHudImmersive);
    s.screenDistance = Num(L"UI", L"ScreenDistance", s.screenDistance);
    s.screenWidth = Num(L"UI", L"ScreenWidth", s.screenWidth);

    s.worldHud = Bool(L"UI", L"WorldHud", s.worldHud);
    s.uiMarkers = Str(L"UI", L"UIMarkers", s.uiMarkers.c_str());
    std::string detect = Str(L"UI", L"UIDetect", "texture");
    s.uiDetect = detect == "markers" ? UiDetect::Markers : detect == "backbuffer" ? UiDetect::Backbuffer : UiDetect::Texture;
    s.uiSkipBackbufferBinds = (int)Num(L"UI", L"UISkipBackbufferBinds", (float)s.uiSkipBackbufferBinds);
    s.hudDistance = Num(L"UI", L"HudDistance", s.hudDistance);
    s.hudWidth = Num(L"UI", L"HudWidth", s.hudWidth);
    s.hudDropDeg = Num(L"UI", L"HudDropDeg", s.hudDropDeg);
    s.hudFollowDeg = Num(L"UI", L"HudFollowDeg", s.hudFollowDeg);

    s.sharedPassMarkers = Str(L"Performance", L"SharedPassMarkers", s.sharedPassMarkers.c_str());
    s.markerDump = Bool(L"Performance", L"MarkerDump", s.markerDump);
    s.foveation = (int)Num(L"Performance", L"Foveation", (float)s.foveation);
    s.foveaInnerDeg = Num(L"Performance", L"FoveaInnerDeg", s.foveaInnerDeg);
    s.foveaMidDeg = Num(L"Performance", L"FoveaMidDeg", s.foveaMidDeg);
    s.foveaOuterDeg = Num(L"Performance", L"FoveaOuterDeg", s.foveaOuterDeg);
    s.perEyeTaa = Bool(L"Performance", L"PerEyeTAA", s.perEyeTaa);
    s.taaMarkers = Str(L"Performance", L"TAAMarkers", s.taaMarkers.c_str());

    s.logPoses = Bool(L"Debug", L"LogPoses", s.logPoses);

    // Clamp what would break the rig.
    if (s.overviewScale < 1.f) s.overviewScale = 1.f;
    if (s.ipdScale < 0.1f) s.ipdScale = 0.1f;
    if (s.aerPresentLag < 0) s.aerPresentLag = 0;
    if (s.aerPresentLag > 3) s.aerPresentLag = 3;

    g_settings.store(new Settings(s));
    FileTime(g_lastWrite);
    LOGI("settings loaded: stereo=%s lag=%d overviewScale=%.1f pad=%d", mode.c_str(), s.aerPresentLag, s.overviewScale,
         (int)s.virtualPad);
}

const Settings& Get() { return *g_settings.load(); }

bool ReloadIfChanged() {
    FILETIME t;
    if (!FileTime(t)) return false;
    if (CompareFileTime(&t, &g_lastWrite) == 0) return false;
    Load();
    return true;
}

float Number(const char* dottedKey, float fallback) {
    const Settings& s = Get();
    struct Entry { const char* key; float value; };
    const Entry table[] = {
        {"Rig.OverviewScale", s.overviewScale},
        {"Rig.IPDScale", s.ipdScale},
        {"Rig.HorizonLock", s.horizonLock ? 1.f : 0.f},
        {"Rig.Seated", s.seated ? 1.f : 0.f},
        {"Rig.SnapTurnDeg", (float)s.snapTurnDeg},
        {"Rig.SmoothTurnDegPerSec", s.smoothTurnDegPerSec},
        {"Input.LeftHanded", s.leftHanded ? 1.f : 0.f},
        {"Input.GunnerAimGain", s.gunnerAimGain},
        {"Input.SteeringWheel", s.steeringWheel ? 1.f : 0.f},
        {"Input.LaserSmoothing", s.laserSmoothing},
        {"UI.HideHudImmersive", s.hideHudImmersive ? 1.f : 0.f},
    };
    for (const Entry& e : table)
        if (strcmp(e.key, dottedKey) == 0) return e.value;
    return fallback;
}

} // namespace jvr::config
