// JWE3VR.ini: created with defaults and comments on first run, reloaded live when saved.
// Lua reads the same values through the bridge (Bridge op Config), so one file drives both sides.
#pragma once

#include <string>

namespace jvr {

enum class StereoMode { Mono = 0, AER = 1, Sync = 2 };

enum class UiDetect { Texture, Backbuffer, Markers };

struct Settings {
    // [General]
    bool enabled = true;

    // [Stereo]
    StereoMode stereo = StereoMode::AER;
    int aerPresentLag = 1;        // ticks between Lua setting an eye camera and that frame reaching Present
    bool swapEyes = false;        // flip if AER pairing is one frame off (the world looks inside-out)
    float cullMarginDeg = 3.0f;   // extra culling FOV so late head motion never shows popped-out edges
    bool syncDiscovery = true;    // look for the view constant buffer and log what was found
    bool syncCaptureRT = true;    // sync: backbuffer state after the replayed submission (true RENDER_TARGET, false PRESENT)
    bool forceVsyncOff = true;    // present with sync interval 0 while the headset paces the game

    // [Rig]
    float overviewScale = 30.0f;  // world metres per real metre in the park overview (1:30 model)
    float ipdScale = 1.0f;        // 1 = your real IPD; raise for a more miniature look
    bool horizonLock = true;      // vehicles: keep the horizon level instead of rolling with the cabin
    bool seated = false;
    float seatedEyeHeight = 1.20f; // real-world eye height while seated (m)
    float standingEyeHeight = 1.67f; // game guest eye height; your head height is added relative to this
    int snapTurnDeg = 30;         // 0 = smooth turn
    float smoothTurnDegPerSec = 90.0f;

    // [Input]
    bool leftHanded = false;
    bool virtualPad = true;       // VR controllers appear to the game as an Xbox pad (XInput slot 0)
    float gunnerAimGain = 6.0f;   // stick deflection per radian of aim error when steering the gun to your hand
    bool steeringWheel = true;    // grab the wheel with both grips to steer
    float laserSmoothing = 12.0f; // cursor follow rate (1/s); higher = snappier, lower = steadier

    // [UI]
    bool hideHudImmersive = true; // hide the 2D HUD while you are in the 3D world (it is shown on the tablet)
    float screenDistance = 2.0f;  // flat-screen quad for menus, loading and the tablet fallback (m)
    float screenWidth = 2.4f;

    bool worldHud = true;         // draw the game's UI into its own texture and show it as a panel in the world
    // JWE3 draws its UI with Coherent Gameface ("UI (Gameface)", Renoir). "GUI" is left out: it would
    // also match WorldGUIPass (in-world icons). Unverified until a MarkerDump run.
    std::string uiMarkers = "Gameface,Renoir,Scaleform,UIRender,HUD";
    // How UI drawing is recognised. Retail JWE3 emits no pass markers and draws its UI into an 8-bit texture of
    // its own, composited into the final image by a compute pass: by default ("texture") that texture is found
    // and its draws redirected. "backbuffer": every render-target binding of the backbuffer (or of the image the
    // game copies into it) after the first uiSkipBackbufferBinds of each frame; "markers": the uiMarkers passes.
    UiDetect uiDetect = UiDetect::Texture;
    int uiSkipBackbufferBinds = 0;
    float hudDistance = 1.4f;     // in-world HUD panel distance (m)
    float hudWidth = 1.6f;
    float hudDropDeg = 12.0f;     // panel sits this far below eye level
    float hudFollowDeg = 40.0f;   // panel re-centres when you look further away than this

    // [Performance]
    std::string sharedPassMarkers = "DirectionalShadow,ReflectiveShadowMap,LPV,EnvironmentCapture,MegaTextureBake,SkinningProcessor,HeightVariance";
    bool markerDump = false;      // log every render pass marker once (to choose sharedPassMarkers)
    int foveation = 1;            // 0 off, 1 HDR scene targets only, 2 every frame-sized target
    float foveaInnerDeg = 20.f;   // full rate inside this angle from the eye centre
    float foveaMidDeg = 35.f;     // half rate (2x1) out to here
    float foveaOuterDeg = 50.f;   // quarter rate (2x2) out to here; 4x4 beyond (2x2 if the GPU has no 4x4)
    bool perEyeTaa = true;        // AER: keep one TAA history per eye
    std::string taaMarkers = "TemporalAntiAliasing,TemporalUpscale";

    // [Debug]
    bool logPoses = false;
};

namespace config {

// Loads (creating the file with defaults if needed). Returns the current settings.
const Settings& Get();
void Load();
// Re-reads the file if it was modified since the last load. Cheap; call about once a second.
bool ReloadIfChanged();
// Generic numeric lookup by "Section.Key" for the Lua side. Returns fallback if unknown.
float Number(const char* dottedKey, float fallback);

} // namespace config
} // namespace jvr
