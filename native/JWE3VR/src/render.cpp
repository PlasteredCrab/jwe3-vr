#include "render.h"
#include "config.h"
#include "hooks.h"
#include "input.h"
#include "log.h"
#include "passes.h"
#include "rig.h"
#include "viewcb.h"
#include "xr.h"

#include <dxgi1_6.h>

namespace jvr::render {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using RootCbvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using BeginEventFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const void*, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
using SetRtFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE*);
using EndEventFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
static SetRtFn o_SetRenderTargets;
static EndEventFn o_EndEvent;
static BarrierFn o_Barrier;
static CreateRtvFn o_CreateRtv;
using CreateSwapForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static CreateSwapForHwndFn o_CreateSwapForHwnd;
static CreateSwapFn o_CreateSwap;
static std::atomic<ID3D12CommandQueue*> g_swapQueue{nullptr}; // the queue the game created its swapchain on
static IDXGISwapChain3* g_swap = nullptr;
static ULONGLONG g_reinitAt = 0;

static PresentFn o_Present;
static Present1Fn o_Present1;
static ResizeFn o_Resize;
static ExecuteFn o_Execute;
static ResetFn o_Reset;
static RootCbvFn o_GfxCbv, o_CompCbv;
static BeginEventFn o_BeginEvent;
static MapFn o_Map;

static ID3D12Device* g_device = nullptr;
static std::atomic<ID3D12CommandQueue*> g_lastDirect{nullptr};
static ID3D12CommandQueue* g_queue = nullptr;
static bool g_xrTried = false;
static thread_local bool t_inPresent = false;
static std::atomic<bool> g_tablet{false};
static ULONGLONG g_lastConfigCheck = 0;
static xr::EyeView g_views[2];

// Pointer dot (laser / rifle sight): a small premade disc copied into its swapchain while visible.
static std::atomic<bool> g_pointerVisible{false};
static std::mutex g_pointerLock;
static vm::Vec3 g_pointerStage;
static ID3D12Resource* g_dotTex = nullptr;
static DXGI_FORMAT g_dotFormat = DXGI_FORMAT_UNKNOWN;

void SetPointer(bool visible, vm::Vec3 stagePos) {
    {
        std::lock_guard<std::mutex> l(g_pointerLock);
        g_pointerStage = stagePos;
    }
    g_pointerVisible = visible;
}

// Our own copy commands, executed on the game's present queue.
struct CopySlot {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fenceValue = 0;
};
static CopySlot g_slots[8];
static int g_slot = 0;
static ID3D12GraphicsCommandList* g_copyList = nullptr;
static ID3D12Fence* g_fence = nullptr;
static UINT64 g_fenceValue = 0;
static HANDLE g_fenceEvent = nullptr;
static std::mutex g_copyLock;

ID3D12CommandQueue* GameQueue() { return g_queue; }
void SetTabletVisible(bool v) { g_tablet = v; }

static bool InitCopy() {
    if (g_copyList) return true;
    for (CopySlot& s : g_slots)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_slots[0].alloc, nullptr, IID_PPV_ARGS(&g_copyList))))
        return false;
    g_copyList->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

bool Submit(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
    if (!g_queue) return false;
    std::lock_guard<std::mutex> l(g_copyLock);
    if (!InitCopy()) return false;
    CopySlot& s = g_slots[g_slot];
    if (g_fence->GetCompletedValue() < s.fenceValue) {
        g_fence->SetEventOnCompletion(s.fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
        // Never reset an allocator the GPU may still be reading: skip this copy instead (a stalled or
        // shutting-down queue). Resetting it anyway faulted the GPU on JWE3 (NVIDIA event 153).
        if (g_fence->GetCompletedValue() < s.fenceValue) {
            JVR_LOG_ONCE("copy queue stalled for 2 s; skipping copies until it catches up");
            return false;
        }
    }
    g_slot = (g_slot + 1) % 8;
    s.alloc->Reset();
    g_copyList->Reset(s.alloc, nullptr);
    record(g_copyList);
    g_copyList->Close();
    ID3D12CommandList* lists[] = {g_copyList};
    o_Execute(g_queue, 1, lists);
    s.fenceValue = ++g_fenceValue;
    g_queue->Signal(g_fence, s.fenceValue);
    return true;
}

ID3D12Device* Device() { return g_device; }

bool CopyToImage(ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst) {
    if (!src || !dst) return false;
    return Submit([&](ID3D12GraphicsCommandList* list) { RecordCopy(list, src, srcState, dst); });
}

void RecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst) {
    D3D12_RESOURCE_DESC sd = src->GetDesc(), dd = dst->GetDesc();
    D3D12_RESOURCE_BARRIER pre[2] = {Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                     Transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)};
    bool srcNeedsBarrier = srcState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(srcNeedsBarrier ? 2 : 1, srcNeedsBarrier ? pre : pre + 1);

    D3D12_TEXTURE_COPY_LOCATION from{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION to{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_BOX box{0, 0, 0, (UINT)std::min(sd.Width, dd.Width), std::min(sd.Height, dd.Height), 1};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);

    D3D12_RESOURCE_BARRIER post[2] = {Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState),
                                      Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(srcNeedsBarrier ? 2 : 1, srcNeedsBarrier ? post : post + 1);
}

bool WaitCopies() {
    std::lock_guard<std::mutex> l(g_copyLock);
    if (!g_fence || g_fence->GetCompletedValue() >= g_fenceValue) return true;
    g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 2000);
    return g_fence->GetCompletedValue() >= g_fenceValue;
}

ID3D12Resource* NextBackbuffer() {
    if (!g_swap) return nullptr;
    ID3D12Resource* bb = nullptr;
    g_swap->GetBuffer(g_swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    return bb;
}

// Set while InstallHooks creates its own throwaway swapchain, so that one is not taken for the game's.
static thread_local bool t_creatingDummy = false;

static void RememberSwapQueue(IUnknown* device) {
    if (t_creatingDummy) return;
    ID3D12CommandQueue* q = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&q)))) {
        g_swapQueue = q; // keep the reference: the queue lives as long as the game's swapchain
        LOGI("game swapchain created on queue %p", q);
    }
}

static HRESULT STDMETHODCALLTYPE Hk_CreateSwapForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** sc) {
    HRESULT hr = o_CreateSwapForHwnd(f, dev, hwnd, d, fs, o, sc);
    if (SUCCEEDED(hr)) RememberSwapQueue(dev);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateSwap(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) {
    HRESULT hr = o_CreateSwap(f, dev, d, sc);
    if (SUCCEEDED(hr)) RememberSwapQueue(dev);
    return hr;
}

static void CopyBackbufferTo(ID3D12Resource* bb, xr::SwapId id) {
    ID3D12Resource* img = xr::AcquireImage(id);
    if (!img) return;
    CopyToImage(bb, D3D12_RESOURCE_STATE_PRESENT, img);
    xr::ReleaseImage(id);
}

// White disc with a dark rim, premultiplied alpha, in the dot swapchain's format (kept in COPY_SOURCE).
static bool EnsureDotTexture() {
    DXGI_FORMAT fmt = xr::DotFormat();
    if (g_dotTex && g_dotFormat == fmt) return true;
    g_dotTex = nullptr; // swapchain format changed (OpenXR re-init): rebuild; the old one may be in flight
    bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!g_device || (!bgra && !rgba)) return false;
    const UINT n = xr::kDotSize, pitch = 256; // n * 4 bytes = 256
    std::vector<uint8_t> px((size_t)pitch * n, 0);
    for (UINT y = 0; y < n; ++y)
        for (UINT x = 0; x < n; ++x) {
            float dx = (x + 0.5f) / n * 2.f - 1.f, dy = (y + 0.5f) / n * 2.f - 1.f, r = std::sqrt(dx * dx + dy * dy);
            float a = std::clamp((1.f - r) * n * 0.5f, 0.f, 1.f);         // anti-aliased disc
            float c = r < 0.55f ? 1.f : (r < 0.75f ? 0.1f : 0.f);         // white core, dark rim
            uint8_t* p = &px[(size_t)y * pitch + x * 4];
            p[0] = p[1] = p[2] = (uint8_t)(c * a * 230.f); // premultiplied: colour never exceeds alpha
            p[3] = (uint8_t)(a * 230.f);
        }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = n;
    td.Height = n;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = px.size();
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* tex = nullptr;
    ID3D12Resource* upload = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))) ||
        FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)))) {
        SafeRelease(tex);
        return false;
    }
    void* m = nullptr;
    upload->Map(0, nullptr, &m);
    memcpy(m, px.data(), px.size());
    upload->Unmap(0, nullptr);
    Submit([&](ID3D12GraphicsCommandList* l) {
        D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint.Footprint = {fmt, n, n, 1, pitch};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->ResourceBarrier(1, &b);
    });
    if (WaitCopies()) upload->Release(); // else leak it: the GPU may still read it
    g_dotTex = tex;
    g_dotFormat = fmt;
    return true;
}

// Debug aid for remote tests: a file JWE3VR_shot.req next to the DLL asks for the frame being presented,
// written as JWE3VR_shot.bmp (24-bit) in the same folder. Runs on the present thread; stalls one frame.
static void WriteBmp(const std::wstring& path, UINT w, UINT h, const std::vector<uint8_t>& bgr) {
    UINT row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = (LONG)h; // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, &fh, sizeof(fh), &n, nullptr);
    WriteFile(f, &ih, sizeof(ih), &n, nullptr);
    std::vector<uint8_t> line(row, 0);
    for (UINT y = 0; y < h; ++y) {
        memcpy(line.data(), &bgr[(size_t)(h - 1 - y) * w * 3], (size_t)w * 3);
        WriteFile(f, line.data(), row, &n, nullptr);
    }
    CloseHandle(f);
}

static void DebugScreenshot() {
    ID3D12Resource* bb = NextBackbuffer();
    if (!bb) return;
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&rb)))) {
        bb->Release();
        LOGW("debug: screenshot readback buffer failed");
        return;
    }
    bool submitted = Submit([&](ID3D12GraphicsCommandList* l) {
        D3D12_RESOURCE_BARRIER b = Transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        l->ResourceBarrier(1, &b);
    });
    if (!submitted || !WaitCopies()) {
        // Not copied, or the GPU has not finished: keep both alive (leak) rather than free memory in use.
        LOGW("debug: screenshot skipped (copy queue busy)");
        return;
    }
    uint8_t* p = nullptr;
    D3D12_RANGE range{0, (SIZE_T)total};
    UINT w = (UINT)d.Width, h = d.Height;
    bool ok = false;
    if (SUCCEEDED(rb->Map(0, &range, (void**)&p)) && p) {
        std::vector<uint8_t> bgr((size_t)w * h * 3);
        ok = true;
        for (UINT y = 0; y < h && ok; ++y) {
            const uint8_t* row = p + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
            uint8_t* out = &bgr[(size_t)y * w * 3];
            for (UINT x = 0; x < w; ++x) {
                uint32_t v = ((const uint32_t*)row)[x];
                uint8_t r, g, b;
                switch (d.Format) {
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    r = (uint8_t)((v & 0x3FF) >> 2), g = (uint8_t)(((v >> 10) & 0x3FF) >> 2), b = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
                    break;
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    r = (uint8_t)v, g = (uint8_t)(v >> 8), b = (uint8_t)(v >> 16);
                    break;
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    b = (uint8_t)v, g = (uint8_t)(v >> 8), r = (uint8_t)(v >> 16);
                    break;
                default:
                    ok = false;
                    r = g = b = 0;
                    break;
                }
                out[x * 3 + 0] = b, out[x * 3 + 1] = g, out[x * 3 + 2] = r;
            }
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        if (ok) WriteBmp(ModuleDir() + L"JWE3VR_shot.bmp", w, h, bgr);
    }
    rb->Release();
    bb->Release();
    if (ok) LOGI("debug: screenshot %ux%u saved to JWE3VR_shot.bmp; %s", w, h, input::DebugSummary().c_str());
    else LOGW("debug: screenshot failed (backbuffer format %d)", (int)d.Format);
}

static void OnPresent(IDXGISwapChain* sc) {
    if (!g_device) {
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) return;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) input::SetGameWindow(d.OutputWindow);
    }
    if (!g_queue) {
        g_queue = g_swapQueue.load();
        if (!g_queue) {
            // The swapchain predates our hooks: fall back to the last direct queue the game submitted to.
            g_queue = g_lastDirect.load();
            if (!g_queue) return;
            LOGW("game swapchain queue not seen; using the last direct queue %p", g_queue);
        }
        LOGI("game present queue %p, device %p", g_queue, g_device);
    }
    if (!g_swap) sc->QueryInterface(IID_PPV_ARGS(&g_swap));
    input::DebugTick();
    ULONGLONG now = GetTickCount64();
    if (now - g_lastConfigCheck > 1000) {
        g_lastConfigCheck = now;
        if (config::ReloadIfChanged()) LOGI("settings reloaded");
        std::wstring req = ModuleDir() + L"JWE3VR_shot.req";
        if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(req.c_str());
            DebugScreenshot();
        }
    }
    if (xr::NeedsReinit()) {
        LOGW("OpenXR session ended; retrying in 5 s");
        xr::Shutdown();
        g_reinitAt = now + 5000;
        g_xrTried = false;
    }
    if (!g_xrTried && now >= g_reinitAt) {
        g_xrTried = true;
        if (!xr::Init(g_device, g_queue)) LOGE("OpenXR init failed; is a headset runtime (SteamVR / Oculus / VDXR) running?");
    }
    if (!xr::Running()) {
        xr::BeginFrame(); // polls session events until the runtime is ready
        return;
    }

    ID3D12Resource* bb = NextBackbuffer();
    if (!bb) return;

    D3D12_RESOURCE_DESC d = bb->GetDesc();
    bool haveSwaps = xr::EnsureSwapchains((UINT)d.Width, d.Height, d.Format);
    float aspect = (float)d.Width / (float)d.Height;

    const Settings& cfg = config::Get();
    rig::RenderRequest req;
    bool haveReq = rig::TakeRequestForPresent(req);
    rig::AnchorKind kind = rig::CurrentKind();
    bool immersive = cfg.enabled && haveSwaps && haveReq && kind != rig::AnchorKind::Off && kind != rig::AnchorKind::Screen;

    // Per-pass work for the frames the game records next (foveation, UI capture, per-eye TAA).
    passes::FrameInfo fi;
    fi.immersive = immersive;
    fi.aer = cfg.stereo == StereoMode::AER;
    fi.width = (UINT)d.Width;
    fi.height = d.Height;
    fi.format = d.Format;
    fi.fovY = haveReq ? req.fovY : 1.f;
    fi.aspect = aspect;
    fi.worldHudAllowed = !g_tablet.load(); // the tablet shows the frame, so it needs the UI in it
    DXGI_SWAP_CHAIN_DESC scd{};
    if (g_swap && SUCCEEDED(g_swap->GetDesc(&scd)))
        for (UINT i = 0; i < scd.BufferCount && i < 4; ++i) {
            ID3D12Resource* b = nullptr;
            if (SUCCEEDED(g_swap->GetBuffer(i, IID_PPV_ARGS(&b)))) {
                fi.backbuffers[i] = b; // the swapchain keeps them alive
                b->Release();
            }
        }
    passes::OnPresent(fi);

    if (immersive) {
        vm::Fov fov = vm::SymmetricFov(req.fovY, aspect);
        if (cfg.stereo == StereoMode::Sync && viewcb::TakeLeftCaptured()) {
            // Left was captured between the two replays; the backbuffer now holds the right eye.
            CopyBackbufferTo(bb, xr::SwapId::Right);
            g_views[0] = {req.eyeStage[0], fov, true};
            g_views[1] = {req.eyeStage[1], fov, true};
        } else if (req.eye >= 0) {
            CopyBackbufferTo(bb, req.eye == 0 ? xr::SwapId::Left : xr::SwapId::Right);
            g_views[req.eye] = {req.stagePose, fov, true};
        } else {
            CopyBackbufferTo(bb, xr::SwapId::Left);
            CopyBackbufferTo(bb, xr::SwapId::Right);
            g_views[0] = g_views[1] = {req.stagePose, fov, true};
        }
    }
    bool screen = haveSwaps && (!immersive || g_tablet.load());
    if (screen) CopyBackbufferTo(bb, xr::SwapId::Screen);

    xr::Layers layers;
    layers.projection = immersive;
    layers.screen = screen;
    // In-world HUD: the UI the game drew this frame went into its own texture instead of the eyes.
    if (ID3D12Resource* hudTex = passes::TakeHudTexture(); hudTex && immersive) {
        if (ID3D12Resource* img = xr::AcquireImage(xr::SwapId::Hud)) {
            CopyToImage(hudTex, D3D12_RESOURCE_STATE_RENDER_TARGET, img);
            xr::ReleaseImage(xr::SwapId::Hud);
            layers.hud = true;
            JVR_LOG_ONCE("in-world HUD active");
        }
        passes::ClearHud(); // ready for the next frame's UI (ordered after the copy on the same queue)
    }
    if (haveSwaps && g_pointerVisible.load() && EnsureDotTexture()) {
        vm::Vec3 dotPos;
        {
            std::lock_guard<std::mutex> l(g_pointerLock);
            dotPos = g_pointerStage;
        }
        if (ID3D12Resource* img = xr::AcquireImage(xr::SwapId::Dot)) {
            CopyToImage(g_dotTex, D3D12_RESOURCE_STATE_COPY_SOURCE, img);
            xr::ReleaseImage(xr::SwapId::Dot);
            layers.dot = true;
            layers.dotPos = dotPos;
        }
    }

    xr::EndFrame(g_views, layers);
    input::OnFrame(screen, layers.hud);
    viewcb::OnPresent();
    xr::BeginFrame();
    bb->Release();
}

// The headset paces frames through xrWaitFrame; a v-synced monitor on top would halve the rate.
static UINT SyncInterval(UINT sync) { return (xr::Running() && config::Get().forceVsyncOff) ? 0 : sync; }

static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        t_inPresent = true;
        OnPresent(sc);
        HRESULT hr = o_Present(sc, SyncInterval(sync), flags);
        t_inPresent = false;
        return hr;
    }
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        t_inPresent = true;
        OnPresent(sc);
        HRESULT hr = o_Present1(sc, SyncInterval(sync), flags, p);
        t_inPresent = false;
        return hr;
    }
    return o_Present1(sc, sync, flags, p);
}

static HRESULT STDMETHODCALLTYPE Hk_Resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    LOGI("ResizeBuffers %ux%u", w, h);
    return o_Resize(sc, n, w, h, f, flags);
}

static void STDMETHODCALLTYPE Hk_Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
    if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_lastDirect = q;
    if (q == g_queue && viewcb::OnExecute(q, n, lists, o_Execute)) return;
    o_Execute(q, n, lists);
}

static HRESULT STDMETHODCALLTYPE Hk_Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    if (l != g_copyList) {
        viewcb::OnCommandListReset(l);
        passes::OnReset(l);
    }
    return o_Reset(l, a, p);
}

static void STDMETHODCALLTYPE Hk_SetRenderTargets(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                                  BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE swapped[8];
    if (l != g_copyList && passes::OnSetRenderTargets(l, n, rtvs, single, swapped)) {
        o_SetRenderTargets(l, n, swapped, FALSE, dsv);
        return;
    }
    o_SetRenderTargets(l, n, rtvs, single, dsv);
}

static void STDMETHODCALLTYPE Hk_EndEvent(ID3D12GraphicsCommandList* l) {
    if (l != g_copyList) passes::OnEndEvent(l);
    o_EndEvent(l);
}

static void STDMETHODCALLTYPE Hk_Barrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (l != g_copyList) passes::OnBarrier(l, n, b);
    o_Barrier(l, n, b);
}

static void STDMETHODCALLTYPE Hk_CreateRtv(ID3D12Device* dev, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d,
                                           D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRtv(dev, r, d, h);
    passes::OnCreateRtv(r, d, h);
}

static void STDMETHODCALLTYPE Hk_GfxCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    viewcb::OnRootCbv(l, va);
    o_GfxCbv(l, i, va);
}

static void STDMETHODCALLTYPE Hk_CompCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    viewcb::OnRootCbv(l, va);
    o_CompCbv(l, i, va);
}

static void STDMETHODCALLTYPE Hk_BeginEvent(ID3D12GraphicsCommandList* l, UINT meta, const void* data, UINT size) {
    if (l != g_copyList) {
        std::string name = viewcb::DecodeMarker(meta, data, size);
        viewcb::OnMarker(l, name);
        passes::OnBeginEvent(l, name);
    }
    o_BeginEvent(l, meta, data, size);
}

static HRESULT STDMETHODCALLTYPE Hk_Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** out) {
    HRESULT hr = o_Map(r, sub, range, out);
    if (SUCCEEDED(hr) && out && *out && sub == 0) viewcb::OnMap(r, *out);
    return hr;
}

static HMODULE SystemModule(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring path = std::wstring(dir) + L"\\" + name;
    return LoadLibraryW(path.c_str());
}

bool InstallHooks() {
    if (!hooks::Init()) return false;
    HMODULE d3d12 = SystemModule(L"d3d12.dll");
    HMODULE dxgi = SystemModule(L"dxgi.dll");
    if (!d3d12 || !dxgi) {
        LOGE("could not load system d3d12/dxgi");
        return false;
    }
    auto createDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    auto createFactory = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory1");

    ID3D12Device* dev = nullptr;
    if (!createDevice || FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        LOGE("dummy D3D12 device failed");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* buffer = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDXGISwapChain1* sc = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
    if (ok) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&buffer)));
    }
    if (ok && createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory)))) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"JWE3VR_dummy";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Factory hooks first, so the game's own swapchain creation (if still to come) reports its queue.
        hooks::Create("IDXGIFactory::CreateSwapChain", hooks::VtableEntry(factory, 10), (void*)Hk_CreateSwap, (void**)&o_CreateSwap);
        hooks::Create("IDXGIFactory2::CreateSwapChainForHwnd", hooks::VtableEntry(factory, 15), (void*)Hk_CreateSwapForHwnd,
                      (void**)&o_CreateSwapForHwnd);
        t_creatingDummy = true;
        ok = SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc));
        t_creatingDummy = false;
        if (ok) {
            hooks::Create("IDXGISwapChain::Present", hooks::VtableEntry(sc, 8), (void*)Hk_Present, (void**)&o_Present);
            hooks::Create("IDXGISwapChain::ResizeBuffers", hooks::VtableEntry(sc, 13), (void*)Hk_Resize, (void**)&o_Resize);
            hooks::Create("IDXGISwapChain1::Present1", hooks::VtableEntry(sc, 22), (void*)Hk_Present1, (void**)&o_Present1);
        }
        SafeRelease(sc);
        DestroyWindow(hwnd);
    } else {
        ok = false;
    }
    if (ok) {
        hooks::Create("ID3D12CommandQueue::ExecuteCommandLists", hooks::VtableEntry(queue, 10), (void*)Hk_Execute, (void**)&o_Execute);
        hooks::Create("ID3D12GraphicsCommandList::Reset", hooks::VtableEntry(list, 10), (void*)Hk_Reset, (void**)&o_Reset);
        hooks::Create("ID3D12GraphicsCommandList::SetComputeRootConstantBufferView", hooks::VtableEntry(list, 37), (void*)Hk_CompCbv,
                      (void**)&o_CompCbv);
        hooks::Create("ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView", hooks::VtableEntry(list, 38), (void*)Hk_GfxCbv,
                      (void**)&o_GfxCbv);
        hooks::Create("ID3D12GraphicsCommandList::BeginEvent", hooks::VtableEntry(list, 57), (void*)Hk_BeginEvent,
                      (void**)&o_BeginEvent);
        hooks::Create("ID3D12Resource::Map", hooks::VtableEntry(buffer, 8), (void*)Hk_Map, (void**)&o_Map);
        hooks::Create("ID3D12GraphicsCommandList::ResourceBarrier", hooks::VtableEntry(list, 26), (void*)Hk_Barrier, (void**)&o_Barrier);
        hooks::Create("ID3D12GraphicsCommandList::OMSetRenderTargets", hooks::VtableEntry(list, 46), (void*)Hk_SetRenderTargets,
                      (void**)&o_SetRenderTargets);
        hooks::Create("ID3D12GraphicsCommandList::EndEvent", hooks::VtableEntry(list, 58), (void*)Hk_EndEvent, (void**)&o_EndEvent);
        hooks::Create("ID3D12Device::CreateRenderTargetView", hooks::VtableEntry(dev, 20), (void*)Hk_CreateRtv, (void**)&o_CreateRtv);
    }
    SafeRelease(factory);
    SafeRelease(buffer);
    SafeRelease(list);
    SafeRelease(alloc);
    SafeRelease(queue);
    SafeRelease(dev);
    if (!ok) LOGE("D3D12 hook install failed");
    return ok;
}

} // namespace jvr::render
