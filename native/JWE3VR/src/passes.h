// Per-pass work inside the game's own command lists, keyed on render targets, pass markers and
// resource states seen while the game records:
//
//  * Foveated rendering: D3D12 variable rate shading (tier 2). A shading-rate image (full rate in the
//    middle, 2x2 then 4x4 toward the edges, in angles from the eye centre) is bound whenever the game
//    sets a frame-sized scene render target. Shadow maps, UI and post passes keep full rate.
//  * In-world HUD: the game's UI draws are moved to a texture of our own, so the eyes get the scene without UI
//    and the UI becomes a panel in the world (xr Hud layer). JWE3 draws its UI into a texture of its own
//    (UI.UIDetect = texture, the default): bindings of that texture are swapped for ours. The older modes swap
//    the backbuffer inside the UI pass (markers in UI.UIMarkers) or after the first bindings of a frame.
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

// After the UI texture was copied to the HUD panel: clears it on our own queue for the next frame's UI.
void ClearHud();
// Present: whether the panel swapchain can take the UI texture (no redirect before it can be shown).
void SetHudOutputReady(bool ready);
// The view format the HUD panel should show the UI texture through (its swapchain format); UNKNOWN before it exists.
DXGI_FORMAT HudViewFormat();
// A game copy into the backbuffer: its source is where the game drew scene + UI (the HUD redirect's target).
void OnCopyToBackbuffer(ID3D12Resource* src);

// Render thread, at Present (before the eye copies). Builds/updates the shading-rate image and the
// UI texture, and advances the frame parity.
void OnPresent(const FrameInfo& info);

// UI captured this frame (call once per Present after OnPresent): the texture to show as the HUD panel,
// in RENDER_TARGET state, or null if the UI pass was not redirected.
ID3D12Resource* TakeHudTexture();
// The HUD texture itself (debug dumps), whether or not the UI drew into it this frame.
ID3D12Resource* PeekHudTexture();
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
// A render-target clear (traced for UI textures; the clear itself is left alone).
void OnClearRtv(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE view);

// Debug (JWE3VR_rt.req): log what the next frame does with each large texture (bindings, states, copies), then
// save each one as JWE3VR_rt<n>.bmp, captured inside the game's list just after it was written.
void RequestCensus();
// A game copy between resources (whole resource or one subresource), for the census.
void OnCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src);

// Shared helper: does `name` contain any comma-separated key of `keys`?
bool MarkerMatches(const std::string& name, const std::string& keys);

} // namespace jvr::passes
