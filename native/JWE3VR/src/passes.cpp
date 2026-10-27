#include "passes.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "rig.h"
#include "viewcb.h"

#include <cmath>
#include <unordered_map>

namespace jvr::passes {

struct RtvInfo {
    ID3D12Resource* resource = nullptr;
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the view's format
};

struct ListState {
    std::vector<std::string> scopes;
    int uiDepth = -1;  // scope index where the UI pass began (-1: not inside)
    int taaDepth = -1; // scope index where the handled TAA pass began
    int taaEye = -1;
    ID3D12Resource* taaOutput = nullptr;
    bool vrsBound = false;
    bool checked5 = false;
    ID3D12GraphicsCommandList5* l5 = nullptr; // not AddRef'd: valid while the list lives
    ID3D12Resource* bbToRt = nullptr;         // a backbuffer this list transitioned to RENDER_TARGET
    ID3D12Resource* uiToRt = nullptr;         // the game's UI texture, made a render target by this list
};

static SRWLOCK g_rtvLock = SRWLOCK_INIT;
static std::unordered_map<SIZE_T, RtvInfo> g_rtvs;
static SRWLOCK g_listLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;
static SRWLOCK g_stateLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_states; // last known whole-resource state

static SRWLOCK g_frameLock = SRWLOCK_INIT;
static FrameInfo g_frame;
static std::atomic<uint64_t> g_frameIndex{0};

// Commands we insert into game lists go through the hooked functions too; this keeps them out of our tracking.
static thread_local bool t_ours = false;

// Foveation
static bool g_vrsChecked = false, g_vrsSupported = false, g_vrs4x4 = false;
static UINT g_tile = 16;
static ID3D12Resource* g_sri = nullptr;
static UINT g_sriW = 0, g_sriH = 0;
static float g_sriFov = 0.f, g_sriAspect = 0.f;
static int g_sriMode = -1;
static bool g_sriFresh = false;

// In-world HUD
// Our UI texture and its views, published whole: game threads redirect into it while the render thread may replace
// it. Each generation has its own descriptor heap, so a view a game thread is reading is never rewritten; old
// generations are kept (lists in flight may still draw into them; replacements are rare).
struct UiTex {
    ID3D12Resource* r = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE unorm{}, srgb{}; // UNORM (or the one typed format) and sRGB views
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // resource format (typeless for 8-bit colour)
};
static std::atomic<const UiTex*> g_uiTex{nullptr};
static std::atomic<DXGI_FORMAT> g_uiWantFormat{DXGI_FORMAT_UNKNOWN}; // the view format the game's UI draws through
static std::atomic<UINT> g_uiWantW{0}, g_uiWantH{0};                 // the game's UI texture size (0: the frame's)
static std::atomic<bool> g_uiUsedThisFrame{false};
static std::atomic<uint64_t> g_uiLastFrame{0};
static std::atomic<UINT> g_rtvIncrement{0};
static std::atomic<unsigned> g_bbBindsThisFrame{0}; // render-target bindings of the backbuffer this frame
static unsigned g_bbBindsTotal = 0, g_bbFrames = 0;
// UIDetect = texture: the textures the game draws its UI into (learned), and the redirect's state.
constexpr int kUiTargets = 4;
static std::atomic<ID3D12Resource*> g_uiTargets[kUiTargets]{};
static std::atomic<uint64_t> g_uiTargetSeen[kUiTargets]{}; // frame index it last left RENDER_TARGET for COPY_SOURCE
static std::atomic<unsigned> g_uiTargetNext{0};
// A UI texture not seen for this many frames is forgotten: leaving a park frees it, and a new resource at the same
// address must not have its draws taken for UI (the next park's UI textures are learned anew).
constexpr uint64_t kUiStaleFrames = 120;
static std::mutex g_uiTraceLock;
static std::string g_uiTrace;
static std::atomic<int> g_uiTraceFrames{0};
static std::atomic<bool> g_hudOutputReady{false}; // the panel swapchain can take our UI texture (set at Present)
static std::atomic<bool> g_uiRedirecting{false};  // the last binding of a UI texture was redirected (for the log)

// Per-eye TAA: a few stashes per eye, one per texture shape seen.
struct Stash {
    ID3D12Resource* r = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    bool valid = false;
};
static std::vector<Stash> g_stash[2];
static ID3D12Resource* g_lastTaaOutput = nullptr;
static std::atomic<uint64_t> g_taaHandledFrame{UINT64_MAX};
static bool g_taaWasWanted = false;
static std::mutex g_taaLock;

bool MarkerMatches(const std::string& name, const std::string& keys) {
    if (name.empty()) return false;
    size_t start = 0;
    while (start < keys.size()) {
        size_t end = keys.find(',', start);
        if (end == std::string::npos) end = keys.size();
        std::string key = keys.substr(start, end - start);
        while (!key.empty() && key.front() == ' ') key.erase(key.begin());
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (!key.empty() && name.find(key) != std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

static FrameInfo Frame() {
    AcquireSRWLockShared(&g_frameLock);
    FrameInfo f = g_frame;
    ReleaseSRWLockShared(&g_frameLock);
    return f;
}

static bool IsHdr(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return true;
    default:
        return false;
    }
}

static bool LookupRtv(D3D12_CPU_DESCRIPTOR_HANDLE h, RtvInfo& out) {
    AcquireSRWLockShared(&g_rtvLock);
    auto it = g_rtvs.find(h.ptr);
    bool ok = it != g_rtvs.end();
    if (ok) out = it->second;
    ReleaseSRWLockShared(&g_rtvLock);
    return ok;
}

void OnCreateRtv(ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!r) return;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    RtvInfo info{r, d.Width, d.Height, desc ? desc->Format : d.Format};
    AcquireSRWLockExclusive(&g_rtvLock);
    g_rtvs[h.ptr] = info;
    ReleaseSRWLockExclusive(&g_rtvLock);
}

static ListState& StateOf(ID3D12GraphicsCommandList* l) { return g_lists[l]; } // caller holds g_listLock

void OnReset(ID3D12GraphicsCommandList* list) {
    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    s.scopes.clear();
    s.uiDepth = s.taaDepth = -1;
    s.taaOutput = nullptr;
    s.vrsBound = false;
    s.bbToRt = s.uiToRt = nullptr;
    ReleaseSRWLockExclusive(&g_listLock);
}

// ---------------------------------------------------------------- foveation

// Full rate in the middle, then half (2x1), quarter (2x2) and, where supported, 4x4 at the edges.
static uint8_t RateFor(float angleDeg, const Settings& c) {
    if (angleDeg < c.foveaInnerDeg) return D3D12_SHADING_RATE_1X1;
    if (angleDeg < c.foveaMidDeg) return D3D12_SHADING_RATE_2X1;
    if (angleDeg < c.foveaOuterDeg || !g_vrs4x4) return D3D12_SHADING_RATE_2X2;
    return D3D12_SHADING_RATE_4X4;
}

static void BuildShadingRateImage(const FrameInfo& f) {
    const Settings& c = config::Get();
    ID3D12Device* dev = render::Device();
    if (!dev) return;
    if (!g_vrsChecked) {
        g_vrsChecked = true;
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) &&
            o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2) {
            g_vrsSupported = true;
            g_vrs4x4 = o6.AdditionalShadingRatesSupported;
            g_tile = o6.ShadingRateImageTileSize ? o6.ShadingRateImageTileSize : 16;
            LOGI("foveation: VRS tier 2, %u px tiles, 4x4 %s", g_tile, g_vrs4x4 ? "yes" : "no");
        } else {
            LOGW("foveation: this GPU has no VRS tier 2; foveated rendering is off");
        }
    }
    if (!g_vrsSupported) return;
    UINT w = (f.width + g_tile - 1) / g_tile, h = (f.height + g_tile - 1) / g_tile;
    bool same = g_sri && w == g_sriW && h == g_sriH && std::fabs(f.fovY - g_sriFov) < 0.01f &&
                std::fabs(f.aspect - g_sriAspect) < 0.01f && g_sriMode == c.foveation;
    if (same) return;

    if (!g_sri || w != g_sriW || h != g_sriH) {
        // Lists recorded for the next frame may already bind the old image: keep it alive (resizes are rare).
        g_sri = nullptr;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8_UINT;
        rd.SampleDesc.Count = 1;
        ID3D12Resource* sri = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&sri)))) {
            LOGE("foveation: shading-rate image creation failed");
            g_vrsSupported = false;
            return;
        }
        g_sri = sri;
        g_sriW = w;
        g_sriH = h;
        g_sriFresh = true;
    }

    // Fill on the CPU in tangent space: the angle of each tile's centre from the eye's forward axis.
    UINT pitch = (w + 255) & ~255u;
    std::vector<uint8_t> rates((size_t)pitch * h, 0);
    float tv = std::tan(f.fovY * 0.5f), th = tv * f.aspect;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            float u = ((x + 0.5f) * g_tile) / f.width, v = ((y + 0.5f) * g_tile) / f.height;
            float tx = (u - 0.5f) * 2.f * th, ty = (v - 0.5f) * 2.f * tv;
            float deg = std::atan(std::sqrt(tx * tx + ty * ty)) * 57.29578f;
            rates[(size_t)y * pitch + x] = RateFor(deg, c);
        }

    ID3D12Resource* upload = nullptr;
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rates.size();
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&upload))))
        return;
    void* p = nullptr;
    upload->Map(0, nullptr, &p);
    memcpy(p, rates.data(), rates.size());
    upload->Unmap(0, nullptr);

    bool fresh = g_sriFresh; // a new image starts in COPY_DEST; an existing one is in SHADING_RATE_SOURCE
    g_sriFresh = false;
    ID3D12Resource* sri = g_sri;
    render::Submit([&](ID3D12GraphicsCommandList* l) {
        if (!fresh) {
            D3D12_RESOURCE_BARRIER b = render::Transition(sri, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            l->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{sri, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, w, h, 1, pitch};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = render::Transition(sri, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        l->ResourceBarrier(1, &b);
    });
    if (render::WaitCopies()) upload->Release(); // else leak it: the GPU may still read it
    g_sriFov = f.fovY;
    g_sriAspect = f.aspect;
    g_sriMode = c.foveation;
    LOGI("foveation: %ux%u rate image for %.0f deg FOV (inner %.0f / mid %.0f / outer %.0f deg)", w, h, f.fovY * 57.29578f,
         c.foveaInnerDeg, c.foveaMidDeg, c.foveaOuterDeg);
}

static int UiTextureIndex(ID3D12Resource* r);

// Per frame: frame-sized render-target bindings, and those that got the rate image (logged once a minute: JWE3
// shades materials into 8-bit G-buffer targets and lights them in compute, which shading rates do not reach).
static std::atomic<unsigned> g_vrsFrameSized{0}, g_vrsBoundBinds{0};

// Colour targets coarse shading may write: albedo / HDR / packed-colour formats. Integer targets (JWE3's frame-sized
// R32_UINT G-buffer IDs) and two-channel ones (R16G16 motion vectors) are left at full rate: shaded coarsely they gave
// dark seams in the foveated region (run 62).
static bool IsShadedColour(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return IsHdr(f);
    }
}

static void ApplyVrs(ID3D12GraphicsCommandList* list, ListState& s, const RtvInfo* rt0, const FrameInfo& f) {
    const Settings& c = config::Get();
    bool frameSized = rt0 && rt0->width == f.width && rt0->height == f.height;
    // The UI textures stay at full rate in every mode (they become the HUD panel).
    bool want = c.foveation > 0 && g_vrsSupported && g_sri && f.immersive && frameSized && s.uiDepth < 0 &&
                (c.foveation == 2 ? IsShadedColour(rt0->format) : IsHdr(rt0->format)) && UiTextureIndex(rt0->resource) < 0;
    if (frameSized) {
        ++g_vrsFrameSized;
        if (want) ++g_vrsBoundBinds;
    }
    if (!s.checked5) {
        s.checked5 = true;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&s.l5)))) s.l5->Release(); // same object as the list
    }
    if (!s.l5 || want == s.vrsBound) return;
    if (want) {
        D3D12_SHADING_RATE_COMBINER comb[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE};
        s.l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, comb);
        s.l5->RSSetShadingRateImage(g_sri);
    } else {
        s.l5->RSSetShadingRateImage(nullptr);
        s.l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
    }
    s.vrsBound = want;
}

// ---------------------------------------------------------------- in-world HUD

static void ClearUi();

// 8-bit colour formats come as a typeless group with UNORM and sRGB views; JWE3 draws its UI through both.
static DXGI_FORMAT TypelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:
        return f; // other formats: one view format, the resource's own
    }
}

static bool IsColor8(DXGI_FORMAT f) { return TypelessOf(f) != f || f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS; }

static bool IsSrgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }

static DXGI_FORMAT UnormOf(DXGI_FORMAT f) {
    switch (TypelessOf(f)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static DXGI_FORMAT SrgbOf(DXGI_FORMAT f) {
    switch (TypelessOf(f)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

// A size and format the UI texture could not be created in: not retried every frame (each try would log again).
static UINT g_uiFailW = 0, g_uiFailH = 0;
static DXGI_FORMAT g_uiFailFormat = DXGI_FORMAT_UNKNOWN;

static void EnsureUiTexture(const FrameInfo& f) {
    const Settings& c = config::Get();
    if (!c.worldHud || !f.width) return;
    DXGI_FORMAT want = g_uiWantFormat.load();
    // Texture mode: wait until a UI texture was bound (its format and size are not the backbuffer's).
    if (want == DXGI_FORMAT_UNKNOWN && c.uiDetect == UiDetect::Texture) return;
    if (want == DXGI_FORMAT_UNKNOWN) want = f.format;
    want = TypelessOf(want);
    UINT w = g_uiWantW.load() ? g_uiWantW.load() : f.width, h = g_uiWantH.load() ? g_uiWantH.load() : f.height;
    const UiTex* cur = g_uiTex.load();
    if (cur && cur->w == w && cur->h == h && cur->format == want) return;
    if (w == g_uiFailW && h == g_uiFailH && want == g_uiFailFormat) return;
    ID3D12Device* dev = render::Device();
    if (!dev) return;
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
        LOGE("in-world HUD: descriptor heap creation failed");
        g_uiFailW = w, g_uiFailH = h, g_uiFailFormat = want;
        return;
    }
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_rtvIncrement = inc;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = want; // the game's group: its UI pipelines draw through views of these formats
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{UnormOf(want), {0, 0, 0, 0}};
    ID3D12Resource* tex = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&tex)))) {
        LOGE("in-world HUD: UI texture creation failed (format %d, %ux%u); the UI stays on the eyes", (int)want, w, h);
        g_uiFailW = w, g_uiFailH = h, g_uiFailFormat = want;
        heap->Release();
        return;
    }
    UiTex* next = new UiTex;
    next->r = tex;
    next->unorm = heap->GetCPUDescriptorHandleForHeapStart();
    next->srgb = D3D12_CPU_DESCRIPTOR_HANDLE{next->unorm.ptr + inc};
    next->w = w;
    next->h = h;
    next->format = want;
    D3D12_RENDER_TARGET_VIEW_DESC vd{UnormOf(want), D3D12_RTV_DIMENSION_TEXTURE2D};
    dev->CreateRenderTargetView(tex, &vd, next->unorm);
    vd.Format = SrgbOf(want);
    dev->CreateRenderTargetView(tex, &vd, next->srgb);
    g_uiTex.store(next); // the views are written before anyone can see them; the old generation stays alive
    g_uiRedirecting = false; // a new texture starts empty: hand the UI over again
    LOGI("in-world HUD: UI texture %ux%u format %d", w, h, (int)want);
    ClearUi();
}

// Our view (of generation `t`) for a game view of format `view`, if our texture can take it (same format group).
static bool UiViewFor(const UiTex* t, DXGI_FORMAT view, D3D12_CPU_DESCRIPTOR_HANDLE& out) {
    if (!t || TypelessOf(view) != t->format) return false;
    out = IsSrgb(view) ? t->srgb : t->unorm;
    return true;
}

DXGI_FORMAT HudViewFormat() {
    const UiTex* t = g_uiTex.load();
    return t ? SrgbOf(t->format) : DXGI_FORMAT_UNKNOWN;
}

static void ClearUi() {
    const UiTex* t = g_uiTex.load();
    if (!t) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = t->unorm;
    render::Submit([&](ID3D12GraphicsCommandList* l) {
        const float clear[4] = {0, 0, 0, 0};
        l->ClearRenderTargetView(rtv, clear, 0, nullptr);
    });
}

// After the panel copy: the game draws its whole UI anew every frame (JWE3 clears each UI texture the frame after
// drawing it), so ours starts the next frame transparent.
void ClearHud() { ClearUi(); }

void SetHudOutputReady(bool ready) { g_hudOutputReady = ready; }

// Backbuffer formats whose render-target view can only have the resource's own format (no sRGB twin), so a
// learned view's format is certain (the HUD texture must match the game's pipeline exactly).
static bool ViewFormatCertain(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// The texture the game copies into the backbuffer each frame (JWE3 composes scene + UI into it, then copies).
static std::atomic<ID3D12Resource*> g_finalTex{nullptr};

void OnCopyToBackbuffer(ID3D12Resource* src) {
    if (!src) return;
    ID3D12Resource* prev = g_finalTex.exchange(src);
    if (prev != src) LOGI("in-world HUD: the game copies texture %p into the backbuffer", (void*)src);
}

// The backbuffer itself, or the texture the game copies into it.
static bool IsFinalImage(ID3D12Resource* r, const FrameInfo& f) {
    if (!r) return false;
    if (r == g_finalTex.load()) return true;
    for (ID3D12Resource* b : f.backbuffers)
        if (b && b == r) return true;
    return false;
}

static bool IsBackbuffer(const RtvInfo& rt, const FrameInfo& f) { return IsFinalImage(rt.resource, f); }

static D3D12_CPU_DESCRIPTOR_HANDLE HandleAt(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single, UINT i) {
    if (!single || i == 0) return rtvs[i];
    UINT inc = g_rtvIncrement.load();
    if (!inc)
        if (ID3D12Device* dev = render::Device()) g_rtvIncrement = inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + (SIZE_T)i * inc};
}

// The game's UI textures (it may use more than one, e.g. one per frame in flight).
static int UiTextureIndex(ID3D12Resource* r) {
    if (!r) return -1;
    uint64_t now = g_frameIndex.load();
    for (int i = 0; i < kUiTargets; ++i)
        if (g_uiTargets[i].load() == r && now - g_uiTargetSeen[i].load() < kUiStaleFrames) return i;
    return -1;
}

// Debug: what the game does with its UI textures in the first frames after they are found (b = bound,
// c = cleared, r = made a render target, x = left for COPY_SOURCE, each followed by the texture's number), one
// log line per frame. Run 25: two textures in turn, "c0 x0 r1 r0 b1 x1 r1 b1" - the frame's UI is drawn into
// one while the other (last frame's) is cleared.
static void Trace(char what, int index) {
    if (index < 0 || g_uiTraceFrames.load() >= 24) return;
    std::lock_guard<std::mutex> l(g_uiTraceLock);
    g_uiTrace += what;
    g_uiTrace += (char)('0' + index);
    g_uiTrace += ' ';
}

// JWE3 draws its UI (Gameface) into 8-bit colour textures of its own, copies them out of RENDER_TARGET and later
// composites them into the final image in a compute pass. A frame-sized 8-bit target leaving RENDER_TARGET for
// COPY_SOURCE is taken to be one (run 23's census found one such target per frame: the HUD on transparent black).
static void LearnUiTarget(ID3D12Resource* r, const FrameInfo& f) {
    uint64_t now = g_frameIndex.load();
    for (int i = 0; i < kUiTargets; ++i)
        if (g_uiTargets[i].load() == r && now - g_uiTargetSeen[i].load() < kUiStaleFrames) {
            g_uiTargetSeen[i] = now; // still in use
            return;
        }
    if (IsFinalImage(r, f) || !f.width) return;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1 || !IsColor8(d.Format))
        return;
    if (d.Width * 2 < f.width || d.Height * 2 < f.height) return;
    // A forgotten slot first, else round robin.
    int slot = -1;
    for (int i = 0; i < kUiTargets && slot < 0; ++i)
        if (!g_uiTargets[i].load() || now - g_uiTargetSeen[i].load() >= kUiStaleFrames) slot = i;
    if (slot < 0) slot = (int)(g_uiTargetNext.fetch_add(1) % kUiTargets);
    g_uiTargetSeen[slot] = now;
    g_uiTargets[slot] = r;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 16)
        LOGI("in-world HUD: the game draws its UI into texture %p (#%u, %llux%u, format %d); those draws go to the HUD panel",
             (void*)r, slot, (unsigned long long)d.Width, d.Height, (int)d.Format);
}

// UIDetect = texture. A binding of a game UI texture draws into ours instead, and the game's own is cleared there
// (it is a render target at that point), so its compositing pass finds no UI for the eyes. The game's own clears
// are left alone: they clear last frame's texture, while this frame's UI is already being drawn into ours.
static bool RedirectUiTexture(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                              D3D12_CPU_DESCRIPTOR_HANDLE* out, const FrameInfo& f, const Settings& c) {
    if (!g_uiTargets[0].load()) return false;
    int at = -1, index = -1;
    RtvInfo rt{};
    for (UINT i = 0; i < count && at < 0; ++i)
        if (LookupRtv(HandleAt(rtvs, single, i), rt) && (index = UiTextureIndex(rt.resource)) >= 0) at = (int)i;
    if (at < 0) return false;
    Trace('b', index);
    ID3D12Resource* target = rt.resource;
    D3D12_RESOURCE_DESC d = target->GetDesc();
    D3D12_CPU_DESCRIPTOR_HANDLE ours{};
    const UiTex* tex = g_uiTex.load(); // one generation for the whole decision
    bool want = c.worldHud && f.immersive && f.worldHudAllowed;
    if (want && (!UiViewFor(tex, rt.format, ours) || d.Width != tex->w || d.Height != tex->h)) {
        // Ours must be in the game's format group (pipeline formats) and of its size: (re)created at the next Present.
        g_uiWantFormat = rt.format;
        g_uiWantW = (UINT)d.Width;
        g_uiWantH = d.Height;
        want = false;
    }
    want = want && g_hudOutputReady.load();

    AcquireSRWLockShared(&g_listLock);
    auto it = g_lists.find(list);
    bool isRt = it != g_lists.end() && it->second.uiToRt == target; // this list made it a render target: state known
    ReleaseSRWLockShared(&g_listLock);

    if (g_uiRedirecting.exchange(want) != want)
        LOGI("in-world HUD: %s", want ? "the UI now goes to the panel" : "the UI is back in the frame");
    if (!want) return false;
    if (isRt) {
        t_ours = true;
        const float clear[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(HandleAt(rtvs, single, (UINT)at), clear, 0, nullptr);
        t_ours = false;
    } else {
        JVR_LOG_ONCE("in-world HUD: a UI texture was bound without a known state; its old content is not cleared");
    }
    for (UINT i = 0; i < count; ++i) out[i] = (int)i == at ? ours : HandleAt(rtvs, single, i);
    g_uiUsedThisFrame = true;
    return true;
}

void OnClearRtv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (t_ours || !g_uiTargets[0].load() || g_uiTraceFrames.load() >= 24) return;
    RtvInfo rt{};
    if (LookupRtv(h, rt)) Trace('c', UiTextureIndex(rt.resource));
}

// ---------------------------------------------------------------- resource census (debug)
// JWE3VR_rt.req. Frame 1 (listing): every large texture (a quarter of the frame or more) the frame binds as a render
// target, transitions or copies is logged with its formats and, in recording order, what happened to it:
// B bound as a render target, W made a render target, U made a UAV, R made a shader resource, S / D made a copy
// source / destination, P present or common, Z / z depth write / read, ? other. Copies between them are listed too.
// Frame 2 (capturing): each one is copied out inside the game's list just before it first leaves RENDER_TARGET or
// UNORDERED_ACCESS (its state is then known), and the copies are saved a few seconds later as JWE3VR_rt<n>.bmp.
// Finds where the game draws something, and how it keeps temporal history, when it has no pass markers.

struct CensusEntry {
    ID3D12Resource* r = nullptr;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN; // a render-target view format seen (else the resource's is used)
    unsigned binds = 0;
    std::string events;
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
};
enum class CensusStep { Off, Requested, Listing, Capturing, Saving };
static std::mutex g_censusLock;
static CensusStep g_censusStep = CensusStep::Off;
static std::vector<CensusEntry> g_census;
static std::vector<std::pair<int, int>> g_censusCopies; // (source, destination) entry numbers
static std::vector<ID3D12Resource*> g_censusOld;        // last census's readback buffers, freed at the next one
static unsigned g_censusBinds = 0, g_censusUnknown = 0, g_censusCaptures = 0;
static ULONGLONG g_censusSaveAt = 0;
static std::atomic<bool> g_censusActive{false}; // Listing or Capturing (cheap check in the hooks)
constexpr size_t kCensusMax = 96;
constexpr unsigned kCensusMaxCaptures = 24;

void RequestCensus() {
    std::lock_guard<std::mutex> l(g_censusLock);
    if (g_censusStep == CensusStep::Off) g_censusStep = CensusStep::Requested;
}

// The entry of `r`, added if it is a large 2D texture (caller holds g_censusLock).
static int CensusIndex(ID3D12Resource* r, bool add) {
    if (!r) return -1;
    for (size_t i = 0; i < g_census.size(); ++i)
        if (g_census[i].r == r) return (int)i;
    if (!add || g_census.size() >= kCensusMax) return -1;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    FrameInfo f = Frame();
    UINT64 fw = f.width ? f.width : 1920, fh = f.height ? f.height : 1080;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width * d.Height * 4 < fw * fh) return -1;
    CensusEntry e;
    e.r = r;
    g_census.push_back(e);
    return (int)g_census.size() - 1;
}

static void CensusEvent(int index, char what) {
    if (index >= 0 && g_census[index].events.size() < 48) g_census[index].events += what;
}

static void CensusBind(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single) {
    if (!rtvs || count > 8) return;
    std::lock_guard<std::mutex> l(g_censusLock);
    if (g_censusStep != CensusStep::Listing) return;
    for (UINT i = 0; i < count; ++i) {
        RtvInfo rt{};
        if (!LookupRtv(HandleAt(rtvs, single, i), rt)) {
            ++g_censusUnknown;
            continue;
        }
        ++g_censusBinds;
        int index = CensusIndex(rt.resource, true);
        if (index < 0) continue;
        g_census[index].view = rt.format;
        ++g_census[index].binds;
        CensusEvent(index, 'B');
    }
}

static char StateLetter(D3D12_RESOURCE_STATES s) {
    if (s == D3D12_RESOURCE_STATE_RENDER_TARGET) return 'W';
    if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return 'U';
    if (s & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) return 'R';
    if (s == D3D12_RESOURCE_STATE_COPY_SOURCE) return 'S';
    if (s == D3D12_RESOURCE_STATE_COPY_DEST) return 'D';
    if (s == D3D12_RESOURCE_STATE_COMMON) return 'P'; // also PRESENT (same value)
    if (s == D3D12_RESOURCE_STATE_DEPTH_WRITE) return 'Z';
    if (s & D3D12_RESOURCE_STATE_DEPTH_READ) return 'z';
    return '?';
}

// Copies subresource 0 of `e` (now in state `from`) into a new readback buffer inside the game's list.
static void CensusCapture(ID3D12GraphicsCommandList* list, CensusEntry& e, D3D12_RESOURCE_STATES from) {
    D3D12_RESOURCE_DESC d = e.r->GetDesc();
    DXGI_FORMAT format = e.view != DXGI_FORMAT_UNKNOWN ? e.view : d.Format;
    if (d.SampleDesc.Count != 1 || !render::DebugFormatSupported(format)) return;
    ID3D12Device* dev = render::Device();
    if (!dev || list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY) return;
    if (from == D3D12_RESOURCE_STATE_RENDER_TARGET && list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &e.fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&e.readback))))
        return;
    ++g_censusCaptures;
    t_ours = true;
    D3D12_RESOURCE_BARRIER b = render::Transition(e.r, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.Transition.Subresource = 0;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{e.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = e.fp;
    D3D12_TEXTURE_COPY_LOCATION src{e.r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    t_ours = false;
}

static void CensusBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* b) {
    std::lock_guard<std::mutex> l(g_censusLock);
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& x = b[i];
        if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || x.Flags == D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) continue;
        if (g_censusStep == CensusStep::Listing) {
            CensusEvent(CensusIndex(x.Transition.pResource, true), StateLetter(x.Transition.StateAfter));
            continue;
        }
        if (g_censusStep != CensusStep::Capturing || g_censusCaptures >= kCensusMaxCaptures) continue;
        D3D12_RESOURCE_STATES from = x.Transition.StateBefore;
        if (from != D3D12_RESOURCE_STATE_RENDER_TARGET && from != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) continue;
        if (x.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && x.Transition.Subresource != 0) continue;
        int index = CensusIndex(x.Transition.pResource, false);
        if (index >= 0 && !g_census[index].readback) CensusCapture(list, g_census[index], from); // written, not yet moved on
    }
}

void OnCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    if (!g_censusActive.load() || t_ours) return;
    std::lock_guard<std::mutex> l(g_censusLock);
    if (g_censusStep != CensusStep::Listing || g_censusCopies.size() >= 32) return;
    int s = CensusIndex(src, true), d = CensusIndex(dst, true);
    if (s >= 0 || d >= 0) g_censusCopies.push_back({s, d});
}

// Advances the census one step per Present.
static void CensusAtPresent() {
    std::lock_guard<std::mutex> l(g_censusLock);
    switch (g_censusStep) {
    case CensusStep::Off:
        break;
    case CensusStep::Requested:
        for (ID3D12Resource* r : g_censusOld) r->Release(); // the frame that used them finished long ago
        g_censusOld.clear();
        g_census.clear();
        g_censusCopies.clear();
        g_censusBinds = g_censusUnknown = g_censusCaptures = 0;
        g_censusStep = CensusStep::Listing;
        g_censusActive = true;
        break;
    case CensusStep::Listing: {
        ID3D12Resource* fin = g_finalTex.load();
        LOGI("census: %u render-target bindings in one frame (%u through views created before the mod loaded), %zu large "
             "textures. Events: B bound, W render target, U UAV, R shader resource, S/D copy source/dest, P common, Z/z depth",
             g_censusBinds, g_censusUnknown, g_census.size());
        for (size_t n = 0; n < g_census.size(); ++n) {
            const CensusEntry& e = g_census[n];
            D3D12_RESOURCE_DESC d = e.r->GetDesc();
            int ui = UiTextureIndex(e.r);
            LOGI("census: #%zu %p %llux%u format %d (view %d) mips %u flags %#x: %s%s%s", n, (void*)e.r, (unsigned long long)d.Width,
                 d.Height, (int)d.Format, (int)e.view, (unsigned)d.MipLevels, (unsigned)d.Flags, e.events.empty() ? "-" : e.events.c_str(),
                 e.r == fin ? "  <- final image" : "", ui >= 0 ? "  <- UI texture" : "");
        }
        for (const auto& c : g_censusCopies) LOGI("census: copy #%d -> #%d", c.first, c.second);
        g_censusStep = CensusStep::Capturing;
        break;
    }
    case CensusStep::Capturing:
        g_censusActive = false;
        g_censusStep = CensusStep::Saving;
        g_censusSaveAt = GetTickCount64() + 3000; // the captured frame has long finished on the GPU by then
        break;
    case CensusStep::Saving: {
        if (GetTickCount64() < g_censusSaveAt) break;
        int saved = 0;
        for (size_t n = 0; n < g_census.size(); ++n) {
            CensusEntry& e = g_census[n];
            if (!e.readback) continue;
            wchar_t file[64];
            swprintf(file, 64, L"JWE3VR_rt%zu.bmp", n);
            D3D12_RESOURCE_DESC d = e.r->GetDesc();
            if (render::SaveReadbackBmp(e.readback, e.fp, e.view != DXGI_FORMAT_UNKNOWN ? e.view : d.Format, (UINT)d.Width, d.Height,
                                        file))
                ++saved;
            g_censusOld.push_back(e.readback);
            e.readback = nullptr;
        }
        LOGI("census: saved %d captures as JWE3VR_rt<n>.bmp (n = census number)", saved);
        g_censusStep = CensusStep::Off;
        break;
    }
    }
}

bool OnSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                        D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    const Settings& c = config::Get();
    if (!c.enabled || t_ours) return false;
    if (g_censusActive.load()) CensusBind(count, rtvs, single);
    FrameInfo f = Frame();
    // Before the immersive check: leaving immersive mode hands the UI back to the game's texture.
    bool uiRedirected = c.uiDetect == UiDetect::Texture && count > 0 && count <= 8 && rtvs &&
                        RedirectUiTexture(list, count, rtvs, single, out, f, c);
    if (!f.immersive || (c.foveation <= 0 && !c.worldHud)) return uiRedirected; // nothing else to do: skip the locks
    RtvInfo rt0{};
    bool have0 = count > 0 && rtvs && LookupRtv(rtvs[0], rt0);

    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    if (!have0 && count == 1 && rtvs && s.bbToRt) {
        // An unknown view bound right after this list made a backbuffer a render target: that backbuffer's view.
        D3D12_RESOURCE_DESC d = s.bbToRt->GetDesc();
        if (ViewFormatCertain(d.Format)) {
            rt0 = RtvInfo{s.bbToRt, d.Width, d.Height, d.Format};
            have0 = true;
            AcquireSRWLockExclusive(&g_rtvLock);
            g_rtvs[rtvs[0].ptr] = rt0;
            ReleaseSRWLockExclusive(&g_rtvLock);
            LOGI("in-world HUD: learned a backbuffer render-target view (format %d)", (int)d.Format);
        } else {
            JVR_LOG_ONCE("in-world HUD: backbuffer view format uncertain (%d); not learning its views", (int)d.Format);
        }
    }
    bool inUi = s.uiDepth >= 0;
    ApplyVrs(list, s, have0 ? &rt0 : nullptr, f);
    ReleaseSRWLockExclusive(&g_listLock);

    if (count == 0 || count > 8 || !rtvs || c.uiDetect == UiDetect::Texture) return uiRedirected;
    // Backbuffer bindings of this frame (counted for the log, and the UI rule when there are no markers).
    bool bindsBackbuffer = false;
    for (UINT i = 0; i < count && !bindsBackbuffer; ++i) {
        RtvInfo rt{};
        bindsBackbuffer = LookupRtv(HandleAt(rtvs, single, i), rt) && IsBackbuffer(rt, f);
    }
    if (!bindsBackbuffer) return false;
    unsigned bindIndex = g_bbBindsThisFrame.fetch_add(1);
    if (c.uiDetect == UiDetect::Backbuffer) inUi = bindIndex >= (unsigned)std::max(0, c.uiSkipBackbufferBinds);
    const UiTex* tex = g_uiTex.load();
    if (!inUi || !c.worldHud || !f.worldHudAllowed || !tex || !g_hudOutputReady.load()) return false;
    bool replaced = false;
    for (UINT i = 0; i < count; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = HandleAt(rtvs, single, i);
        RtvInfo rt{};
        out[i] = h;
        if (!LookupRtv(h, rt) || !IsBackbuffer(rt, f)) continue;
        D3D12_CPU_DESCRIPTOR_HANDLE ours{};
        if (!UiViewFor(tex, rt.format, ours)) {
            // Draw through the same view format as the game's pipeline: recreate at the next Present.
            g_uiWantFormat = rt.format;
            return false;
        }
        out[i] = ours;
        replaced = true;
    }
    if (!replaced) return false;
    g_uiUsedThisFrame = true;
    return true;
}

ID3D12Resource* TakeHudTexture() {
    if (!g_uiUsedThisFrame.exchange(false)) return nullptr;
    g_uiLastFrame = g_frameIndex.load();
    return PeekHudTexture();
}

ID3D12Resource* PeekHudTexture() {
    const UiTex* t = g_uiTex.load();
    return t ? t->r : nullptr;
}

bool WorldHudActive() {
    uint64_t now = g_frameIndex.load(), last = g_uiLastFrame.load();
    return config::Get().worldHud && last > 0 && now - last < 60;
}

// ---------------------------------------------------------------- per-eye TAA

static D3D12_RESOURCE_STATES KnownState(ID3D12Resource* r, bool& known) {
    AcquireSRWLockShared(&g_stateLock);
    auto it = g_states.find(r);
    known = it != g_states.end();
    D3D12_RESOURCE_STATES s = known ? it->second : D3D12_RESOURCE_STATE_COMMON;
    ReleaseSRWLockShared(&g_stateLock);
    return s;
}

// Per-eye history is only right together with the previous-camera fix (viewcb): without it the game would
// reproject this eye's history from the other eye's position.
static bool TaaWanted(const FrameInfo& f) {
    const Settings& c = config::Get();
    return c.enabled && c.perEyeTaa && f.aer && f.immersive && viewcb::CameraRowsKnown();
}

static bool SameShape(ID3D12Resource* a, ID3D12Resource* b) {
    D3D12_RESOURCE_DESC x = a->GetDesc(), y = b->GetDesc();
    return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format && x.MipLevels == 1 && x.DepthOrArraySize == 1 &&
           x.SampleDesc.Count == 1;
}

// The stash of this eye for textures shaped like `like` (created once per shape, at most 4 per eye).
static Stash* StashFor(int eye, ID3D12Resource* like, bool create) {
    for (Stash& s : g_stash[eye])
        if (SameShape(s.r, like)) return &s;
    if (!create || g_stash[eye].size() >= 4) return nullptr;
    ID3D12Device* dev = render::Device();
    if (!dev) return nullptr;
    D3D12_RESOURCE_DESC want = like->GetDesc();
    want.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &want, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    g_stash[eye].push_back({r, D3D12_RESOURCE_STATE_COPY_DEST, false});
    LOGI("per-eye TAA: history stash for eye %d (%llux%u format %d)", eye, (unsigned long long)want.Width, want.Height, (int)want.Format);
    return &g_stash[eye].back();
}

// Copies `src` -> `dst` inside a game list; both end in the states given.
static void GameListCopy(ID3D12GraphicsCommandList* l, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst,
                         D3D12_RESOURCE_STATES dstState, D3D12_RESOURCE_STATES srcAfter, D3D12_RESOURCE_STATES dstAfter) {
    t_ours = true;
    D3D12_RESOURCE_BARRIER pre[2];
    UINT n = 0;
    if (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) pre[n++] = render::Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (dstState != D3D12_RESOURCE_STATE_COPY_DEST) pre[n++] = render::Transition(dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (n) l->ResourceBarrier(n, pre);
    l->CopyResource(dst, src);
    D3D12_RESOURCE_BARRIER post[2];
    n = 0;
    if (srcAfter != D3D12_RESOURCE_STATE_COPY_SOURCE) post[n++] = render::Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcAfter);
    if (dstAfter != D3D12_RESOURCE_STATE_COPY_DEST) post[n++] = render::Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, dstAfter);
    if (n) l->ResourceBarrier(n, post);
    t_ours = false;
}

static bool IsDirect(ID3D12GraphicsCommandList* l) { return l->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }

static void TaaBegin(ID3D12GraphicsCommandList* list, int eye) {
    std::lock_guard<std::mutex> l(g_taaLock);
    ID3D12Resource* hist = g_lastTaaOutput; // last frame's output = what this pass reads as history
    if (!hist) return;
    Stash* s = StashFor(eye, hist, false);
    if (!s || !s->valid) return;
    bool known = false;
    D3D12_RESOURCE_STATES st = KnownState(hist, known);
    if (!known) return;
    // The history becomes this eye's own previous output; both resources end where they started.
    GameListCopy(list, s->r, s->state, hist, st, s->state, st);
}

static void TaaEnd(ID3D12GraphicsCommandList* list, int eye, ID3D12Resource* output) {
    std::lock_guard<std::mutex> l(g_taaLock);
    if (!output) return;
    bool known = false;
    D3D12_RESOURCE_STATES st = KnownState(output, known);
    if (!known) return;
    Stash* s = StashFor(eye, output, true);
    if (!s) return;
    GameListCopy(list, output, st, s->r, s->state, st, s->state);
    s->valid = true;
    if (g_lastTaaOutput != output) {
        if (!g_lastTaaOutput) LOGI("per-eye TAA: TAA output %p found; keeping one history per eye", output);
        g_lastTaaOutput = output;
    }
}

void OnBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* b) {
    if (t_ours || !b) return;
    if (g_censusActive.load()) CensusBarrier(list, count, b);
    FrameInfo f = Frame();
    if (config::Get().uiDetect == UiDetect::Texture) {
        for (UINT i = 0; i < count; ++i) {
            const D3D12_RESOURCE_BARRIER& x = b[i];
            if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !x.Transition.pResource) continue;
            if (x.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && x.Transition.StateAfter == D3D12_RESOURCE_STATE_COPY_SOURCE &&
                x.Flags != D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)
                LearnUiTarget(x.Transition.pResource, f);
            int index = UiTextureIndex(x.Transition.pResource);
            if (index < 0 || x.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) continue;
            if (x.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) Trace('r', index);
            else if (x.Transition.StateAfter == D3D12_RESOURCE_STATE_COPY_SOURCE) Trace('x', index);
            AcquireSRWLockExclusive(&g_listLock);
            ListState& s = StateOf(list);
            bool whole = x.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || x.Transition.Subresource == 0;
            s.uiToRt = whole && x.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET ? x.Transition.pResource : nullptr;
            ReleaseSRWLockExclusive(&g_listLock);
        }
    }
    // Remember backbuffer transitions into / out of RENDER_TARGET per list, to learn the backbuffer's
    // render-target views when they were created before the mod loaded (no version.dll proxy).
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& x = b[i];
        if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !x.Transition.pResource) continue;
        if (!IsFinalImage(x.Transition.pResource, f)) continue;
        AcquireSRWLockExclusive(&g_listLock);
        ListState& s = StateOf(list);
        if (x.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET) s.bbToRt = x.Transition.pResource;
        else if (s.bbToRt == x.Transition.pResource) s.bbToRt = nullptr;
        ReleaseSRWLockExclusive(&g_listLock);
    }
    if (!TaaWanted(f)) return;
    AcquireSRWLockExclusive(&g_stateLock);
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& x = b[i];
        if (x.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !x.Transition.pResource) continue;
        if (x.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) continue; // the state changes at END_ONLY
        if (x.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            g_states.erase(x.Transition.pResource); // per-subresource states: no longer one known state
            continue;
        }
        g_states[x.Transition.pResource] = x.Transition.StateAfter;
    }
    ReleaseSRWLockExclusive(&g_stateLock);

    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    if (s.taaDepth >= 0) {
        for (UINT i = 0; i < count; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b[i].Transition.pResource) continue;
            if (b[i].Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) continue;
            D3D12_RESOURCE_STATES after = b[i].Transition.StateAfter;
            if (!(after & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET))) continue;
            D3D12_RESOURCE_DESC d = b[i].Transition.pResource->GetDesc();
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == f.width && d.Height == f.height && d.MipLevels == 1 &&
                d.DepthOrArraySize == 1)
                s.taaOutput = b[i].Transition.pResource; // the last frame-sized target written in the pass
        }
    }
    ReleaseSRWLockExclusive(&g_listLock);
}

// ---------------------------------------------------------------- markers

void OnBeginEvent(ID3D12GraphicsCommandList* list, const std::string& name) {
    const Settings& c = config::Get();
    FrameInfo f = Frame();
    bool isUi = MarkerMatches(name, c.uiMarkers);
    bool isTaa = TaaWanted(f) && MarkerMatches(name, c.taaMarkers) && IsDirect(list);
    int eye = isTaa ? rig::UpcomingEye() : -1;
    bool startTaa = false;
    uint64_t frame = g_frameIndex.load();

    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    s.scopes.push_back(name);
    int depth = (int)s.scopes.size() - 1;
    if (isUi && s.uiDepth < 0) s.uiDepth = depth;
    // Only the first TAA pass of a frame is handled (others may write differently shaped targets).
    if (isTaa && eye >= 0 && s.taaDepth < 0 && g_taaHandledFrame.exchange(frame) != frame) {
        s.taaDepth = depth;
        s.taaEye = eye;
        s.taaOutput = nullptr;
        startTaa = true;
    }
    if (s.uiDepth == depth && s.vrsBound) ApplyVrs(list, s, nullptr, f); // UI at full rate
    ReleaseSRWLockExclusive(&g_listLock);

    if (startTaa) TaaBegin(list, eye);
}

void OnEndEvent(ID3D12GraphicsCommandList* list) {
    ID3D12Resource* taaOut = nullptr;
    int eye = -1;
    bool endTaa = false;
    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    if (!s.scopes.empty()) {
        int depth = (int)s.scopes.size() - 1;
        s.scopes.pop_back();
        if (s.uiDepth == depth) s.uiDepth = -1;
        if (s.taaDepth == depth) {
            s.taaDepth = -1;
            endTaa = true;
            taaOut = s.taaOutput;
            eye = s.taaEye;
        }
    }
    ReleaseSRWLockExclusive(&g_listLock);
    if (endTaa && eye >= 0) TaaEnd(list, eye, taaOut);
}

// ---------------------------------------------------------------- frame

void OnPresent(const FrameInfo& info) {
    AcquireSRWLockExclusive(&g_frameLock);
    g_frame = info;
    ReleaseSRWLockExclusive(&g_frameLock);
    g_frameIndex.fetch_add(1);
    // Every 600 frames: how often a frame binds the backbuffer as a render target (tunes UISkipBackbufferBinds).
    g_bbBindsTotal += g_bbBindsThisFrame.exchange(0);
    if (info.immersive && ++g_bbFrames >= 600) {
        LOGI("in-world HUD: %.2f render-target bindings of the final image per frame (UI = all after the first %d)",
             g_bbBindsTotal / (double)g_bbFrames, config::Get().uiSkipBackbufferBinds);
        g_bbFrames = g_bbBindsTotal = 0;
    }
    if (config::Get().foveation > 0 && info.immersive) BuildShadingRateImage(info);
    {
        static unsigned frames = 0, sized = 0, bound = 0;
        static ULONGLONG last = 0;
        sized += g_vrsFrameSized.exchange(0);
        bound += g_vrsBoundBinds.exchange(0);
        ++frames;
        ULONGLONG now = GetTickCount64();
        if (!last) last = now;
        if (now - last >= 60000 && info.immersive) {
            LOGI("foveation (mode %d): %.1f of %.1f frame-sized render-target bindings per frame got the rate image",
                 config::Get().foveation, bound / (double)frames, sized / (double)frames);
            frames = sized = bound = 0;
            last = now;
        }
    }
    EnsureUiTexture(info);
    CensusAtPresent();
    if (g_uiTargets[0].load() && g_uiTraceFrames.load() < 24) {
        std::lock_guard<std::mutex> l(g_uiTraceLock);
        LOGI("in-world HUD trace, frame %d: %s", g_uiTraceFrames.fetch_add(1), g_uiTrace.empty() ? "(nothing)" : g_uiTrace.c_str());
        g_uiTrace.clear();
    }

    // Whenever per-eye TAA (re)starts, everything learned before is stale: states, histories.
    bool wanted = TaaWanted(info);
    if (wanted && !g_taaWasWanted) {
        AcquireSRWLockExclusive(&g_stateLock);
        g_states.clear();
        ReleaseSRWLockExclusive(&g_stateLock);
        std::lock_guard<std::mutex> l(g_taaLock);
        for (auto& v : g_stash)
            for (Stash& s : v) s.valid = false;
        g_lastTaaOutput = nullptr;
    }
    g_taaWasWanted = wanted;
}

} // namespace jvr::passes
