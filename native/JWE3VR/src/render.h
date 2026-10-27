// D3D12/DXGI hooks and the per-frame path from the game's backbuffer to the headset.
#pragma once

#include "common.h"
#include "vrmath.h"

#include <functional>

namespace jvr::render {

// Hooks the real system d3d12/dxgi vtables (below Streamline). Safe to call before the game
// creates its device; hooks catch the game's objects from the first call on.
bool InstallHooks();

// Records a GPU copy of `src` (in `srcState`) into `dst` (in RENDER_TARGET) on the game's queue.
// Used by the Present path and by sync replay. Returns false if no queue is known yet.
bool CopyToImage(ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst);
// Records the same copy into any command list (dst stays RENDER_TARGET before and after).
void RecordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, ID3D12Resource* dst);
// Records commands with `record` into our own list and executes it on the game's queue.
bool Submit(const std::function<void(ID3D12GraphicsCommandList*)>& record);
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
ID3D12Device* Device();

ID3D12CommandQueue* GameQueue();
// The backbuffer the game's next Present will show (AddRef'd; caller releases). Null before the first Present.
ID3D12Resource* NextBackbuffer();
// Blocks (up to timeoutMs) until our copies submitted so far have finished on the GPU (before destroying swapchain
// images or upload/readback buffers). False = still running: callers must not free what those copies use.
bool WaitCopies(unsigned timeoutMs = 2000);

// Debug BMPs: can texels of this format be decoded, and write a readback buffer holding one texture copy
// (footprint `fp`, decoded as `format`) as <file> next to the DLL.
bool DebugFormatSupported(DXGI_FORMAT format);
bool SaveReadbackBmp(ID3D12Resource* rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, DXGI_FORMAT format, UINT w, UINT h,
                     const wchar_t* file);

// Lua bridge flags.
void SetTabletVisible(bool visible);
// Pointer dot (laser hit / rifle sight) at a stage-space position.
void SetPointer(bool visible, vm::Vec3 stagePos);

} // namespace jvr::render
