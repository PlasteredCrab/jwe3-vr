// Main view constant buffer: discovery, and synchronous stereo by replaying the frame.
//
// Discovery (always on with SyncDiscovery=1): root CBVs bound while the game records are
// matched against the camera position Lua last set. The row holding the camera position is
// logged with the buffer, so the layout can be compared with Planet Coaster 2's (row 84 = position,
// row 85 = previous position, 106 rows in total).
//
// Sync mode (Stereo.Mode = sync, experimental): when one ExecuteCommandLists call contains the main
// view CB, it is run once per eye. Between the runs a helper thread rewrites the camera position rows
// in upload memory, ordered with GPU fences (the PlanetCoaster2VR technique, written fresh here).
// Cobra renders camera-relative, so moving the position rows moves the eye; orientation and the
// symmetric projection stay the game's. Lists whose first render pass marker matches
// Performance.SharedPassMarkers run only once (shadows, GI, skinning...).
#pragma once

#include "common.h"
#include "rig.h"
#include "xr.h"

namespace jvr::viewcb {

void OnCommandListReset(ID3D12GraphicsCommandList* list);
void OnRootCbv(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS va);
// PIX event payload -> readable pass name (unicode, ANSI or PIX3-encoded).
std::string DecodeMarker(UINT metadata, const void* data, UINT size);
void OnMarker(ID3D12GraphicsCommandList* list, const std::string& name);
void OnMap(ID3D12Resource* resource, void* cpu);

// Called from the ExecuteCommandLists hook. Returns true if it executed the lists itself (sync replay).
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
bool OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn original);

// Present: true if sync replay captured the left eye this frame (the right eye is the backbuffer).
bool TakeLeftCaptured();
void OnPresent();

// Lua told us where the game camera is this tick (discovery target).
// eye: AER eye (0/1) whose position `centrePos` is, or -1 for a centre camera (mono / sync).
void SetCameraTarget(vm::Vec3 centrePos, vm::Vec3 eyeOffsetLeft, vm::Vec3 eyeOffsetRight, int eye);

} // namespace jvr::viewcb
