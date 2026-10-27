# JWE3 VR

Full 6DoF VR for Jurassic World Evolution 3: your headset is the camera in every mode, and your hands drive
the jeep, aim the dart and medicine guns, point at the park and give your staff orders.

Ported from [JWE2 VR](https://github.com/PlasteredCrab/jwe2-vr) (same studio, same Cobra engine family).

Design guide: https://claude.ai/artifact/4X3NR1K6W3fxGVFGwkptwz

> **Status: first runs in JWE3 (game 1.5.5, build 1792088119), headset not worn.** Seen working in the game: the
> mod loads without a proxy DLL, the Lua bridge and hooks run, OpenXR starts on SteamVR, and in a Sandbox park the
> VR rig drives the rendered camera (headset FOV, the two eyes' positions, foveated rendering on), the game's UI is
> moved out of the eye images into the in-world HUD panel's texture, taking direct control of a ranger jeep or the
> capture helicopter puts you in its seat in first person, the gun seat puts you at the gun mount, and the
> virtual Xbox pad reaches the game (a scripted pad drove the jeep and entered the gun seat). Not yet seen: head
> motion and stereo depth in a worn headset, the HUD panel itself in the headset, the VR controllers, walking
> (guest view), and
> per-eye TAA and sync stereo (both need the game's view constant buffer, which the Sophos-safe install without
> version.dll cannot reach). Features below describe what the code does; see
> `STATUS.txt` for what each run showed.

## Features

**Seeing the park in VR**
- Head tracking in every camera mode: your headset is the game camera (6DoF), with no pitch or roll forced on
  you, snap or smooth turn, recentre, seated and standing play. Covers JWE3's new modes too: the Freelook park
  camera and the sandbox map generator as the park model, the nest-exit cinematic on the flat screen.
- Stereo 3D by alternate-eye rendering (one eye per frame), plus an experimental mode that renders both eyes every
  frame by replaying the frame.
- True world scale in metres: walking and vehicles are 1:1, the park overview is a 1:30 model you can grab,
  scale and rotate.
- Menus, loading screens and cutscenes on a large flat screen in front of you.
- Vehicles in first person: jeeps start in the driver's seat, and helicopters (third-person only in JWE3) are
  switched to their own seat camera, so you sit in the vehicle instead of floating behind it.
- In-world HUD: the game's UI is drawn as a floating panel below eye level that follows you lazily, instead of
  being painted flat onto both eyes. You point at it to click. The mod finds the textures JWE3 draws its UI into
  by itself (no render-pass markers needed). (Falls back to the tablet if the UI can't be captured.)
- Camera shake removed; shakes and shots become controller haptics instead.

**Performance**
- One culling pass for both eyes (a single wide view that covers both).
- Fixed foveated rendering (variable rate shading): full detail where you look, coarser toward the edges of
  each eye. Needs a GPU with VRS tier 2 (NVIDIA RTX 20 or newer, AMD RX 6000 or newer, Intel Arc). In JWE3 it
  saves little: the game rasterises IDs and shades materials and light in compute passes, which shading rates do
  not reach (measured: no visible change at the edges in modes 1 and 2).
- Separate TAA history per eye, so neither eye smears in the other's image (needs the `version.dll` install,
  see Troubleshooting).
- In the experimental both-eyes mode, shadows, GI and skinning run once for both eyes.
- V-sync is forced off while the headset paces the game.

**Playing with your hands**
- Park overview: a laser picks and builds; a dot shows where it lands. Grab to move the park, two hands to scale
  and rotate it, the stick to glide.
- Walking (the game's first-person view): you walk where you look, at your real height.
- Ranger jeep and helicopter: you sit in the driver's seat (the game's own first-person seat camera); the
  controllers act as an Xbox pad, and gripping with both hands turns them into a steering wheel.
- Gun seats (jeep and helicopter): the dart, medicine and camera guns follow your hand; the trigger fires; the
  game's own ammo switch keeps the HUD right.
- Staff orders: point at a vehicle to take control of it, or at a dinosaur or building to send the nearest
  ranger team or ACU helicopter into the game's own order screen.
- On-foot ranger: while walking, carry a dart rifle, a medicine gun or a radio. Darts count toward the game's
  own estimate for that dinosaur, then it goes down, or its disease is cured. There is no visible gun or dart
  yet; a dot shows where you aim and haptics confirm hits. The radio sends teams, takes over vehicles, and calls
  the nearest free jeep to where you stand.
- Left-handed mode, and every setting in one ini file that reloads live.

**Install and safety**
- No script extender needed: the mod's content pack installs its own module hooks (ACSE for JWE3 is
  unmaintained), including a hook on the game's environment loader so the VR manager joins every park. It still
  works alongside ACSE.
- The Lua bridge checks the game's Lua stack layout on every call and does nothing if it doesn't match, so a game
  update turns VR off instead of crashing.
- No game files are patched; the exe is never modified.

## Controls

"Dominant" is your right hand (left with `LeftHanded = 1`); "off-hand" is the other one.

| Where | Control | Does |
| --- | --- | --- |
| Everywhere | Click both thumbsticks | Recentre |
| Everywhere | Menu button | Pause (hold off-hand grip: Back) |
| Park, walking | Off-hand B/Y | Tablet: the game screen with its HUD in front of you |
| Jeep / helicopter seat, gun seat | Off-hand grip + off-hand B/Y | Tablet (plain off-hand Y belongs to the game there) |
| Flat screen, tablet; in-world HUD in the park overview | Point with dominant hand | Moves the cursor |
| Flat screen, tablet; in-world HUD in the park overview | Dominant trigger / grip | Left click / right click |
| Park overview | Dominant laser + trigger | Pick, build, select (the dot shows where) |
| Park overview | One grip | Drag the park |
| Park overview | Both grips | Scale and rotate the park |
| Park overview | Off-hand stick | Glide over the park |
| Park overview | Dominant A on a vehicle | Take control of it |
| Park overview | Dominant B on a spot | Nearest ranger team opens the order screen (hold off-hand grip: ACU helicopter); point at the target and pull the trigger |
| Walking | Left stick | Walk where you look |
| Walking | Right stick | Snap turn (30 degrees; smooth with `SnapTurnDeg = 0`) |
| Walking | Tap dominant B | Cycle tool: empty hands, dart rifle, medicine gun, radio (stronger pulse per step) |
| Walking | Hold dominant B | Leave the first-person view (sends Escape) |
| Walking, dart rifle | Dominant trigger | Fire at what the dot is on (reloads in 1.2 s); after the game's dart estimate the dinosaur goes down |
| Walking, medicine gun | Dominant trigger | Fire at a sick dinosaur; cures its disease after the game's dart estimate (a weak buzz means nothing to treat) |
| Walking, radio | Dominant trigger on a vehicle | Take control of it |
| Walking, radio | Dominant trigger on a dinosaur or building | Send the nearest ranger team (hold off-hand grip: ACU helicopter) |
| Walking, radio | Dominant A | Call the nearest free jeep to where you stand |
| Jeep / helicopter seat | Right / left trigger | Gas / brake (as the game's pad layout) |
| Jeep / helicopter seat | Left stick | Steer |
| Jeep / helicopter seat | Both grips, turn your hands | Steering wheel |
| Jeep / helicopter seat | Head | Look around |
| Jeep / helicopter seat | Off-hand Y | The game's Aim Mode: into the gun seat (pad Y in JWE3) |
| Jeep / helicopter seat, gun seat | Dominant B | The game's Back (pad B): gun seat to driver's seat, then out of the vehicle |
| Jeep / helicopter seat | Off-hand grip alone | Site map (pad LB); not while both grips hold the wheel |
| Gun seat | Dominant hand | Aim (the gun follows it) |
| Gun seat | Dominant trigger | Fire (haptic kick) |
| Gun seat | Off-hand stick left / right | Previous / next ammo |

## Phase 0 checklist

All checked on 2026-10-26 against `JWE3.exe` build 1792088119 (Steam build 25967550) and its decompiled Lua.
Nothing below has been confirmed at runtime.

| Check | JWE3 result | What changed in the mod |
| --- | --- | --- |
| Script extender | ACSE for JWE3 is v0.730 (Oct 2025), unmaintained | Own loader, `jwe3vr.hooks.lua`; ACSE not needed |
| Exe and build | `JWE3.exe`, PE stamp 1792088119 | `scan::kKnownBuild` set; loader and DLL require `JWE3.exe` |
| Proxy DLL | The exe imports `VERSION.dll` (3 functions) | None: the `version.dll` proxy works as on JWE2 |
| Graphics | D3D12 + DXGI, NVIDIA Streamline (`sl.*.dll`), DLSS and FSR3 frame generation | None |
| Denuvo layout | Code in `.code` (38.7 MB); VM blob in executable `.shared` (252 MB) | Scanners and the Ghidra pre-script find both by flags and size |
| Lua | 5.3 with `loadlib`, `loadstring`, `tryrequire` | None |
| Lua stack layout | Same as JWE2: `L+0x10` top, `L+0x20` CallInfo, 24-byte slots, tag at +0x10, float32 tag 3, int tag 0x13 (64-bit), counter stored at `L-0x10` (checked in the `GetFOV` and `GetZoom` code) | None; the bridge's layout self-check stays |
| Stack-op counter | One dominant target, 175 uses | None |
| Lua bindings | 5,422 found; 90 (1.7%) jump into the VM. JWE3 adds a second registration pattern (function in `r8`, name in `rdx`) | `luaapi.py` reads both patterns |
| Content-pack Lua loading | `Database.Main` calls `AddContentToCall` on `Database.<Pack>LuaDatabase` for every loaded pack, at startup | None |
| Park manager injection | `EnvironmentPrototype.Managers` as on JWE2, but `Environment.Environment` loads prototypes through a `require` saved at game start, past our wrap | Hooks wrap `Environment._MergePrototype` to load the prototype through our `require` first |
| Camera choke point | `CameraModeManager.Advance` -> `SetTransform(main camera, tFinal.transformQ)`, `SetFOVY(nCameraFOV / nMagnify)`; `GetConfig().bCameraShake`; walking eye height 1.67 m (metres). In game: the camera component runs in a Lua state the mod cannot reach, so wrapping it does nothing | The VR manager reads the game's camera each frame and writes the VR camera over it (seen working); new modes mapped: Freelook and MapGenerator (overview), NestExit (flat screen) |
| Lua states | A park shares the startup Lua state; `api.*` tables are read-only ("Scripts may NOT modify the API") | Laser picking wraps `Helpers.RaycastUtils.GetCursorRaycast` instead of `api.input.GetMouseRay` |
| Vehicle entry | Same `RequestChangeUI` / `DriveVehicle` call; JWE2's `_SetCameraMode` is gone; `StartInFirstPerson` init flag is new | Drive-mode patch sets `StartInFirstPerson` |
| Gunner aim and ammo | `GetCameraAimDirection`, `GetCameraAttachTransform`, `tModeData.gunnerID`, `VehicleGunnerUIMode:_SwitchAmmo` unchanged | None |
| Staff orders | `SelectVehicleDestination` with `{Vehicle, OrderPriority}` unchanged; `FindAvailableRangerCar` / `FindAvailableACUHelicopter` removed | New `jwe3vr.vehicles.lua` finds the nearest ranger car / ACU helicopter from the AI director's own tables |
| On-foot tools | `Tranquillise`, `IsDiseased`, `RemoveDisease`, `GetApproxNumDartsToCure` / `ToTreat` present; no `...ToTranquillise` | None (falls back to 3 darts) |
| Cursor ray | `Helpers.RaycastUtils` still calls `api.input.GetMouseRay` | None |
| UI renderer | Coherent Gameface (`cohtml`, Renoir). The UI goes into two 1920x1080 RGBA8 textures used in turn (drawn through sRGB and UNORM views, cleared the frame after), copied out of RENDER_TARGET and composited by a compute pass into a 10-bit image that is copied into the backbuffer (which is never a render target) | New `UIDetect = texture` (default): learns those textures from their RENDER_TARGET -> COPY_SOURCE transition and redirects their bindings; the HUD panel gets its own RGBA8 sRGB swapchain |
| Render passes | 60 named nodes in the data, but retail emits no pass markers. TAA keeps its history in a ping-pong pair of 1920x1080 RGBA16 textures (run 27 census) | Per-eye TAA and sync stereo also need the view CB: with a late load (no version.dll) its upload ring was mapped before the Map hook, so neither runs (the log says so) |
| New JWE3 gameplay | Player-controllable aviary drones (with drone gun cameras), drone tranquillise tasks, monorail, underwater lagoon placement | Not covered yet (Phases 5 and 7) |

## Build

Needs Visual Studio (MSVC x64). Fetch the third-party code once (MinHook, OpenXR loader SDK, Lua 5.3 source for
the syntax check):

```bash
powershell -ExecutionPolicy Bypass -File third_party/fetch.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File native/JWE3VR/build.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File native/JWE3VR/test.ps1
```

The tests need no game or headset: rig math, the Lua bridge against a fake `lua_State` (including the layout
self-check), and a Lua 5.3 syntax check of the content pack.

Everything stays on E:: compiler temp files go to `native\JWE3VR\build\tmp`, downloads to `third_party\`, and the
tools to `E:\Modding\tools` (`cobra-tools`, portable `python311`, the `re-venv` for the RE scripts, `pip-cache`,
and `ghidra-home` for Ghidra's settings, cache and temp files).

## Install

Installed and run in JWE3 on the dev machine with `-NoProxy` (headset not worn yet). The pack's `Main.ovl` is built with cobra-tools (`E:\Modding\tools\cobra-tools`, a
Python 3.11 venv; see `re/lua/README.md`). Close the game, then:

```bash
powershell -ExecutionPolicy Bypass -File deploy.ps1 -Build -NoProxy
```

This copies the pack (`JWE3VR.dll`, `openxr_loader.dll`, `Manifest.xml`, `Main.ovl`) into `Win64\ovldata\JWE3VR`.
With `-NoProxy` (used on the dev machine) nothing goes next to the exe: the game's own Lua loads `JWE3VR.dll` when
content packs start, which avoids the `version.dll` side-loading pattern that endpoint protection (Sophos there)
watches for. Without `-NoProxy`, `version.dll` is also copied next to `JWE3.exe` so the mod starts before the
renderer. Remove it all with `deploy.ps1 -Uninstall`. `deploy.ps1 -Package` builds `dist\JWE3VR.zip` to unzip into a
game folder. ACSE is not required.

## Run

1. Game settings first: **windowed or borderless**, **HDR off**, **frame generation off**, motion blur,
   depth of field, film grain and chromatic aberration off. Prefer the game's TAA over DLSS. (With the
   `version.dll` install TAA can keep a history per eye; with `-NoProxy` it keeps one shared history.)
2. Start your OpenXR runtime (SteamVR, Oculus/Meta, Virtual Desktop/VDXR) with the headset on.
3. Start the game and load a park. Stand (or sit) facing forward and click both thumbsticks to recentre.

## First headset test

None of this has been seen in a worn headset yet. Go through it in order; each step names the line in
`Win64\ovldata\JWE3VR\JWE3VR.log` that says it works. When a step fails, note which one and keep the log.

1. **Main menu:** the menu is a large flat screen in front of you and the game window keeps working. Log:
   "swapchains 1920x1080 format 24" and, every 10 s, "frames: ... OpenXR state 5" (5 = focused; 3 means the
   headset is not being worn or the runtime is not showing the game).
2. **Park, head tracking:** load the Sandbox park. You look down on the park as a 1:30 model. Turning and leaning
   move the view one to one, with no lag or swimming and the horizon level. Log: "camera tick (manager path): mode
   Standard, VR override 1.0".
3. **Stereo depth:** close one eye, then the other: the two views must differ, and near things must look near. If
   the depth looks inside-out, doubled or a frame late, try `AERPresentLag` 0, 1 or 2, or flip `SwapEyes` (the ini
   reloads live).
4. **In-world HUD:** the game's toolbar and bottom bar float on a panel a little below eye level and follow when
   you turn away; nothing of the UI is painted flat on the eyes. Log: "the game draws its UI into texture",
   "the UI now goes to the panel", "in-world HUD active".
5. **Controllers:** a dot shows where the dominant laser hits the ground; the trigger selects; one grip drags the
   park, both grips scale and rotate it. Point at the HUD panel: the cursor follows and the trigger clicks a toolbar
   button.
6. **Tablet and recentre:** off-hand B/Y brings up the game screen with its HUD in front of you and hides it again;
   clicking both thumbsticks recentres.
7. **Frame rate:** the "frames: ... fps" line should be near the headset's refresh rate (72, 80 or 90).

## Settings (`Win64\ovldata\JWE3VR\JWE3VR.ini`, saved changes apply live)

The file is written with comments on first run. The ones you are most likely to touch:

| Setting | What it does |
| --- | --- |
| `Stereo.Mode` | `aer` (default), `sync` (experimental, both eyes every frame), `mono` |
| `Stereo.AERPresentLag`, `Stereo.SwapEyes` | Pair game frames with eyes if the 3D looks wrong |
| `Rig.OverviewScale` | Size of the park model (30 = 1:30) |
| `Rig.Seated`, `Rig.SnapTurnDeg` | Seated play; snap-turn angle (0 = smooth) |
| `Input.LeftHanded`, `Input.GunnerAimGain` | Handedness; how hard the gun chases your hand |
| `UI.WorldHud`, `UI.HudDistance`, `UI.HudWidth` | In-world HUD panel on/off, distance and size |
| `UI.UIDetect` | How the UI is found: `texture` (default, JWE3), `backbuffer`, `markers` |
| `Performance.Foveation` | 0 off, 1 HDR scene passes (default), 2 all frame-sized colour passes (never the UI or ID / motion-vector targets) |
| `Performance.FoveaInnerDeg` / `MidDeg` / `OuterDeg` | Where shading drops to half, quarter and 4x4 |
| `Performance.PerEyeTAA` | Separate TAA history per eye (alternate-eye mode) |
| `Performance.MarkerDump` | Log the game's render pass names (to tune the marker lists) |

## Troubleshooting

- **No VR at all:** check `JWE3VR.log`. "OpenXR init failed" = runtime not running. No log = the mod never loaded
  (with `-NoProxy` the game's Lua loads it at startup: look for "bridge: first call from Lua").
- **3D looks wrong (inside-out, doubled, one eye a frame late):** set `AERPresentLag` to 0, 1 or 2, or flip `SwapEyes`.
- **Camera does not follow the head:** the Lua side reports into `JWE3VR.log` (lines starting `lua:`). Expect
  "hooks installed", "VR manager Init (park loaded)", "gameplay init done", then "camera tick (manager path): ...
  VR override 1.0" and "camera: driven from the camera manager's own Advance". Without that last line the game's
  camera overwrites the rig every frame and only the FOV changes. "bridge: unexpected Lua stack layout" means a game
  update changed the Lua layout.
- **Vehicles ignore the controllers:** JWE3 adopts a pad only after it sees a keystroke from it; look for "virtual
  pad: announced on XInput slot 0" and, in `JWE3VR_shot.req` lines, XInputGetState calls on slot 0.
- **Remote testing:** `JWE3VR_shot.req` in the mod folder saves the next frame as `JWE3VR_shot.bmp`;
  `JWE3VR_pad.txt` plays clicks (`CLICK=x,y`), `FOCUS`, keys and pad buttons (format in `input.cpp`). The game reads
  no XInput on its menus and ignores posted keys; clicks register only while it has focus (add `FOCUS`).
  `re/remote/to_continue.sh` drives a full run into the last Sandbox save, `re/remote/to_menu.sh` stops at the main
  menu, and `re/remote/vehicle.sh <name> jeep|heli` takes direct control of a response facility's jeep or helicopter
  (switching the mod off live while it clicks through the menus, so the UI is in the screenshots). `JWE3VR_hud.req` saves the HUD texture
  (`JWE3VR_hud.bmp`; `re/remote/hudcheck.sh` puts it next to the frame), and `JWE3VR_rt.req` logs every render
  target of one frame and saves the large ones as `JWE3VR_rt<n>.bmp` (finding where something is drawn).
  `JWE3VR_cbscan.req` searches the game's large read-write memory for the camera position (constant-buffer hunting).
  `JWE3VR_aer.req` saves the next 8 frames as `JWE3VR_aer<n>_eye<e>.bmp` with the eye each is shown to (with a large
  `IPDScale` the eyes' frames differ visibly: a frame for the left eye shows the scene shifted right).
- **HUD stays flat in both eyes / no panel:** look for "the game draws its UI into texture", "the UI now goes to the
  panel" and "in-world HUD active" in the log. If the texture is never found (a game update), use `JWE3VR_rt.req`
  to see which render target holds the UI; until then the tablet shows the HUD.
- **Blurry edges:** lower foveation (`FoveaInnerDeg` up, or `Foveation = 0`). "foveation: VRS tier 2" in the log
  confirms your GPU supports it.
- **Smearing in alternate-eye mode:** per-eye TAA needs the view CB. "view CB discovery: ... Discovery stopped" in the
  log means the install without version.dll hides it; TAA then keeps one shared history (softer edges while the head
  moves). Otherwise look for "per-eye TAA: TAA output ... found"; if missing, tune `TAAMarkers`
  with `MarkerDump`. DLSS keeps one history; use the game's TAA instead.
- **Edges pop when turning your head fast:** raise `CullMarginDeg`.
- **Gun aims the wrong way or oscillates:** lower `GunnerAimGain`. The stick direction is learned automatically within a second.
- **Sync stereo:** leave `SyncDiscovery = 1`, play a minute, and look for "view CB found" in the log before trying `Mode = sync`.
  If the left eye is black or shows the previous frame, try `SyncCaptureState = present`; if it stays wrong, the
  game finishes its image in a later submission than the one replayed, and AER is the mode to use.
- **Half frame rate in the headset:** keep `ForceVsyncOff = 1` (the headset paces the game, not the monitor).

## What's in the repo

| Path | What |
| --- | --- |
| `native/JWE3VR/src` | The mod DLL (C++20): D3D12 hooks under Streamline, OpenXR, rig, stereo, input, Lua bridge |
| `native/JWE3VR/src/rig.*` | Anchor x head pose per camera mode, world scale, recentre, snap turn, overview grab |
| `native/JWE3VR/src/render.*` | Present hook: backbuffer -> eye / screen / HUD swapchains, xrEndFrame |
| `native/JWE3VR/src/passes.*` | Inside the game's passes: foveated rendering, UI capture for the in-world HUD, per-eye TAA |
| `native/JWE3VR/src/viewcb.*` | View constant buffer discovery and experimental synchronous stereo (frame replay) |
| `native/JWE3VR/src/input.*` | Virtual Xbox pad (XInput), laser -> mouse cursor, clicks |
| `native/JWE3VR/src/bridge.*` | `jwe3vr_lua`: numbers in/out on the game's Lua stack, no Lua linked, layout self-check |
| `native/loader` | `version.dll` proxy that loads the mod at game start |
| `native/JWE3VR/test` | Offline checks (rig math; the bridge against a fake `lua_State`) |
| `pack/JWE3VR` | Content pack: own module hooks, camera wrap, gameplay, on-foot ranger, staff wand |
| `re/tools` | `survey.py` (Phase 0 binary survey), strings / LEA scan / Lua binding map / refs / disassembly, Lua decompile, `luagrep.py` (search extracted Lua) |
| `re/remote` | Remote test runs without a person at the PC: deploy, launch, click into a park, screenshots, logs |
| `re/ghidra.ps1`, `re/ghidra_scripts` | Headless Ghidra: import with the Denuvo blob removed, decompile `re/targets.txt` |
| `third_party/fetch.ps1` | Fetches MinHook, the OpenXR loader SDK and the Lua 5.3 source |

## Legal

The EULA forbids reverse engineering and mods; Frontier has tolerated script mods for years. This mod ships
only its own code: no game files and no patched executables. Extracted game Lua, decompiles and the Ghidra
database stay on the developer's machine (see `.gitignore`).
