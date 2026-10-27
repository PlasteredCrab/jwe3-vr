#include "viewcb.h"
#include "config.h"
#include "log.h"
#include "passes.h"
#include "render.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

namespace jvr::viewcb {

using namespace vm;

struct ListInfo {
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> cbvs;
    std::string firstMarker;
};

struct Mapping {
    UINT64 size;
    uint8_t* cpu;
};

// Cameras recently handed to the game. The submission being executed belongs to a tick or two ago,
// so matching (and the eye positions written for it) use whichever recent camera it actually holds.
struct Target {
    Vec3 centre, offL, offR;
    int eye = -1; // AER: which eye this camera was (its centre is that eye's position)
};

static SRWLOCK g_listLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12GraphicsCommandList*, ListInfo> g_lists;
static SRWLOCK g_mapLock = SRWLOCK_INIT;
static std::map<UINT64, Mapping> g_maps; // GPU VA start -> CPU pointer (persistently mapped upload buffers)

static std::mutex g_targetLock;
static std::deque<Target> g_targets;

// Discovery result
static int g_posRow = -1; // float4 row holding the camera position
static int g_frames = 0;
static bool g_layoutLogged = false;
static std::set<std::string> g_markersSeen;

// Sync replay
static ID3D12Fence* g_done = nullptr;
static ID3D12Fence* g_go = nullptr;
static UINT64 g_syncValue = 0;
static std::mutex g_jobLock;
static std::condition_variable g_jobCv;
struct Job {
    UINT64 value = 0;
    std::vector<uint8_t*> rows;
    Vec3 pos, prevPos;
    bool pending = false;
};
static Job g_job;
static bool g_leftCaptured = false;
static bool g_replayedThisFrame = false;
static Vec3 g_lastRunPos;
static bool g_haveLastRun = false;

// Recording root CBVs costs a lock per bind on every game render thread, so it only runs while it is
// needed: until the view CB has been found (discovery), in sync mode, or while dumping markers.
static bool Active() {
    const Settings& c = config::Get();
    return c.enabled && (c.stereo == StereoMode::Sync || c.markerDump || (c.syncDiscovery && g_posRow < 0) ||
                         (c.stereo == StereoMode::AER && c.perEyeTaa)); // per-eye TAA patches the previous-camera row
}

void OnCommandListReset(ID3D12GraphicsCommandList* list) {
    if (!Active()) return;
    AcquireSRWLockExclusive(&g_listLock);
    ListInfo& li = g_lists[list];
    li.cbvs.clear();
    li.firstMarker.clear();
    ReleaseSRWLockExclusive(&g_listLock);
}

void OnRootCbv(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS va) {
    if (!Active()) return;
    AcquireSRWLockExclusive(&g_listLock);
    ListInfo& li = g_lists[list];
    bool dup = false;
    for (size_t i = li.cbvs.size() > 8 ? li.cbvs.size() - 8 : 0; i < li.cbvs.size(); ++i) dup |= li.cbvs[i] == va;
    if (!dup && li.cbvs.size() < 4096) li.cbvs.push_back(va);
    ReleaseSRWLockExclusive(&g_listLock);
}

std::string DecodeMarker(UINT metadata, const void* data, UINT size) {
    std::string s;
    if (!data || size == 0) return s;
    if (metadata == 0) { // WINPIX_EVENT_UNICODE_VERSION
        const wchar_t* w = (const wchar_t*)data;
        for (UINT i = 0; i < size / 2 && w[i]; ++i) s.push_back((char)w[i]);
        return s;
    }
    // ANSI, or PIX3 encoded (strings packed into UINT64s, as 8-bit or UTF-16 characters): keep the
    // longest printable run, read both as bytes and as 16-bit units. JWE3 sends PIX3 events with UTF-16
    // names, which a byte scan cuts to their first letter.
    const uint8_t* b = (const uint8_t*)data;
    std::string run;
    for (UINT i = 0; i < size; ++i) {
        if (b[i] >= 32 && b[i] < 127) {
            run.push_back((char)b[i]);
        } else {
            if (run.size() > s.size()) s = run;
            run.clear();
        }
    }
    if (run.size() > s.size()) s = run;
    run.clear();
    for (UINT i = 0; i + 1 < size; i += 2) {
        unsigned c = b[i] | (b[i + 1] << 8);
        if (c >= 32 && c < 127) {
            run.push_back((char)c);
        } else {
            if (run.size() > s.size()) s = run;
            run.clear();
        }
    }
    if (run.size() > s.size()) s = run;
    return s;
}

void OnMarker(ID3D12GraphicsCommandList* list, const std::string& name) {
    const Settings& c = config::Get();
    // Markers only matter here for sync's shared passes and for the dump.
    if (!c.enabled || !(c.stereo == StereoMode::Sync || c.markerDump)) return;
    if (name.empty()) return;
    AcquireSRWLockExclusive(&g_listLock);
    ListInfo& li = g_lists[list];
    if (li.firstMarker.empty()) li.firstMarker = name;
    bool fresh = c.markerDump && g_markersSeen.size() < 2000 && g_markersSeen.insert(name).second;
    ReleaseSRWLockExclusive(&g_listLock);
    if (fresh) LOGI("marker: %s", name.c_str());
}

void OnMap(ID3D12Resource* r, void* cpu) {
    if (!Active()) return;
    D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    UINT64 va = r->GetGPUVirtualAddress();
    AcquireSRWLockExclusive(&g_mapLock);
    g_maps[va] = {d.Width, (uint8_t*)cpu};
    ReleaseSRWLockExclusive(&g_mapLock);
}

void SetCameraTarget(Vec3 centre, Vec3 offL, Vec3 offR, int eye) {
    std::lock_guard<std::mutex> l(g_targetLock);
    g_targets.push_front({centre, offL, offR, eye});
    while (g_targets.size() > 8) g_targets.pop_back();
}

static uint8_t* CpuOf(D3D12_GPU_VIRTUAL_ADDRESS va, UINT64& avail) {
    uint8_t* p = nullptr;
    AcquireSRWLockShared(&g_mapLock);
    auto it = g_maps.upper_bound(va);
    if (it != g_maps.begin()) {
        --it;
        if (va >= it->first && va < it->first + it->second.size) {
            p = it->second.cpu + (va - it->first);
            avail = it->first + it->second.size - va;
        }
    }
    ReleaseSRWLockShared(&g_mapLock);
    return p;
}

// Mapped upload memory may have been released since it was recorded: never let a stale pointer crash
// the game. Rows are only ever written right after being read back and matched in the same call.
static bool SafeRead(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int MatchTarget(const float* v, const std::vector<Target>& targets) {
    for (size_t i = 0; i < targets.size(); ++i) {
        Vec3 p = targets[i].centre;
        float eps = std::max(0.02f, Length(p) * 2e-5f);
        if (std::fabs(v[0] - p.x) < eps && std::fabs(v[1] - p.y) < eps && std::fabs(v[2] - p.z) < eps) return (int)i;
    }
    return -1;
}

// Finds the view CB copies bound by these lists and which recent camera they hold.
static std::vector<uint8_t*> FindViewRows(UINT count, ID3D12CommandList* const* lists, const std::vector<Target>& targets,
                                          std::vector<bool>& listHasView, int& matched) {
    std::vector<uint8_t*> rows;
    listHasView.assign(count, false);
    matched = -1;
    std::vector<std::pair<UINT, std::vector<D3D12_GPU_VIRTUAL_ADDRESS>>> cbvs;
    AcquireSRWLockShared(&g_listLock);
    for (UINT i = 0; i < count; ++i) {
        auto it = g_lists.find((ID3D12GraphicsCommandList*)lists[i]);
        if (it != g_lists.end()) cbvs.push_back({i, it->second.cbvs});
    }
    ReleaseSRWLockShared(&g_listLock);

    std::set<uint8_t*> seen;
    for (auto& [li, vas] : cbvs) {
        for (D3D12_GPU_VIRTUAL_ADDRESS va : vas) {
            UINT64 avail = 0;
            uint8_t* cpu = CpuOf(va, avail);
            if (!cpu || avail < 64) continue;
            int found = -1, which = -1;
            if (g_posRow >= 0) {
                // Known layout: read just the one row (upload memory is slow to read).
                float row[4];
                if ((UINT64)(g_posRow + 2) * 16 > avail || !SafeRead(cpu + g_posRow * 16, row, sizeof(row))) continue;
                which = MatchTarget(row, targets);
                if (which >= 0) found = g_posRow;
            } else {
                UINT64 bytes = std::min<UINT64>(avail, 2048);
                float buf[512];
                if (!SafeRead(cpu, buf, (size_t)bytes)) continue;
                int nrows = (int)(bytes / 16);
                for (int r = 0; r + 1 < nrows && found < 0; ++r) {
                    which = MatchTarget(buf + r * 4, targets);
                    if (which >= 0) found = r;
                }
                if (found >= 0) {
                    g_posRow = found;
                    LOGI("view CB found: camera position in row %d of a %llu-byte buffer at GPU VA %llx "
                         "(Planet Coaster 2: row 84 of 1700)",
                         found, (unsigned long long)avail, (unsigned long long)va);
                    if (!g_layoutLogged) {
                        g_layoutLogged = true;
                        for (int r = std::max(0, found - 6); r < std::min(nrows, found + 18); ++r)
                            LOGI("  row %3d: % .4f % .4f % .4f % .4f", r, buf[r * 4], buf[r * 4 + 1], buf[r * 4 + 2], buf[r * 4 + 3]);
                    }
                }
            }
            if (found < 0) continue;
            if (matched < 0) matched = which;
            if (which != matched) continue; // one frame, one camera
            listHasView[li] = true;
            uint8_t* rowPtr = cpu + (size_t)found * 16;
            if (seen.insert(rowPtr).second) rows.push_back(rowPtr);
        }
    }
    return rows;
}

static void HelperMain() {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(g_jobLock);
            g_jobCv.wait(l, [] { return g_job.pending; });
            job = g_job;
            g_job.pending = false;
        }
        bool ok = true;
        if (g_done->GetCompletedValue() < job.value) {
            g_done->SetEventOnCompletion(job.value, ev);
            ok = WaitForSingleObject(ev, 500) == WAIT_OBJECT_0;
        }
        if (ok) {
            float pos[3] = {job.pos.x, job.pos.y, job.pos.z};
            float prev[3] = {job.prevPos.x, job.prevPos.y, job.prevPos.z};
            for (uint8_t* row : job.rows) {
                SafeWrite(row, pos, sizeof(pos));
                SafeWrite(row + 16, prev, sizeof(prev)); // previous position (row + 1): the other eye's run
            }
        } else {
            LOGW("sync: eye 0 did not finish within 500 ms; eye 1 renders with eye 0's camera");
        }
        g_go->Signal(job.value); // never leave the queue waiting
    }
}

static bool StartSync(ID3D12CommandQueue* q) {
    if (g_done) return true;
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    bool ok = SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_done))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_go)));
    dev->Release();
    if (!ok) return false;
    std::thread(HelperMain).detach();
    LOGI("sync replay ready (one replay per frame; capture state %s)", config::Get().syncCaptureRT ? "RENDER_TARGET" : "PRESENT");
    return true;
}

static bool IsShared(const std::string& marker, const std::string& list) { return passes::MarkerMatches(marker, list); }

bool OnExecute(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists, ExecuteFn original) {
    const Settings& cfg = config::Get();
    if (!Active() || count == 0 || g_replayedThisFrame) return false;
    // Until the row is known, the full 2 KB scan of every bound CB runs only every 30th frame.
    if (g_posRow < 0 && (g_frames % 30) != 0) return false;
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> l(g_targetLock);
        targets.assign(g_targets.begin(), g_targets.end());
    }
    if (targets.empty()) return false;

    std::vector<bool> hasView;
    int matched = -1;
    std::vector<uint8_t*> rows = FindViewRows(count, lists, targets, hasView, matched);
    if (rows.empty() || matched < 0 || rig::CurrentKind() == rig::AnchorKind::Off) return false;
    if (cfg.stereo == StereoMode::AER) {
        // Per-eye TAA: the history this frame reads is this eye's own (two frames old), so the "previous
        // camera" the game reprojects from must be this eye's previous position, not the other eye's.
        const Target& t = targets[matched];
        if (cfg.perEyeTaa && t.eye >= 0) {
            for (size_t j = (size_t)matched + 1; j < targets.size(); ++j)
                if (targets[j].eye == t.eye) {
                    float prev[3] = {targets[j].centre.x, targets[j].centre.y, targets[j].centre.z};
                    for (uint8_t* row : rows) SafeWrite(row + 16, prev, sizeof(prev));
                    break;
                }
        }
        return false; // the game's submission runs as is
    }
    if (cfg.stereo != StereoMode::Sync) return false;
    ID3D12Resource* bb = render::NextBackbuffer();
    if (!bb) return false;
    if (!StartSync(q)) {
        bb->Release();
        return false;
    }
    g_replayedThisFrame = true;
    const Target& t = targets[matched];
    Vec3 left = t.centre + t.offL, right = t.centre + t.offR;

    // Eye 0: the GPU has not read these rows yet (this submission is about to), so write them now.
    float pos[3] = {left.x, left.y, left.z};
    float prev[3] = {g_lastRunPos.x, g_lastRunPos.y, g_lastRunPos.z};
    for (uint8_t* row : rows) {
        SafeWrite(row, pos, sizeof(pos));
        if (g_haveLastRun) SafeWrite(row + 16, prev, sizeof(prev));
    }
    original(q, count, lists);

    // Capture eye 0 before eye 1 overwrites the backbuffer.
    if (ID3D12Resource* img = xr::AcquireImage(xr::SwapId::Left)) {
        render::CopyToImage(bb, cfg.syncCaptureRT ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PRESENT, img);
        xr::ReleaseImage(xr::SwapId::Left);
        g_leftCaptured = true;
    }
    bb->Release();

    // Eye 1: the GPU waits until the helper has rewritten the rows after eye 0 finished reading them.
    ++g_syncValue;
    q->Signal(g_done, g_syncValue);
    {
        std::lock_guard<std::mutex> l(g_jobLock);
        g_job.value = g_syncValue;
        g_job.rows = rows;
        g_job.pos = right;
        g_job.prevPos = left;
        g_job.pending = true;
    }
    g_jobCv.notify_one();
    q->Wait(g_go, g_syncValue);

    std::vector<ID3D12CommandList*> second;
    AcquireSRWLockShared(&g_listLock);
    for (UINT i = 0; i < count; ++i) {
        auto it = g_lists.find((ID3D12GraphicsCommandList*)lists[i]);
        bool shared = it != g_lists.end() && !hasView[i] && IsShared(it->second.firstMarker, cfg.sharedPassMarkers);
        if (!shared) second.push_back(lists[i]);
    }
    ReleaseSRWLockShared(&g_listLock);
    original(q, (UINT)second.size(), second.data());
    g_lastRunPos = right;
    g_haveLastRun = true;
    return true;
}

bool TakeLeftCaptured() {
    bool v = g_leftCaptured;
    g_leftCaptured = false;
    return v;
}

void OnPresent() {
    g_replayedThisFrame = false;
    // FindViewRows re-checks the known row on every submission, so a layout change (game update,
    // another render path) just stops sync replay for those frames instead of writing wrong rows.
    if ((++g_frames % 3600) == 0 && config::Get().stereo == StereoMode::Sync)
        LOGI("sync: view CB row %d (frame %d)", g_posRow, g_frames);
}

} // namespace jvr::viewcb
