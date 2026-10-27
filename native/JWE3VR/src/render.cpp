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
using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
static SetRtFn o_SetRenderTargets;
static ClearRtvFn o_ClearRtv;
static EndEventFn o_EndEvent;
static BarrierFn o_Barrier;
static CreateRtvFn o_CreateRtv;
// Diagnostics: other ways a frame can write the backbuffer (newer D3D12 paths the redirect does not cover).
using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                                   const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using Barrier7Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);
using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                              const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
static BeginRenderPassFn o_BeginRenderPass;
static Barrier7Fn o_Barrier7;
static CopyRegionFn o_CopyRegion;
static CopyResourceFn o_CopyResource;
static std::atomic<ID3D12Resource*> g_bbCache[4]{};
static std::atomic<unsigned> g_statRenderPass{0}, g_statBarrier7{0}, g_statBarrier7Bb{0}, g_statCopyToBb{0}, g_statLegacyBbBarrier{0},
    g_statSetRt{0};

static bool IsCachedBackbuffer(ID3D12Resource* r) {
    if (!r) return false;
    for (auto& b : g_bbCache)
        if (b.load() == r) return true;
    return false;
}
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
// The direct queue this thread submitted to last: on the thread that presents, the queue of the frame's final work
// (the composite and the copy into the backbuffer), a better guess for the present queue than the last direct
// queue of any thread (JWE3 also submits direct work from other threads).
static thread_local ID3D12CommandQueue* t_lastDirect = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static bool g_xrTried = false;
static thread_local bool t_inPresent = false;
static std::atomic<bool> g_tablet{false};
static ULONGLONG g_lastConfigCheck = 0;
static std::atomic<int> g_debugCommand{0};

int TakeDebugCommand() { return g_debugCommand.exchange(0); }
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

bool Submit(const std::function<void(ID3D12GraphicsCommandList*)>& record, UINT64* fence) {
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
    if (fence) *fence = s.fenceValue;
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

bool WaitCopies(unsigned timeoutMs) {
    std::lock_guard<std::mutex> l(g_copyLock);
    if (!g_fence || g_fence->GetCompletedValue() >= g_fenceValue) return true;
    g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, timeoutMs);
    return g_fence->GetCompletedValue() >= g_fenceValue;
}

static UINT64 CopyFenceCompleted() {
    std::lock_guard<std::mutex> l(g_copyLock);
    return g_fence ? g_fence->GetCompletedValue() : 0;
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

static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) bits = sign; // zero / denormal (shown as black)
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static uint8_t Unit8(float v) { return (uint8_t)(v <= 0.f ? 0 : v >= 1.f ? 255 : v * 255.f + 0.5f); }

// HDR values for a look at the texture: Reinhard, then gamma 2.2.
static uint8_t Hdr8(float v) { return v <= 0.f ? 0 : Unit8(std::pow(v / (1.f + v), 1.f / 2.2f)); }

// 11- and 10-bit unsigned floats (R11G11B10): 6 / 5 mantissa bits, 5 exponent bits, bias 15.
static float SmallFloat(uint32_t bits, int mantissaBits) {
    uint32_t m = bits & ((1u << mantissaBits) - 1), e = (bits >> mantissaBits) & 0x1F;
    float frac = (float)m / (float)(1u << mantissaBits);
    if (e == 0) return frac * std::ldexp(1.f, -14);
    if (e == 31) return 65504.f;
    return (1.f + frac) * std::ldexp(1.f, (int)e - 15);
}

// One texel of the formats the debug BMPs understand (false: not supported).
static bool DecodeTexel(DXGI_FORMAT f, const uint8_t* px, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint32_t v;
    memcpy(&v, px, 4);
    switch (f) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        r = (uint8_t)((v & 0x3FF) >> 2), g = (uint8_t)(((v >> 10) & 0x3FF) >> 2), b = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
        return true;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        r = (uint8_t)v, g = (uint8_t)(v >> 8), b = (uint8_t)(v >> 16);
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        b = (uint8_t)v, g = (uint8_t)(v >> 8), r = (uint8_t)(v >> 16);
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        uint16_t h[3];
        memcpy(h, px, 6);
        r = Hdr8(HalfToFloat(h[0])), g = Hdr8(HalfToFloat(h[1])), b = Hdr8(HalfToFloat(h[2]));
        return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT:
        r = Hdr8(SmallFloat(v & 0x7FF, 6)), g = Hdr8(SmallFloat((v >> 11) & 0x7FF, 6)), b = Hdr8(SmallFloat(v >> 22, 5));
        return true;
    default:
        return false;
    }
}

static UINT TexelBytes(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8 : 4;
}

bool DebugFormatSupported(DXGI_FORMAT f) {
    uint8_t px[8]{}, r, g, b;
    return DecodeTexel(f, px, r, g, b);
}

bool SaveReadbackBmp(ID3D12Resource* rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, DXGI_FORMAT format, UINT w, UINT h,
                     const wchar_t* file) {
    uint8_t* p = nullptr;
    D3D12_RANGE range{0, (SIZE_T)(fp.Offset + (UINT64)fp.Footprint.RowPitch * h)};
    if (FAILED(rb->Map(0, &range, (void**)&p)) || !p) return false;
    std::vector<uint8_t> bgr((size_t)w * h * 3);
    UINT step = TexelBytes(format);
    bool ok = DebugFormatSupported(format);
    for (UINT y = 0; y < h && ok; ++y) {
        const uint8_t* row = p + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
        uint8_t* out = &bgr[(size_t)y * w * 3];
        for (UINT x = 0; x < w; ++x) {
            uint8_t r, g, b;
            DecodeTexel(format, row + (size_t)x * step, r, g, b);
            out[x * 3 + 0] = b, out[x * 3 + 1] = g, out[x * 3 + 2] = r;
        }
    }
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    if (ok) WriteBmp(ModuleDir() + file, w, h, bgr);
    return ok;
}

// Debug dumps are saved on a later frame, once the GPU has done the copy: waiting for it inside Present stalled
// whole runs (run 86: every dump's copy stayed two submissions behind until the 15 s wait gave up - the queue's
// work waits on the frame the game records after this Present, which a wait here holds up).
struct PendingDump {
    ID3D12Resource* readback;
    ID3D12Resource* source; // kept alive until the copy is done
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    DXGI_FORMAT format;
    UINT w, h;
    std::wstring file;
    UINT64 fence;
    ULONGLONG since;
};
static std::vector<PendingDump> g_pendingDumps; // render thread only
constexpr ULONGLONG kDumpGiveUpMs = 30000;

// Saves the dumps whose copies have finished (each Present).
static void PollDumps() {
    if (g_pendingDumps.empty()) return;
    UINT64 done = CopyFenceCompleted();
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_pendingDumps.size();) {
        PendingDump& p = g_pendingDumps[i];
        if (done >= p.fence) {
            bool ok = SaveReadbackBmp(p.readback, p.fp, p.format, p.w, p.h, p.file.c_str());
            if (ok) LOGI("debug: %ls %ux%u saved (%llu ms after the request); %s", p.file.c_str(), p.w, p.h, now - p.since,
                         input::DebugSummary().c_str());
            else LOGW("debug: %ls failed (format %d)", p.file.c_str(), (int)p.format);
            p.readback->Release();
            p.source->Release();
        } else if (now - p.since > kDumpGiveUpMs) {
            // Never finished: keep both alive (leak) rather than free memory the GPU may still use.
            LOGW("debug: %ls dropped (copy not finished in 30 s; copy fence %llu of %llu on queue %p)", p.file.c_str(), done,
                 p.fence, g_queue);
        } else {
            ++i;
            continue;
        }
        g_pendingDumps.erase(g_pendingDumps.begin() + i);
    }
}

// Copies one texture (in state `state`) to the CPU and writes it as <file> next to the DLL once the copy is done
// (PollDumps). inFlight is always false: the dump keeps its own reference to `res`, so callers release theirs.
static bool DebugDump(ID3D12Resource* res, D3D12_RESOURCE_STATES state, const wchar_t* file, bool& inFlight) {
    inFlight = false;
    if (!res) return false;
    D3D12_RESOURCE_DESC d = res->GetDesc();
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
        LOGW("debug: readback buffer failed");
        return false;
    }
    UINT64 fence = 0;
    bool submitted = Submit([&](ID3D12GraphicsCommandList* l) {
        D3D12_RESOURCE_BARRIER b = Transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = Transition(res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        l->ResourceBarrier(1, &b);
    }, &fence);
    if (!submitted) {
        LOGW("debug: %ls skipped (copy queue stalled)", file);
        rb->Release(); // never submitted: the GPU does not know it
        return false;
    }
    res->AddRef();
    g_pendingDumps.push_back({rb, res, fp, d.Format, (UINT)d.Width, d.Height, file, fence, GetTickCount64()});
    return true;
}

static void DebugScreenshot() {
    ID3D12Resource* bb = NextBackbuffer();
    if (!bb) return;
    bool inFlight = false;
    DebugDump(bb, D3D12_RESOURCE_STATE_PRESENT, L"JWE3VR_shot.bmp", inFlight);
    if (!inFlight) bb->Release();
}

// Every 10 s: game frames per second, the OpenXR session state and the time Present spent in xrWaitFrame.
static void LogFrameStats(ULONGLONG waitMs) {
    static ULONGLONG windowStart = 0, waitTotal = 0;
    static unsigned frames = 0;
    ULONGLONG now = GetTickCount64();
    if (!windowStart) windowStart = now;
    ++frames;
    waitTotal += waitMs;
    if (now - windowStart >= 10000) {
        LOGI("frames: %.1f fps, OpenXR state %d, xrWaitFrame %.1f ms per frame", frames * 1000.0 / (now - windowStart),
             xr::SessionState(), frames ? (double)waitTotal / frames : 0.0);
        double n = frames ? (double)frames : 1.0;
        LOGI("per frame: OMSetRenderTargets %.1f, BeginRenderPass %.1f, legacy barriers on backbuffer %.1f, enhanced barriers %.1f "
             "(on backbuffer %.1f), copies into backbuffer %.1f",
             g_statSetRt.exchange(0) / n, g_statRenderPass.exchange(0) / n, g_statLegacyBbBarrier.exchange(0) / n,
             g_statBarrier7.exchange(0) / n, g_statBarrier7Bb.exchange(0) / n, g_statCopyToBb.exchange(0) / n);
        windowStart = now;
        frames = 0;
        waitTotal = 0;
    }
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
            // The swapchain predates our hooks: fall back to the direct queue this (the presenting) thread submitted
            // to last, else the last direct queue of any thread.
            ID3D12CommandQueue* any = g_lastDirect.load();
            g_queue = t_lastDirect ? t_lastDirect : any;
            if (!g_queue) return;
            LOGW("game swapchain queue not seen; using the %s direct queue %p (last of any thread: %p)",
                 t_lastDirect ? "presenting thread's last" : "last", g_queue, any);
        }
        LOGI("game present queue %p, device %p", g_queue, g_device);
    }
    if (!g_swap) sc->QueryInterface(IID_PPV_ARGS(&g_swap));
    input::DebugTick();
    PollDumps();
    ULONGLONG now = GetTickCount64();
    if (now - g_lastConfigCheck > 1000) {
        g_lastConfigCheck = now;
        if (config::ReloadIfChanged()) LOGI("settings reloaded");
        std::wstring req = ModuleDir() + L"JWE3VR_shot.req";
        if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(req.c_str());
            DebugScreenshot();
        }
        // JWE3VR_hud.req: the in-world HUD texture (the UI the game drew this frame, on transparent black).
        std::wstring hudReq = ModuleDir() + L"JWE3VR_hud.req";
        if (GetFileAttributesW(hudReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(hudReq.c_str());
            bool inFlight = false;
            if (ID3D12Resource* hud = passes::PeekHudTexture())
                DebugDump(hud, D3D12_RESOURCE_STATE_RENDER_TARGET, L"JWE3VR_hud.bmp", inFlight);
            else
                LOGW("debug: no HUD texture (WorldHud off or not immersive)");
        }
        std::wstring scanReq = ModuleDir() + L"JWE3VR_cbscan.req";
        if (GetFileAttributesW(scanReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(scanReq.c_str());
            viewcb::RequestScan();
        }
        // JWE3VR_drive.req ("jeep" or "heli"): the Lua side takes control of the nearest ranger jeep / ACU
        // helicopter, as the radio and staff wand pick one (remote tests reach the seats without finding the
        // response facility on screen).
        std::wstring driveReq = ModuleDir() + L"JWE3VR_drive.req";
        if (GetFileAttributesW(driveReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
            char text[16]{};
            FILE* f = nullptr;
            if (_wfopen_s(&f, driveReq.c_str(), L"r") == 0 && f) {
                fread(text, 1, sizeof(text) - 1, f);
                fclose(f);
            }
            DeleteFileW(driveReq.c_str());
            int command = strncmp(text, "heli", 4) == 0 ? 2 : 1;
            g_debugCommand = command;
            LOGI("debug: drive request (%s) passed to Lua", command == 2 ? "helicopter" : "jeep");
        }
        // JWE3VR_rt.req: log the render targets of a frame and save each large one (finding the UI target).
        std::wstring rtReq = ModuleDir() + L"JWE3VR_rt.req";
        if (GetFileAttributesW(rtReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(rtReq.c_str());
            passes::RequestCensus();
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
    // Debug (JWE3VR_aer.req): the next presented frames, each saved with the eye and tick it is paired with, to
    // check the alternate-eye pairing (with a large IPDScale the two eyes' frames differ visibly).
    static int s_aerDump = 0, s_aerIndex = 0;
    if (s_aerDump == 0 && now - g_lastConfigCheck < 50) {
        std::wstring aerReq = ModuleDir() + L"JWE3VR_aer.req";
        if (GetFileAttributesW(aerReq.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(aerReq.c_str());
            s_aerDump = 8;
            s_aerIndex = 0;
        }
    }
    if (s_aerDump > 0) {
        wchar_t file[64];
        swprintf(file, 64, L"JWE3VR_aer%d_eye%d.bmp", s_aerIndex, haveReq ? req.eye : -1);
        bool inFlight = false;
        DebugDump(bb, D3D12_RESOURCE_STATE_PRESENT, file, inFlight);
        LOGI("aer debug: present %d paired with eye %d, tick %llu (lag %d)", s_aerIndex, haveReq ? req.eye : -1,
             (unsigned long long)(haveReq ? req.tick : 0), config::Get().aerPresentLag);
        ++s_aerIndex;
        --s_aerDump;
    }
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
                g_bbCache[i] = b;
                b->Release();
            }
        }
    passes::OnPresent(fi);
    // The HUD panel's swapchain follows the UI texture (size and format); no redirect until it exists.
    bool hudReady = false;
    if (ID3D12Resource* ui = passes::PeekHudTexture(); ui && haveSwaps) {
        D3D12_RESOURCE_DESC ud = ui->GetDesc();
        hudReady = xr::EnsureHudSwapchain((UINT)ud.Width, ud.Height, passes::HudViewFormat());
    }
    passes::SetHudOutputReady(hudReady);

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
    ULONGLONG waitStart = GetTickCount64();
    xr::BeginFrame();
    LogFrameStats(GetTickCount64() - waitStart);
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
    if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_lastDirect = q, t_lastDirect = q;
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
    if (l != g_copyList) ++g_statSetRt;
    if (l != g_copyList && passes::OnSetRenderTargets(l, n, rtvs, single, swapped)) {
        o_SetRenderTargets(l, n, swapped, FALSE, dsv);
        return;
    }
    o_SetRenderTargets(l, n, rtvs, single, dsv);
}

static void STDMETHODCALLTYPE Hk_ClearRtv(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT* color, UINT n,
                                          const D3D12_RECT* rects) {
    if (l != g_copyList) passes::OnClearRtv(l, h);
    o_ClearRtv(l, h, color, n, rects);
}

static void STDMETHODCALLTYPE Hk_EndEvent(ID3D12GraphicsCommandList* l) {
    if (l != g_copyList) passes::OnEndEvent(l);
    o_EndEvent(l);
}

static void STDMETHODCALLTYPE Hk_Barrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    if (l != g_copyList) {
        passes::OnBarrier(l, n, b);
        for (UINT i = 0; b && i < n; ++i)
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && IsCachedBackbuffer(b[i].Transition.pResource)) {
                ++g_statLegacyBbBarrier;
                break;
            }
    }
    o_Barrier(l, n, b);
}

static void STDMETHODCALLTYPE Hk_BeginRenderPass(ID3D12GraphicsCommandList4* l, UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                                 const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags) {
    if ((void*)l != (void*)g_copyList) ++g_statRenderPass;
    o_BeginRenderPass(l, n, rts, ds, flags);
}

static void STDMETHODCALLTYPE Hk_Barrier7(ID3D12GraphicsCommandList7* l, UINT32 n, const D3D12_BARRIER_GROUP* groups) {
    if ((void*)l != (void*)g_copyList && groups) {
        ++g_statBarrier7;
        for (UINT32 g = 0; g < n; ++g) {
            if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
            for (UINT32 i = 0; i < groups[g].NumBarriers; ++i)
                if (IsCachedBackbuffer(groups[g].pTextureBarriers[i].pResource)) ++g_statBarrier7Bb;
        }
    }
    o_Barrier7(l, n, groups);
}

static void STDMETHODCALLTYPE Hk_CopyRegion(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                            const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (l != g_copyList && dst && IsCachedBackbuffer(dst->pResource)) {
        ++g_statCopyToBb;
        if (src) passes::OnCopyToBackbuffer(src->pResource);
    }
    if (l != g_copyList && dst && src && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        passes::OnCopy(l, dst->pResource, src->pResource);
    o_CopyRegion(l, dst, x, y, z, src, box);
}

static void STDMETHODCALLTYPE Hk_CopyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src) {
    if (l != g_copyList && IsCachedBackbuffer(dst)) {
        ++g_statCopyToBb;
        passes::OnCopyToBackbuffer(src);
    }
    if (l != g_copyList) passes::OnCopy(l, dst, src);
    o_CopyResource(l, dst, src);
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
        hooks::Create("ID3D12GraphicsCommandList::ClearRenderTargetView", hooks::VtableEntry(list, 48), (void*)Hk_ClearRtv,
                      (void**)&o_ClearRtv);
        hooks::Create("ID3D12Device::CreateRenderTargetView", hooks::VtableEntry(dev, 20), (void*)Hk_CreateRtv, (void**)&o_CreateRtv);
        // Diagnostics (counted only; see LogFrameStats).
        hooks::Create("ID3D12GraphicsCommandList::CopyTextureRegion", hooks::VtableEntry(list, 16), (void*)Hk_CopyRegion,
                      (void**)&o_CopyRegion);
        hooks::Create("ID3D12GraphicsCommandList::CopyResource", hooks::VtableEntry(list, 17), (void*)Hk_CopyResource,
                      (void**)&o_CopyResource);
        ID3D12GraphicsCommandList4* l4 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l4)))) {
            hooks::Create("ID3D12GraphicsCommandList4::BeginRenderPass", hooks::VtableEntry(l4, 68), (void*)Hk_BeginRenderPass,
                          (void**)&o_BeginRenderPass);
            l4->Release();
        }
        ID3D12GraphicsCommandList7* l7 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l7)))) {
            hooks::Create("ID3D12GraphicsCommandList7::Barrier", hooks::VtableEntry(l7, 80), (void*)Hk_Barrier7, (void**)&o_Barrier7);
            l7->Release();
        }
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
