# JWE3 VR

Full 6DoF VR for Jurassic World Evolution 3: your headset is the camera in every mode, and your hands drive
the jeep, aim the dart and medicine guns, point at the park and give your staff orders.

Ported from [JWE2 VR](https://github.com/PlasteredCrab/jwe2-vr) (same studio, same Cobra engine family).

Design guide: https://claude.ai/artifact/4X3NR1K6W3fxGVFGwkptwz

> **Status: Phase 0 done offline, not yet run in the game.** Checked against JWE3 build 1792088119 (Steam build
> 25967550): the exe, the Lua stack layout, every Lua binding the mod calls and the decompiled game Lua it hooks.
> The code was updated for what changed (see [Phase 0 checklist](#phase-0-checklist)). It builds, the offline
> tests pass and the content pack packs into a JWE3 `Main.ovl`. **Nothing has run in JWE3 or a headset yet.**
> Features below describe what the code does, not what has been seen working.

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
- In-world HUD: the game's UI is drawn as a floating panel below eye level that follows you lazily, instead of
  being painted flat onto both eyes. You point at it to click. (Falls back to the tablet if the game's UI pass
  can't be captured.)
- Camera shake removed; shakes and shots become controller haptics instead.

**Performance**
- One culling pass for both eyes (a single wide view that covers both).
- Fixed foveated rendering (variable rate shading): full detail where you look, coarser toward the edges of
  each eye. Needs a GPU with VRS tier 2 (NVIDIA RTX 20 or newer, AMD RX 6000 or newer, Intel Arc).
- Separate TAA history per eye, so neither eye smears in the other's image.
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
| Everywhere | Off-hand B/Y | Tablet: the game screen with its HUD in front of you |
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
| Camera choke point | `CameraModeManager.Advance` -> `SetTransform(main camera, tFinal.transformQ)`, `SetFOVY(nCameraFOV / nMagnify)`; `GetConfig().bCameraShake`; walking eye height 1.67 m (metres) | New modes mapped: Freelook and MapGenerator (overview), NestExit (flat screen) |
| Vehicle entry | Same `RequestChangeUI` / `DriveVehicle` call; JWE2's `_SetCameraMode` is gone; `StartInFirstPerson` init flag is new | Drive-mode patch sets `StartInFirstPerson` |
| Gunner aim and ammo | `GetCameraAimDirection`, `GetCameraAttachTransform`, `tModeData.gunnerID`, `VehicleGunnerUIMode:_SwitchAmmo` unchanged | None |
| Staff orders | `SelectVehicleDestination` with `{Vehicle, OrderPriority}` unchanged; `FindAvailableRangerCar` / `FindAvailableACUHelicopter` removed | New `jwe3vr.vehicles.lua` finds the nearest ranger car / ACU helicopter from the AI director's own tables |
| On-foot tools | `Tranquillise`, `IsDiseased`, `RemoveDisease`, `GetApproxNumDartsToCure` / `ToTreat` present; no `...ToTranquillise` | None (falls back to 3 darts) |
| Cursor ray | `Helpers.RaycastUtils` still calls `api.input.GetMouseRay` | None |
| UI renderer | Coherent Gameface (`cohtml`, Renoir) alongside Scaleform strings; a `WorldGUIPass` exists | `UIMarkers` default now `Gameface,Renoir,Scaleform,UIRender,HUD` (unverified until a `MarkerDump` run) |
| Render passes | 60 named nodes; TAA, shadow, environment capture, skinning and height-variance passes match the marker defaults | None |
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

## Install

Untested in the game so far. The pack's `Main.ovl` is built with cobra-tools (`E:\Modding\tools\cobra-tools`, a
Python 3.11 venv; see `re/lua/README.md`). Close the game, then:

```bash
powershell -ExecutionPolicy Bypass -File deploy.ps1 -Build
```

This copies `version.dll` next to the game exe and the pack (`JWE3VR.dll`, `openxr_loader.dll`, `Manifest.xml`,
`Main.ovl`) into `Win64\ovldata\JWE3VR`. Remove it again with `deploy.ps1 -Uninstall`.
`deploy.ps1 -Package` builds `dist\JWE3VR.zip` to unzip into a game folder. ACSE is not required.

## Run

1. Game settings first: **windowed or borderless**, **HDR off**, **frame generation off**, motion blur,
   depth of field, film grain and chromatic aberration off. Prefer the game's TAA over DLSS (TAA gets a history
   per eye; DLSS does not).
2. Start your OpenXR runtime (SteamVR, Oculus/Meta, Virtual Desktop/VDXR) with the headset on.
3. Start the game and load a park. Stand (or sit) facing forward and click both thumbsticks to recentre.

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
| `Performance.Foveation` | 0 off, 1 scene passes (default), 2 all frame-sized passes |
| `Performance.FoveaInnerDeg` / `MidDeg` / `OuterDeg` | Where shading drops to half, quarter and 4x4 |
| `Performance.PerEyeTAA` | Separate TAA history per eye (alternate-eye mode) |
| `Performance.MarkerDump` | Log the game's render pass names (to tune the marker lists) |

## Troubleshooting

- **No VR at all:** check `JWE3VR.log`. "OpenXR init failed" = runtime not running. No log = `version.dll` not loaded.
- **3D looks wrong (inside-out, doubled, one eye a frame late):** set `AERPresentLag` to 0, 1 or 2, or flip `SwapEyes`.
- **Camera does not follow the head:** the Lua side isn't running. Look in the game's Lua trace for
  "JWE3VR: hooks installed", "JWE3VR: manager added to the park environment" and "JWE3VR: bridge loaded", and in
  `JWE3VR.log` for "bridge: unexpected Lua stack layout".
- **HUD stays flat in both eyes / no panel:** look for "in-world HUD active" in the log. If it never appears, turn on
  `MarkerDump`, find the UI pass name in the log and add it to `UIMarkers`; until then the tablet shows the HUD.
- **Blurry edges:** lower foveation (`FoveaInnerDeg` up, or `Foveation = 0`). "foveation: VRS tier 2" in the log
  confirms your GPU supports it.
- **Smearing in alternate-eye mode:** look for "per-eye TAA: TAA output ... found"; if missing, tune `TAAMarkers`
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
| `re/ghidra.ps1`, `re/ghidra_scripts` | Headless Ghidra: import with the Denuvo blob removed, decompile `re/targets.txt` |
| `third_party/fetch.ps1` | Fetches MinHook, the OpenXR loader SDK and the Lua 5.3 source |

## Legal

The EULA forbids reverse engineering and mods; Frontier has tolerated script mods for years. This mod ships
only its own code: no game files and no patched executables. Extracted game Lua, decompiles and the Ghidra
database stay on the developer's machine (see `.gitignore`).
