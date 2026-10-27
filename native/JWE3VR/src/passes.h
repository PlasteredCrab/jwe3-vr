// Per-pass work inside the game's own command lists, keyed on render targets, pass markers and
// resource states seen while the game records:
//
//  * Foveated rendering: D3D12 variable rate shading (tier 2). A shading-rate image (full rate in the
//    middle, 2x2 then 4x4 toward the edges, in angles from the eye centre) is bound whenever the game
//    sets a frame-sized scene render target. Shadow maps, UI and post passes keep full rate.
//  * In-world HUD: inside the UI pass (markers in UI.UIMarkers) the backbuffer render target is swapped
//    for a transparent texture of our own, so the eyes get the scene without UI and the UI becomes a
//    panel in the world (xr Hud layer).
//  * Per-eye TAA history (alternate-eye mode): after the first TAA pass of a frame (markers in
//    Performance.TAAMarkers) its output is stashed for the eye that frame will be shown to (the rig's next
//    request), and before that eye's next TAA pass the stash is copied back into the history the pass
//    reads. viewcb points the previous-camera row at the same eye's previous position to match.
#pragma once

#include "common.h"

namespace jvr::passes {

struct FrameInfo {
    bool immersive = false;   // eyes are being rendered (not menus / flat screen)
    bool aer = false;         // alternate-eye stereo
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float fovY = 1.f, aspect = 1.f;
    bool worldHudAllowed = true; // false while the tablet is up (it needs the UI in the frame)
    ID3D12Resource* backbuffers[4]{};
};

// Clears the UI texture on our own queue (after its contents were copied to the HUD panel).
void ClearHud();

// Render thread, at Present (before the eye copies). Builds/updates the shading-rate image and the
// UI texture, and advances the frame parity.
void OnPresent(const FrameInfo& info);

// UI captured this frame (call once per Present after OnPresent): the texture to show as the HUD panel,
// in RENDER_TARGET state, or null if the UI pass was not redirected.
ID3D12Resource* TakeHudTexture();
bool WorldHudActive(); // the UI pass has been redirected recently (Lua keeps the GUI on)

// Hooks (game threads, while recording)
void OnCreateRtv(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);
void OnReset(ID3D12GraphicsCommandList* list);
// May rewrite the render target handles (in-world HUD). Returns true if `out` should be used instead.
bool OnSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                        D3D12_CPU_DESCRIPTOR_HANDLE* out);
void OnBeginEvent(ID3D12GraphicsCommandList* list, const std::string& name);
void OnEndEvent(ID3D12GraphicsCommandList* list);
void OnBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);

// Shared helper: does `name` contain any comma-separated key of `keys`?
bool MarkerMatches(const std::string& name, const std::string& keys);

} // namespace jvr::passes
