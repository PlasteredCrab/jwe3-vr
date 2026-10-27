#include "passes.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "rig.h"

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
static ID3D12Resource* g_ui = nullptr;
static ID3D12DescriptorHeap* g_uiHeap = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE g_uiRtv{};
static UINT g_uiW = 0, g_uiH = 0;
static DXGI_FORMAT g_uiFormat = DXGI_FORMAT_UNKNOWN;
static std::atomic<DXGI_FORMAT> g_uiWantFormat{DXGI_FORMAT_UNKNOWN}; // the backbuffer RTV's view format
static std::atomic<bool> g_uiUsedThisFrame{false};
static std::atomic<uint64_t> g_uiLastFrame{0};
static UINT g_rtvIncrement = 0;

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

static void ApplyVrs(ID3D12GraphicsCommandList* list, ListState& s, const RtvInfo* rt0, const FrameInfo& f) {
    const Settings& c = config::Get();
    bool want = c.foveation > 0 && g_vrsSupported && g_sri && f.immersive && rt0 && s.uiDepth < 0 && rt0->width == f.width &&
                rt0->height == f.height && (c.foveation == 2 || IsHdr(rt0->format));
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

static void EnsureUiTexture(const FrameInfo& f) {
    if (!config::Get().worldHud || !f.width) return;
    DXGI_FORMAT want = g_uiWantFormat.load();
    if (want == DXGI_FORMAT_UNKNOWN) want = f.format;
    if (g_ui && g_uiW == f.width && g_uiH == f.height && g_uiFormat == want) return;
    ID3D12Device* dev = render::Device();
    if (!dev) return;
    if (!g_uiHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_uiHeap)))) return;
        g_uiRtv = g_uiHeap->GetCPUDescriptorHandleForHeapStart();
        g_rtvIncrement = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = f.width;
    rd.Height = f.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = want; // same format as the view the UI pipeline draws through
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{want, {0, 0, 0, 0}};
    ID3D12Resource* tex = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&tex)))) {
        LOGE("in-world HUD: UI texture creation failed (format %d)", (int)want);
        return;
    }
    // The old texture may still be referenced by lists in flight; it is kept alive (resizes are rare).
    dev->CreateRenderTargetView(tex, nullptr, g_uiRtv);
    g_ui = tex;
    g_uiW = f.width;
    g_uiH = f.height;
    g_uiFormat = want;
    ClearHud();
}

void ClearHud() {
    if (!g_ui) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_uiRtv;
    render::Submit([&](ID3D12GraphicsCommandList* l) {
        const float clear[4] = {0, 0, 0, 0};
        l->ClearRenderTargetView(rtv, clear, 0, nullptr);
    });
}

static bool IsBackbuffer(const RtvInfo& rt, const FrameInfo& f) {
    for (ID3D12Resource* b : f.backbuffers)
        if (b && b == rt.resource) return true;
    return false;
}

bool OnSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                        D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    const Settings& c = config::Get();
    if (!c.enabled || t_ours) return false;
    FrameInfo f = Frame();
    if (!f.immersive || (c.foveation <= 0 && !c.worldHud)) return false; // nothing to do: skip the locks
    RtvInfo rt0{};
    bool have0 = count > 0 && rtvs && LookupRtv(rtvs[0], rt0);

    AcquireSRWLockExclusive(&g_listLock);
    ListState& s = StateOf(list);
    bool inUi = s.uiDepth >= 0;
    ApplyVrs(list, s, have0 ? &rt0 : nullptr, f);
    ReleaseSRWLockExclusive(&g_listLock);

    if (!inUi || !c.worldHud || !f.worldHudAllowed || !g_ui || count == 0 || count > 8 || !rtvs) return false;
    bool replaced = false;
    for (UINT i = 0; i < count; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = single ? D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + (SIZE_T)i * g_rtvIncrement} : rtvs[i];
        RtvInfo rt{};
        out[i] = h;
        if (!LookupRtv(h, rt) || !IsBackbuffer(rt, f)) continue;
        if (rt.format != g_uiFormat) {
            // Draw through the same view format as the game's pipeline: recreate at the next Present.
            g_uiWantFormat = rt.format;
            return false;
        }
        out[i] = g_uiRtv;
        replaced = true;
    }
    if (!replaced) return false;
    g_uiUsedThisFrame = true;
    return true;
}

ID3D12Resource* TakeHudTexture() {
    if (!g_uiUsedThisFrame.exchange(false)) return nullptr;
    g_uiLastFrame = g_frameIndex.load();
    return g_ui;
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

static bool TaaWanted(const FrameInfo& f) {
    const Settings& c = config::Get();
    return c.enabled && c.perEyeTaa && f.aer && f.immersive;
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
    FrameInfo f = Frame();
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
    if (config::Get().foveation > 0 && info.immersive) BuildShadingRateImage(info);
    EnsureUiTexture(info);

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
