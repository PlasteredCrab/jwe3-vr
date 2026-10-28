# JWE3 VR

Full 6DoF VR for Jurassic World Evolution 3: your headset is the camera in every mode, and your hands drive
the jeep, aim the dart and medicine guns, point at the park and give your staff orders.

Ported from [JWE2 VR](https://github.com/PlasteredCrab/jwe2-vr) (same studio, same Cobra engine family).

Design guide: https://claude.ai/artifact/4X3NR1K6W3fxGVFGwkptwz

> **Status: first runs in JWE3 (game 1.5.5, build 1792088119), headset not worn.** Seen working in the game: the
> mod loads without a proxy DLL, the Lua bridge and hooks run, OpenXR starts on SteamVR, and in a Sandbox park the
> VR rig drives the rendered camera (headset FOV, the two eyes' positions, foveated rendering on), the game's UI is
> moved out of the eye images into the in-world HUD panel's texture, taking direct control of a ranger jeep or the
> capture helicopter puts you in its seat in first person, the gun seat puts you at the gun mount (the gunner's
> own model hidden), and the
> virtual Xbox pad reaches the game (a scripted pad drove the jeep, lifted the helicopter off and entered both gun seats).
> The guest view (walking) puts the rig on the frame too, and a scripted stick walked it; photo mode works from
> the jeep, the capture helicopter and the aviary drone (a photo was taken from the drone), the pause menu lands on the HUD panel, and leaving a park and loading it again works. With scripted VR
> controllers in a Sandbox park built remotely (paths, response facility, hatchery, enclosure, aviary): the on-foot
> dart rifle tranquillised a dinosaur (the game's panel: TRANQUILIZED) and the medicine gun cured one, the radio
> called a jeep (a Fire Flare task in the game's list) and opened the team order screen, and the aviary drone flew,
> entered its aim mode and tranquillised a Dimorphodon. Not yet seen: head motion
> and stereo depth in a worn headset, the HUD panel itself in the headset, real VR controllers in your hands (the
> runs above used scripted ones, and the darts were aimed at the dinosaur by a debug aid), a helicopter or jeep
> dart landing on a dinosaur, and
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
- The main menu, loading screens and cutscenes on a large flat screen in front of you; in a park the pause menu
  is on the HUD panel.
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
- Gun seats (jeep, helicopter and the aviary drone's aim mode): the dart, medicine and camera guns follow your hand
  (the gun settles within about half a degree of where you point), and a dot marks where the gun itself points
  (the game's crosshair is on the HUD panel, not in the world); the trigger fires; the game's own ammo switch keeps
  the HUD right. The gunner character (who stands where your eyes are) is hidden while you are in the seat and
  comes back on the flat screen.
- Photo mode from the jeep, the capture helicopter and the aviary drone, from the driver's seat or the gun seat:
  click the stick (again to go back), and A takes the photo.
- Staff orders: point at a vehicle to take control of it, or at a dinosaur or building to send the nearest
  ranger team or ACU helicopter into the game's own order screen.
- On-foot ranger: while walking, carry a dart rifle, a medicine gun or a radio. Medicine darts count toward the
  game's own estimate for treating that dinosaur, then its disease is cured; three tranquilliser darts put it
  down (the game has no dart estimate for that). Darts fly through grass and bushes but stop at fences. There is
  no visible gun or dart yet; a dot shows where you aim and haptics confirm hits. The radio sends teams, takes
  over vehicles, and calls the nearest free jeep to where you stand.
- Aviary drone (PILOT DRONE in the aviary hatchery's panel): you fly it from its own camera with the vehicle
  controls; once it is airborne, off-hand Y enters its aim mode, where its equipment (scanner, medicine,
  tranquilliser, stimulant, deep analyser) follows your hand and the trigger uses it, as in the gun seats.
- Left-handed mode, and every setting in one ini file that reloads live.

**Install and safety**
- No script extender needed: the mod's content pack installs its own module hooks (ACSE for JWE3 is
  unmaintained), including a hook on the game's environment loader so the VR manager joins every park. It still
  works alongside ACSE.
- The Lua bridge checks the game's Lua stack layout on every call and does nothing if it doesn't match, so a game
  update turns VR off instead of crashing.
- No game files are patched; the exe is never modified.
- No OS-level input: the laser's cursor and clicks are messages to the game's own window (your mouse cursor is
  never moved, nothing reaches other windows), and the controllers reach the game as a virtual Xbox pad.

## Controls

"Dominant" is your right hand (left with `LeftHanded = 1`); "off-hand" is the other one.

| Where | Control | Does |
| --- | --- | --- |
| Everywhere | Click both thumbsticks | Recentre |
| Everywhere | Menu button | Pause (hold off-hand grip: Back) |
| Pause menu (opened from the park overview) | Dominant A / B, sticks | Select / back (B resumes), move; the laser on the HUD panel clicks too |
| Park, walking | Off-hand B/Y | Tablet: the game screen with its HUD in front of you |
| Jeep / helicopter seat, gun seat | Off-hand grip + off-hand B/Y | Tablet (plain off-hand Y belongs to the game there) |
| Flat screen, tablet; in-world HUD in the park overview | Point with dominant hand | Moves the game's cursor (your real mouse cursor stays put) |
| Flat screen, tablet; in-world HUD in the park overview | Dominant trigger / grip | Left click / right click |
| Park overview | Dominant laser + trigger | Pick, build, select (the dot shows where) |
| Park overview | One grip | Drag the park |
| Park overview | Both grips | Scale and rotate the park |
| Park overview | Off-hand stick | Glide over the park |
| Park overview | Dominant A on a vehicle | Take control of it |
| Park overview | Dominant B on a spot | Nearest ranger team opens the order screen (hold off-hand grip: ACU helicopter); point at the target and pull the trigger. A long weak buzz: no team can take orders (the game refuses them while their facility is not operational, e.g. without power) |
| Walking | Left stick | Walk where you look |
| Walking | Right stick | Snap turn (30 degrees; smooth with `SnapTurnDeg = 0`) |
| Walking | Tap dominant B | Cycle tool: empty hands, dart rifle, medicine gun, radio (stronger pulse per step) |
| Walking | Hold dominant B | Leave the first-person view (the game's pad B) |
| Walking, dart rifle | Dominant trigger | Fire at what the dot is on (reloads in 1.2 s); after the game's dart estimate the dinosaur goes down |
| Walking, medicine gun | Dominant trigger | Fire at a sick dinosaur; cures its disease after the game's dart estimate (a weak buzz means nothing to treat) |
| Walking, radio | Dominant trigger on a vehicle | Take control of it |
| Walking, radio | Dominant trigger on a dinosaur or building | Send the nearest ranger team (hold off-hand grip: ACU helicopter) |
| Walking, radio | Dominant A | Call the nearest free jeep to where you stand |
| Jeep seat | Right / left trigger | Gas / brake (as the game's pad layout) |
| Helicopter seat | Right / left trigger | Climb / descend (right trigger also takes off) |
| Jeep / helicopter seat | Left stick | Steer |
| Jeep / helicopter seat | Both grips, turn your hands | Steering wheel |
| Jeep / helicopter seat | Head | Look around |
| Jeep / helicopter seat | Off-hand Y | The game's Aim Mode: into the gun seat (pad Y in JWE3; the helicopter only once it is in the air) |
| Aviary drone | Left stick, off-hand Y | Fly; once airborne, Aim Mode (then as the gun seat: hand aims, trigger uses the equipment, off-hand stick left / right picks it) |
| Jeep / helicopter seat, gun seat | Dominant B | The game's Back (pad B): gun seat to driver's seat, then out of the vehicle |
| Jeep / helicopter seat | Off-hand grip alone | Site map (pad LB); not while both grips hold the wheel |
| Jeep / helicopter / drone seat, gun seat | Dominant stick click | Photo mode, and back (the jeep: the game's pad d-pad right; helicopter and drone: their own photo switch, which no pad button reaches) |
| Photo mode | Dominant A | Take the photo (pad A); the game then shows the photo's details to submit or discard |
| Photo mode | Dominant B | Back (pad B) |
| Gun seat | Dominant hand | Aim (the gun follows it; the dot shows where the gun points, which is where the dart goes) |
| Gun seat | Dominant trigger | Fire / use the equipment (sent as the left mouse button, which is what JWE3's gun seat listens to; haptic kick) |
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
| Camera choke point | `CameraModeManager.Advance` -> `SetTransform(main camera, tFinal.transformQ)`, `SetFOVY(nCameraFOV / nMagnify)`; `GetConfig().bCameraShake`; walking eye height 1.67 m (metres). The world API's `CameraModeManager` is the camera component itself, and it writes the main camera after the park managers every frame, so a write from the VR manager reaches only the FOV | One camera path: the rig runs right after the camera manager's `Advance` (a patch of the module, or a wrapper on the instance made before the patch), with the VR manager as the fallback; camera shake off; new modes mapped: Freelook and MapGenerator (overview), NestExit (flat screen) |
| Lua states | A park shares the startup Lua state; `api.*` tables are read-only ("Scripts may NOT modify the API") | Laser picking wraps `Helpers.RaycastUtils.GetCursorRaycast` instead of `api.input.GetMouseRay` |
| Vehicle entry | Same `RequestChangeUI` / `DriveVehicle` call; JWE2's `_SetCameraMode` is gone; `StartInFirstPerson` init flag is new | Drive-mode patch sets `StartInFirstPerson` |
| Gunner aim and ammo | `GetCameraAimDirection`, `GetCameraAttachTransform`, `VehicleGunnerUIMode:_SwitchAmmo` unchanged | The gunner and vehicle IDs come from the gun seat UI mode itself (`nGunnerEntityID`, `nVehicleEntityID`) |
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

None of this has been seen in a worn headset yet. Remote runs without the headset have seen the rig on the
rendered frame and the log lines of steps 2, 4, 5, 6, 6a and 8-12, the controller steps with scripted controller
input (`JWE3VR_vr.txt`), not real controllers: laser on the HUD panel and the ground, take control, drag and turn
the park, recentre, tablet, pause menu, photo mode, gun seat aim, steering wheel, walking, snap turn, tool cycle
and the hold-B exit. Not seen anywhere: a shot fired, darts or medicine on a dinosaur (the test park has none),
haptics, and anything about how it looks or feels in the headset (two-hand scale was checked in the stereo eye frames). Go through it in order; each step names the line in
`Win64\ovldata\JWE3VR\JWE3VR.log` that says it works. When a step fails, note which one and keep the log.

1. **Main menu:** the menu is a large flat screen in front of you and the game window keeps working. Log:
   "swapchains 1920x1080 format 24" and, every 10 s, "frames: ... OpenXR state 5" (5 = focused; 3 means the
   headset is not being worn or the runtime is not showing the game).
2. **Park, head tracking:** load the Sandbox park. You look down on the park as a 1:30 model. Turning and leaning
   move the view one to one, with no lag or swimming and the horizon level. Log: "camera: driven from the camera
   manager's own Advance" and "camera tick: mode Standard, VR override 1.0".
3. **Stereo depth:** close one eye, then the other: the two views must differ, and near things must look near. If
   the depth looks inside-out, doubled or a frame late, try `AERPresentLag` 0, 1 or 2, or flip `SwapEyes` (the ini
   reloads live).
4. **In-world HUD:** the game's toolbar and bottom bar float on a panel a little below eye level and follow when
   you turn away; nothing of the UI is painted flat on the eyes. Log: "the game draws its UI into texture",
   "the UI now goes to the panel", "in-world HUD active".
5. **Controllers:** a dot shows where the dominant laser hits the ground; the trigger selects; one grip drags the
   park, both grips scale and rotate it. Point at the HUD panel: the cursor follows and the trigger clicks a toolbar
   button (log: "laser: pointing at the in-world HUD panel"). Seen on scripted input: one pull on the FIRST PERSON
   VIEW button opened the drop-a-guest mode, and the trigger on the ground dropped the guest there; one grip
   pulled toward you dragged the park closer, and both grips turned it.
6. **Tablet and recentre:** off-hand B/Y brings up the game screen with its HUD in front of you and hides it again;
   clicking both thumbsticks recentres.
6a. **Pause menu:** the menu button pauses; the game's options menu appears on the HUD panel (not on your eyes)
   and the laser clicks RESUME.
7. **Frame rate:** the "frames: ... fps" line should be near the headset's refresh rate (72, 80 or 90).
8. **Jeep:** point at a ranger jeep and press dominant A (or use the response facility's DIRECT CONTROL). You sit
   in the driver's seat at the game's own seat camera and look around with your head; the triggers drive, the left
   stick steers, both grips make a steering wheel. Log: "camera mode FirstPersonVehicle (rig kind 3".
9. **Gun seat:** off-hand Y. You sit at the gun mount with the gunner's own body hidden; the gun follows your hand
   and the trigger fires. Log: "character model ... hidden in the gun seat", "gun aim: hand vs gun yaw ..., pitch
   ..." (the error should shrink as the gun turns to your hand), and on a shot "the game reports a shot" and
   "weapon fired". The capture helicopter has the tranquilliser; the ranger jeep's gunner uses binoculars, which
   need something to scan. Dominant B goes back to the driver's seat ("shown again").
10. **Photo mode:** in the jeep's driver's seat, click the dominant stick; dominant A takes the photo, then the game shows
    its details to submit or discard. Log: "ui mode photo in". Check that the photo shows what you were looking at.
    The same click works in the helicopter and the aviary drone (log: "photo: asked the game for photo mode"), and
    clicking again there goes back.
11. **Helicopter:** take control of the capture helicopter. You are put in its seat (log: "asked for the
    first-person camera on vehicle ... try 1"), the right trigger lifts off, and off-hand Y in the air goes to its
    gun seat.
12. **Walking:** in the park, the left toolbar's FIRST PERSON VIEW button (a person with a camera), then a spot on
    the ground. You stand there at your real height; the left stick walks where you look, the dominant stick turns.
    Hold dominant B to leave. Log: "camera mode Guest (rig kind 2".

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
- **Headset shows nothing from the game (the runtime's own void or loading screen) although the log says "OpenXR
  state 5":** look for "OpenXR xrEndFrame failed" (logged the first 5 times, then every 1000th with a count). The
  layers the mod hands the headset - eyes, flat screen, HUD panel, laser dot - are only sent once the headset is
  worn (state 5), so they have never been sent in JWE3; the copies into them run, and were seen working, without it.
- **3D looks wrong (inside-out, doubled, one eye a frame late):** set `AERPresentLag` to 0, 1 or 2, or flip `SwapEyes`.
- **Camera does not follow the head:** the Lua side reports into `JWE3VR.log` (lines starting `lua:`). Expect
  "hooks installed", "VR manager Init (park loaded)", "gameplay init done", then "camera tick: mode Standard,
  VR override 1.0" and "camera: driven from the camera manager's own Advance" (logged early, marked "before the
  bridge loaded"). Without that last line the game's
  camera overwrites the rig every frame and only the FOV changes. "bridge: unexpected Lua stack layout" means a game
  update changed the Lua layout.
- **Vehicles ignore the controllers:** JWE3 adopts a pad only after it sees a keystroke from it; look for "virtual
  pad: announced on XInput slot 0" and, in `JWE3VR_shot.req` lines, XInputGetState calls on slot 0.
- **Remote testing:** `JWE3VR_shot.req` in the mod folder saves the next frame as `JWE3VR_shot.bmp`;
  `JWE3VR_vr.txt` plays VR controller input (buttons, triggers, grips, sticks, `AIM` = the hands point where the
  headset looks, `AIM=HUD:u,v` = at a point of the in-world HUD panel, `AIM=VIEW:u,v` = through a point of the
  rendered view, to aim at what a screenshot shows; `L=x,y,z` / `R=x,y,z` = a hand at an offset from the head,
  for grabs; format in `xr.cpp`), so the mod's own
  controller mapping runs with no one wearing the headset;
  `re/remote/vrstep.sh <name> [lines...]` plays one and takes a screenshot.
  `JWE3VR_pad.txt` plays clicks (`CLICK=x,y`), the VR laser's own click path (`LASER=x,y`), `FOCUS`, keys and pad
  buttons (format in `input.cpp`). The game reads no XInput on its menus and ignores posted keys; clicks register
  only while it has focus (add `FOCUS`), and only once the UI has seen the cursor there (send a click twice).
  `re/remote/to_continue.sh` drives a full run into the last Sandbox save, `re/remote/to_menu.sh` stops at the main
  menu, and `re/remote/vehicle.sh <name> jeep|heli [x,y]` takes direct control of a response facility's jeep or
  helicopter (switching the mod off live while it clicks through the menus, so the UI is in the screenshots; the
  facility is looked for in the view or given as x,y, `HERE=1` uses the park the game is already in),
  `re/remote/guest.sh <name> [x,y]` starts the guest view (walking) at a spot in the view, and
  `re/remote/reload.sh <name>` leaves the park to the main menu and loads it again. `JWE3VR_hud.req` saves the HUD texture
  (`JWE3VR_hud.bmp`; `re/remote/hudcheck.sh` puts it next to the frame), and `JWE3VR_rt.req` logs every render
  target of one frame and saves the large ones as `JWE3VR_rt<n>.bmp` (finding where something is drawn).
  `JWE3VR_cbscan.req` searches the game's large read-write memory for the camera position (constant-buffer hunting).
  `JWE3VR_aer.req` saves the next 8 frames as `JWE3VR_aer<n>_eye<e>.bmp` with the eye each is shown to (with a large
  `IPDScale` the eyes' frames differ visibly: a frame for the left eye shows the scene shifted right).
  `JWE3VR_drive.req` ("jeep" or "heli") takes control of the team vehicle nearest the camera, found as the radio
  finds one; `re/remote/drive.sh <name> jeep|heli` uses it to reach a seat with no clicks. The same file with
  "paths" logs each building's path connections and path join, "aimdino" turns the on-foot tools' and gun seats'
  aim at the nearest dinosaur on or off (scripted hands cannot follow an animal; a gun seat stays on the first one), and "sick" gives the nearest dinosaur a disease
  (one the game treats with darts) for the medicine gun. Both log the nearest dinosaur's distance and bearing from
  the camera (to steer a vehicle to it remotely). Shots and projectile impacts are logged.
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
| `native/JWE3VR/src/input.*` | Virtual Xbox pad (XInput), laser -> the game's cursor and clicks (posted window messages, no OS input) |
| `native/JWE3VR/src/bridge.*` | `jwe3vr_lua`: numbers in/out on the game's Lua stack, no Lua linked, layout self-check |
| `native/loader` | `version.dll` proxy that loads the mod at game start |
| `native/JWE3VR/test` | Offline checks (rig math; the bridge against a fake `lua_State`) |
| `pack/JWE3VR` | Content pack: own module hooks, camera wrap, gameplay, on-foot ranger, staff wand |
| `re/tools` | `survey.py` (Phase 0 binary survey), strings / LEA scan / Lua binding map / refs / disassembly, Lua decompile, `luagrep.py` (search extracted Lua) |
| `re/remote` | Remote test runs without a person at the PC: deploy, launch, click into a park, vehicles (`vehicle.sh`, `drive.sh`), walking (`guest.sh`), park reload (`reload.sh`), HUD and frame screenshots, logs |
| `re/ghidra.ps1`, `re/ghidra_scripts` | Headless Ghidra: import with the Denuvo blob removed, decompile `re/targets.txt` |
| `third_party/fetch.ps1` | Fetches MinHook, the OpenXR loader SDK and the Lua 5.3 source |

## Legal

The EULA forbids reverse engineering and mods; Frontier has tolerated script mods for years. This mod ships
only its own code: no game files and no patched executables. Extracted game Lua, decompiles and the Ghidra
database stay on the developer's machine (see `.gitignore`).
