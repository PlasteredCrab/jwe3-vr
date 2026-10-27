#include "xr.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "rig.h"

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <deque>
#include <map>
#include <mutex>

namespace jvr::xr {

using namespace vm;

struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D12Resource*> images;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool acquired = false;
};

struct HapticRequest {
    int hand;
    float amplitude, seconds, frequency;
};

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_stage = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static XrFrameState g_frame{XR_TYPE_FRAME_STATE};
static bool g_frameBegun = false;
static bool g_needsReinit = false;
static UINT g_failedW = 0, g_failedH = 0; // swapchain size that failed last (no retry spam)
static UINT g_hudFailedW = 0, g_hudFailedH = 0;
static DXGI_FORMAT g_hudFailedFormat = DXGI_FORMAT_UNKNOWN;
static Swap g_swaps[5]; // Left, Right, Screen, Hud (the UI texture's size and format), Dot (kDotSize)
static XrPosef g_hudPose{{0, 0, 0, 1}, {0, 1.4f, -1.4f}};
static bool g_hudPlaced = false;
static Pose g_rawHead;
static bool g_rawHeadValid = false;
static void UpdateHudPose(const Pose& head);
static DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
static XrReferenceSpaceType g_spaceType = XR_REFERENCE_SPACE_TYPE_STAGE;

// Actions
static XrActionSet g_set = XR_NULL_HANDLE;
static XrAction g_gripPose, g_aimPose, g_trigger, g_squeeze, g_stick, g_stickClick, g_primary, g_secondary, g_menu,
    g_haptic;
static XrPath g_hand[2];
static XrSpace g_gripSpace[2]{}, g_aimSpace[2]{};

static std::mutex g_ctrlLock;
static Controls g_controls;
static rig::TrackedPose g_rawAim[2];
static std::deque<HapticRequest> g_haptics;

// Screen quad
static XrPosef g_screenPose{{0, 0, 0, 1}, {0, 1.5f, -2.f}};
static bool g_placeScreen = true;
static float g_screenAspect = 16.f / 9.f;

// Failures are logged the first 5 times per call site, then every 1000th with a count: a call that fails every
// frame (xrEndFrame refusing a layer, say) must not write 90 lines a second.
static bool Ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    static std::mutex lock;
    static std::map<const char*, unsigned> failures; // keyed by the call's name literal
    unsigned n;
    {
        std::lock_guard<std::mutex> l(lock);
        n = ++failures[what];
    }
    if (n > 5 && n % 1000 != 0) return false;
    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_instance) xrResultToString(g_instance, r, buf);
    LOGE("OpenXR %s failed: %s (%d)%s", what, buf, (int)r, n == 5 ? "; further failures of this call logged every 1000th" : "");
    if (n > 5) LOGE("OpenXR %s has failed %u times", what, n);
    return false;
}

static Pose ToPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}
static XrPosef ToXr(const Pose& p) {
    XrPosef o;
    o.orientation = {p.q.x, p.q.y, p.q.z, p.q.w};
    o.position = {p.p.x, p.p.y, p.p.z};
    return o;
}

static XrPath Path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, s, &p);
    return p;
}

static XrAction MakeAction(const char* name, const char* loc, XrActionType type, bool perHand) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, loc);
    ci.actionType = type;
    if (perHand) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = g_hand;
    }
    XrAction a = XR_NULL_HANDLE;
    Ok(xrCreateAction(g_set, &ci, &a), name);
    return a;
}

static void Suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& b) {
    std::vector<XrActionSuggestedBinding> v;
    for (auto& e : b) v.push_back({e.first, Path(e.second)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = Path(profile);
    s.countSuggestedBindings = (uint32_t)v.size();
    s.suggestedBindings = v.data();
    XrResult r = xrSuggestInteractionProfileBindings(g_instance, &s);
    if (XR_FAILED(r)) LOGW("bindings for %s rejected (%d)", profile, (int)r);
}

static bool CreateActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "jwe3vr");
    strcpy_s(si.localizedActionSetName, "JWE3 VR");
    if (!Ok(xrCreateActionSet(g_instance, &si, &g_set), "xrCreateActionSet")) return false;
    g_hand[0] = Path("/user/hand/left");
    g_hand[1] = Path("/user/hand/right");

    g_gripPose = MakeAction("grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, true);
    g_aimPose = MakeAction("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, true);
    g_trigger = MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_squeeze = MakeAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_stick = MakeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    g_stickClick = MakeAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_primary = MakeAction("primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_secondary = MakeAction("secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    g_menu = MakeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    g_haptic = MakeAction("haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

    const char* L = "/user/hand/left";
    const char* R = "/user/hand/right";
    auto p = [](const char* hand, const char* sub) {
        static std::deque<std::string> keep;
        keep.push_back(std::string(hand) + sub);
        return keep.back().c_str();
    };
    std::vector<std::pair<XrAction, const char*>> common = {
        {g_gripPose, p(L, "/input/grip/pose")},   {g_gripPose, p(R, "/input/grip/pose")},
        {g_aimPose, p(L, "/input/aim/pose")},     {g_aimPose, p(R, "/input/aim/pose")},
        {g_haptic, p(L, "/output/haptic")},       {g_haptic, p(R, "/output/haptic")},
    };
    auto touch = common;
    touch.insert(touch.end(), {
        {g_trigger, p(L, "/input/trigger/value")},   {g_trigger, p(R, "/input/trigger/value")},
        {g_squeeze, p(L, "/input/squeeze/value")},   {g_squeeze, p(R, "/input/squeeze/value")},
        {g_stick, p(L, "/input/thumbstick")},        {g_stick, p(R, "/input/thumbstick")},
        {g_stickClick, p(L, "/input/thumbstick/click")}, {g_stickClick, p(R, "/input/thumbstick/click")},
        {g_primary, p(L, "/input/x/click")},         {g_primary, p(R, "/input/a/click")},
        {g_secondary, p(L, "/input/y/click")},       {g_secondary, p(R, "/input/b/click")},
        {g_menu, p(L, "/input/menu/click")},
    });
    Suggest("/interaction_profiles/oculus/touch_controller", touch);

    auto index = common;
    index.insert(index.end(), {
        {g_trigger, p(L, "/input/trigger/value")},   {g_trigger, p(R, "/input/trigger/value")},
        {g_squeeze, p(L, "/input/squeeze/value")},   {g_squeeze, p(R, "/input/squeeze/value")},
        {g_stick, p(L, "/input/thumbstick")},        {g_stick, p(R, "/input/thumbstick")},
        {g_stickClick, p(L, "/input/thumbstick/click")}, {g_stickClick, p(R, "/input/thumbstick/click")},
        {g_primary, p(L, "/input/a/click")},         {g_primary, p(R, "/input/a/click")},
        {g_secondary, p(L, "/input/b/click")},       {g_secondary, p(R, "/input/b/click")},
        {g_menu, p(L, "/input/system/click")},
    });
    Suggest("/interaction_profiles/valve/index_controller", index);

    auto wmr = common;
    wmr.insert(wmr.end(), {
        {g_trigger, p(L, "/input/trigger/value")},   {g_trigger, p(R, "/input/trigger/value")},
        {g_squeeze, p(L, "/input/squeeze/click")},   {g_squeeze, p(R, "/input/squeeze/click")},
        {g_stick, p(L, "/input/thumbstick")},        {g_stick, p(R, "/input/thumbstick")},
        {g_stickClick, p(L, "/input/thumbstick/click")}, {g_stickClick, p(R, "/input/thumbstick/click")},
        {g_primary, p(L, "/input/trackpad/click")},  {g_primary, p(R, "/input/trackpad/click")},
        {g_menu, p(L, "/input/menu/click")},
    });
    Suggest("/interaction_profiles/microsoft/motion_controller", wmr);

    auto simple = common;
    simple.insert(simple.end(), {
        {g_trigger, p(L, "/input/select/click")}, {g_trigger, p(R, "/input/select/click")},
        {g_menu, p(L, "/input/menu/click")},
    });
    Suggest("/interaction_profiles/khr/simple_controller", simple);

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    if (!Ok(xrAttachSessionActionSets(g_session, &ai), "xrAttachSessionActionSets")) return false;

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.poseInActionSpace.orientation.w = 1.f;
        sc.subactionPath = g_hand[h];
        sc.action = g_gripPose;
        Ok(xrCreateActionSpace(g_session, &sc, &g_gripSpace[h]), "grip space");
        sc.action = g_aimPose;
        Ok(xrCreateActionSpace(g_session, &sc, &g_aimSpace[h]), "aim space");
    }
    return true;
}

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g_session) return true;
    const char* exts[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "JWE3VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "Cobra");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    if (!Ok(xrCreateInstance(&ci, &g_instance), "xrCreateInstance")) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Ok(xrGetSystem(g_instance, &sg, &g_system), "xrGetSystem")) return false;

    PFN_xrGetD3D12GraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (!getReq || !Ok(getReq(g_instance, g_system, &req), "xrGetD3D12GraphicsRequirementsKHR")) return false;
    LUID luid = device->GetAdapterLuid();
    if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
        LOGW("the headset runtime wants a different GPU than the game uses; expect failure");

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    if (!Ok(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return false;

    uint32_t n = 0;
    xrEnumerateReferenceSpaces(g_session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces(g_session, n, &n, spaces.data());
    bool hasStage = false;
    for (auto s : spaces) hasStage |= s == XR_REFERENCE_SPACE_TYPE_STAGE;
    g_spaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.f;
    rs.referenceSpaceType = g_spaceType;
    if (!Ok(xrCreateReferenceSpace(g_session, &rs, &g_stage), "stage space")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Ok(xrCreateReferenceSpace(g_session, &rs, &g_view), "view space")) return false;
    rig::SetHasFloor(hasStage);
    LOGI("OpenXR session created (%s space)", hasStage ? "stage" : "local, no floor height");

    return CreateActions();
}

void Shutdown() {
    render::WaitCopies();
    for (Swap& s : g_swaps) {
        if (s.handle) xrDestroySwapchain(s.handle);
        s = Swap{};
    }
    if (g_session) xrDestroySession(g_session); // also destroys its spaces
    if (g_instance) xrDestroyInstance(g_instance); // and the action set
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
    g_stage = g_view = XR_NULL_HANDLE;
    g_set = XR_NULL_HANDLE;
    for (int h = 0; h < 2; ++h) g_gripSpace[h] = g_aimSpace[h] = XR_NULL_HANDLE;
    g_running = false;
    g_frameBegun = false;
    g_needsReinit = false;
    g_failedW = g_failedH = 0;
}

bool Running() { return g_running; }
int SessionState() { return (int)g_state; }
bool NeedsReinit() { return g_needsReinit; }

static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            g_state = e->state;
            LOGI("OpenXR session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = Ok(xrBeginSession(g_session, &bi), "xrBeginSession");
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_running = false;
                g_frameBegun = false;
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                // Runtime closed or the headset went away: tear down and let the Present path retry later.
                g_running = false;
                g_frameBegun = false;
                g_needsReinit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            rig::Recenter();
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void SyncControls(XrTime t, rig::XrSnapshot& snap) {
    XrActiveActionSet as{g_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(g_session, &si))) return;

    Controls c;
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = g_hand[h];
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = g_trigger;
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f))) c.trigger[h] = f.currentState;
        gi.action = g_squeeze;
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &f))) c.grip[h] = f.currentState;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = g_stick;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &v))) {
            c.stickX[h] = v.currentState.x;
            c.stickY[h] = v.currentState.y;
        }
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = g_stickClick;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b))) c.stickClick[h] = b.currentState;
        gi.action = g_primary;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b))) c.primary[h] = b.currentState;
        gi.action = g_secondary;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b))) c.secondary[h] = b.currentState;

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(g_gripSpace[h], g_stage, t, &loc)) && (loc.locationFlags & need) == need) {
            snap.grip[h].pose = ToPose(loc.pose);
            snap.grip[h].valid = true;
            c.active[h] = true;
        }
        loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], g_stage, t, &loc)) && (loc.locationFlags & need) == need) {
            snap.aim[h].pose = ToPose(loc.pose);
            snap.aim[h].valid = true;
        }
        std::lock_guard<std::mutex> l(g_ctrlLock);
        g_rawAim[h] = snap.aim[h];
    }
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_menu;
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &b))) c.menu = b.currentState;

    std::deque<HapticRequest> pending;
    {
        std::lock_guard<std::mutex> l(g_ctrlLock);
        g_controls = c;
        pending.swap(g_haptics);
    }
    for (const HapticRequest& r : pending) {
        XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
        vib.amplitude = r.amplitude;
        vib.duration = (XrDuration)(r.seconds * 1e9);
        vib.frequency = r.frequency > 0 ? r.frequency : XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = g_haptic;
        hi.subactionPath = g_hand[r.hand & 1];
        xrApplyHapticFeedback(g_session, &hi, (XrHapticBaseHeader*)&vib);
    }
}

bool BeginFrame() {
    if (!g_session) return false;
    PollEvents();
    if (!g_running || g_frameBegun) return false;

    g_frame = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!Ok(xrWaitFrame(g_session, &wi, &g_frame), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Ok(xrBeginFrame(g_session, &bi), "xrBeginFrame")) return false;
    g_frameBegun = true;

    rig::XrSnapshot snap;
    snap.displayTime = g_frame.predictedDisplayTime;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_frame.predictedDisplayTime;
    li.space = g_stage;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &count, views)) && count == 2 && (vs.viewStateFlags & need) == need) {
        for (int i = 0; i < 2; ++i) {
            snap.eye[i].pose = ToPose(views[i].pose);
            snap.eye[i].valid = true;
            snap.rawEye[i] = snap.eye[i].pose;
            snap.fov[i] = {views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown};
        }
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(g_view, g_stage, g_frame.predictedDisplayTime, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        snap.head.pose = ToPose(loc.pose);
        snap.head.valid = true;
        snap.rawHead = snap.head.pose;
    }
    SyncControls(g_frame.predictedDisplayTime, snap);
    snap.valid = snap.head.valid && snap.eye[0].valid;
    rig::PublishXr(snap);

    if (g_placeScreen && snap.head.valid) {
        Vec3 f = Rotate(snap.rawHead.q, {0, 0, -1});
        f.y = 0;
        f = Normalize(f);
        float d = config::Get().screenDistance;
        Vec3 c = snap.rawHead.p + f * d;
        float yaw = std::atan2(-f.x, -f.z);
        g_screenPose = ToXr({AxisAngle({0, 1, 0}, yaw), c});
        g_placeScreen = false;
        g_hudPlaced = false; // recentre the HUD too
    }
    g_rawHeadValid = snap.head.valid;
    if (snap.head.valid) {
        g_rawHead = snap.rawHead;
        UpdateHudPose(snap.rawHead);
    }
    return true;
}

// The HUD panel sits below eye level in front of you and only re-centres when you look well away from
// it or walk away from it, so it reads like a dashboard instead of being glued to your face.
static void UpdateHudPose(const Pose& head) {
    const Settings& c = config::Get();
    Vec3 f = Rotate(head.q, {0, 0, -1});
    f.y = 0;
    if (Length(f) < 1e-3f) return;
    f = Normalize(f);
    Pose cur = ToPose(g_hudPose);
    Vec3 toHud = cur.p - head.p;
    Vec3 toHudFlat{toHud.x, 0, toHud.z};
    float angle = std::acos(std::clamp(Dot(Normalize(toHudFlat), f), -1.f, 1.f)) * 57.29578f;
    float dist = Length(toHudFlat);
    float drop = c.hudDropDeg * kPi / 180.f;
    float flatDist = c.hudDistance * std::cos(drop); // where the panel is placed horizontally
    bool move = !g_hudPlaced || angle > c.hudFollowDeg || std::fabs(dist - flatDist) > 0.35f;
    if (!move) return;
    Vec3 pos = head.p + f * flatDist - Vec3{0, 1, 0} * (c.hudDistance * std::sin(drop));
    float yaw = std::atan2(-f.x, -f.z);
    // Tilt the top away from you so the panel's face points back up at the eyes.
    Quat q = AxisAngle({0, 1, 0}, yaw) * AxisAngle({1, 0, 0}, -drop);
    g_hudPose = ToXr({q, pos});
    g_hudPlaced = true;
}

static DXGI_FORMAT SrgbOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

static bool CreateSwap(Swap& s, UINT w, UINT h, DXGI_FORMAT format) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Ok(xrCreateSwapchain(g_session, &ci, &s.handle), "xrCreateSwapchain")) return false;
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &ic, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> imgs(ic, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(s.handle, ic, &ic, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& img : imgs) s.images.push_back(img.texture);
    s.width = w;
    s.height = h;
    s.format = format;
    return true;
}

static std::vector<int64_t> RuntimeFormats() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    return fmts;
}

static void DestroySwapchains() {
    render::WaitCopies(); // copies into the old images may still be in flight
    for (Swap& s : g_swaps) {
        if (s.handle) xrDestroySwapchain(s.handle);
        s = Swap{};
    }
}

bool EnsureSwapchains(UINT width, UINT height, DXGI_FORMAT bb) {
    if (!g_session) return false;
    if (g_swaps[4].handle && g_swaps[0].width == width && g_swaps[0].height == height) return true;
    if (width == g_failedW && height == g_failedH) return false;
    DestroySwapchains();
    g_failedW = width;
    g_failedH = height;
    std::vector<int64_t> fmts = RuntimeFormats();
    DXGI_FORMAT want = SrgbOf(bb);
    g_format = DXGI_FORMAT_UNKNOWN;
    for (int64_t f : fmts)
        if ((DXGI_FORMAT)f == want || (DXGI_FORMAT)f == bb) {
            g_format = (DXGI_FORMAT)f;
            break;
        }
    if (g_format == DXGI_FORMAT_UNKNOWN) {
        LOGE("headset runtime has no swapchain format compatible with the game's backbuffer (%d); turn HDR off", (int)bb);
        return false;
    }
    for (int i = 0; i < 5; ++i) {
        if (i == (int)SwapId::Hud) continue; // EnsureHudSwapchain: sized and formatted like the game's UI texture
        UINT w = i == 4 ? kDotSize : width, h = i == 4 ? kDotSize : height;
        if (!CreateSwap(g_swaps[i], w, h, g_format)) {
            DestroySwapchains(); // all or none: EndFrame must never reference a missing one
            return false;
        }
    }
    g_screenAspect = (float)width / (float)height;
    rig::SetRenderAspect(g_screenAspect);
    g_failedW = g_failedH = 0;
    LOGI("swapchains %ux%u format %d (backbuffer %d)", width, height, (int)g_format, (int)bb);
    return true;
}

DXGI_FORMAT SwapFormat() { return g_format; }

// The typeless-group sibling a copy can still go to (sRGB <-> UNORM) when the runtime lacks the exact format.
static DXGI_FORMAT CopySibling(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

bool EnsureHudSwapchain(UINT width, UINT height, DXGI_FORMAT format) {
    Swap& s = g_swaps[(int)SwapId::Hud];
    if (!g_session || !g_swaps[0].handle) return false; // after the eye swapchains (they may be recreated)
    if (s.handle && s.width == width && s.height == height && (s.format == format || s.format == CopySibling(format))) return true;
    if (width == g_hudFailedW && height == g_hudFailedH && format == g_hudFailedFormat) return false;
    if (s.handle) {
        render::WaitCopies();
        xrDestroySwapchain(s.handle);
        s = Swap{};
    }
    std::vector<int64_t> fmts = RuntimeFormats();
    DXGI_FORMAT use = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT f : {format, CopySibling(format)})
        for (int64_t r : fmts)
            if (use == DXGI_FORMAT_UNKNOWN && (DXGI_FORMAT)r == f) use = f;
    if (use == DXGI_FORMAT_UNKNOWN || !CreateSwap(s, width, height, use)) {
        LOGE("in-world HUD: the headset runtime has no swapchain format the UI texture (%d) can be copied into", (int)format);
        s = Swap{};
        g_hudFailedW = width, g_hudFailedH = height, g_hudFailedFormat = format;
        return false;
    }
    LOGI("in-world HUD: panel swapchain %ux%u format %d (UI texture format %d)", width, height, (int)use, (int)format);
    return true;
}

ID3D12Resource* AcquireImage(SwapId id) {
    Swap& s = g_swaps[(int)id];
    if (!s.handle) return nullptr;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Ok(xrAcquireSwapchainImage(s.handle, &ai, &idx), "xrAcquireSwapchainImage")) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!Ok(xrWaitSwapchainImage(s.handle, &wi), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.handle, &ri); // never leave it acquired, or every later acquire fails
        return nullptr;
    }
    s.acquired = true;
    return s.images[idx];
}

void ReleaseImage(SwapId id) {
    Swap& s = g_swaps[(int)id];
    if (!s.acquired) return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.handle, &ri);
    s.acquired = false;
}

static XrRect2Di FullRect(const Swap& s) { return {{0, 0}, {(int32_t)s.width, (int32_t)s.height}}; }

void EndFrame(const EyeView eyes[2], const Layers& in) {
    if (!g_frameBegun) return;
    g_frameBegun = false;

    XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad dot{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;
    bool showProjection = in.projection, showScreen = in.screen;

    bool eyesOk = showProjection && eyes[0].valid && eyes[1].valid && g_swaps[0].handle && g_frame.shouldRender;
    if (eyesOk) {
        for (int i = 0; i < 2; ++i) {
            pv[i].pose = ToXr(eyes[i].stagePose);
            pv[i].fov = {eyes[i].fov.left, eyes[i].fov.right, eyes[i].fov.up, eyes[i].fov.down};
            pv[i].subImage.swapchain = g_swaps[i].handle;
            pv[i].subImage.imageRect = {{0, 0}, {(int32_t)g_swaps[i].width, (int32_t)g_swaps[i].height}};
        }
        proj.space = g_stage;
        proj.viewCount = 2;
        proj.views = pv;
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
    }
    if (showScreen && g_swaps[2].handle && g_frame.shouldRender) {
        float w = config::Get().screenWidth;
        quad.space = g_stage;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swaps[2].handle;
        quad.subImage.imageRect = {{0, 0}, {(int32_t)g_swaps[2].width, (int32_t)g_swaps[2].height}};
        quad.pose = g_screenPose;
        quad.size = {w, w / g_screenAspect};
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
    }
    if (in.hud && eyesOk && g_swaps[3].handle) {
        float w = config::Get().hudWidth;
        hud.space = g_stage;
        hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied UI
        hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hud.subImage.swapchain = g_swaps[3].handle;
        hud.subImage.imageRect = FullRect(g_swaps[3]);
        hud.pose = g_hudPose;
        hud.size = {w, w * g_swaps[3].height / (float)g_swaps[3].width};
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&hud;
    }
    if (in.dot && g_swaps[4].handle && g_frame.shouldRender && g_rawHeadValid) {
        // A small disc facing the head; its size grows with distance so it keeps the same angular size.
        Vec3 toHead = g_rawHead.p - in.dotPos;
        float dist = std::max(Length(toHead), 0.05f);
        Vec3 back = toHead * (1.f / dist);
        Vec3 right = Normalize(Cross({0, 1, 0}, back));
        if (Length(right) < 1e-3f) right = {1, 0, 0};
        Vec3 up = Cross(back, right);
        float s = dist * 0.012f;
        dot.space = g_stage;
        dot.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        dot.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        dot.subImage.swapchain = g_swaps[4].handle;
        dot.subImage.imageRect = FullRect(g_swaps[4]);
        dot.pose = ToXr({QuatFromBasis(right, up, back), in.dotPos});
        dot.size = {s, s};
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&dot;
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_frame.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    Ok(xrEndFrame(g_session, &ei), "xrEndFrame");
}

Controls GetControls() {
    std::lock_guard<std::mutex> l(g_ctrlLock);
    return g_controls;
}

void Haptic(int hand, float amplitude, float seconds, float frequency) {
    std::lock_guard<std::mutex> l(g_ctrlLock);
    if (g_haptics.size() < 16) g_haptics.push_back({hand, amplitude, seconds, frequency});
}

void PlaceScreen() { g_placeScreen = true; }

bool RawAim(int hand, Pose& out) {
    std::lock_guard<std::mutex> l(g_ctrlLock);
    if (hand < 0 || hand > 1 || !g_rawAim[hand].valid) return false;
    out = g_rawAim[hand].pose;
    return true;
}

DXGI_FORMAT DotFormat() { return g_format; }

bool HudRect(Vec3& centre, Vec3& right, Vec3& up, float& halfW, float& halfH) {
    if (!g_swaps[3].handle) return false;
    Pose p = ToPose(g_hudPose);
    centre = p.p;
    right = Rotate(p.q, {1, 0, 0});
    up = Rotate(p.q, {0, 1, 0});
    halfW = config::Get().hudWidth * 0.5f;
    halfH = halfW * g_swaps[3].height / (float)g_swaps[3].width;
    return true;
}

bool ScreenRect(Vec3& centre, Vec3& right, Vec3& up, float& halfW, float& halfH) {
    if (!g_swaps[2].handle) return false;
    Pose p = ToPose(g_screenPose);
    centre = p.p;
    right = Rotate(p.q, {1, 0, 0});
    up = Rotate(p.q, {0, 1, 0});
    halfW = config::Get().screenWidth * 0.5f;
    halfH = halfW / g_screenAspect;
    return true;
}

} // namespace jvr::xr
