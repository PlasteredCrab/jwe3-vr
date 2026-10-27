# JWE3 game Lua (extracted locally, never committed)

Do not redistribute: these are Frontier's game scripts, kept here for reference only. Everything in
this folder except this README is ignored by git.

**Status (2026-10-26, JWE3.exe build 1792088119):** extracted `Content0` (14,595 modules), `Content0_Init` (209),
`GameMain` (236); 70 modules decompiled into `decompiled/`. Findings are in the main README's Phase 0 checklist.

## Setup (once)

cobra-tools pins Python 3.11 (`requires-python = "==3.11.*"`; it imports `typing.LiteralString`). The dev
machine has only 3.10 installed, so a portable 3.11 from the official NuGet package lives in
`E:\Modding\tools\python311`:

```bash
cd /e/Modding/tools && curl -sSL -o py311.zip https://www.nuget.org/api/v2/package/python/3.11.9
```

Unzip its `tools/` folder to `E:\Modding\tools\python311`, then:

```bash
git clone --depth 1 https://github.com/OpenNaja/cobra-tools /e/Modding/tools/cobra-tools
cd /e/Modding/tools/cobra-tools
../python311/python.exe -m venv .venv
.venv/Scripts/python -m pip install imageio "numpy<2" pillow "bitarray~=2.9.2"
```

The binary RE scripts in `re/tools` use a second venv on the same Python, with pefile and capstone (pip's
cache also kept on E:):

```bash
cd /e/Modding/tools && PIP_CACHE_DIR=E:/Modding/tools/pip-cache python311/python.exe -m venv re-venv && re-venv/Scripts/python -m pip install pefile capstone
```

Find which modules mention a name (constants, fields, function names), about 12 s over everything:

```bash
/e/Modding/tools/re-venv/Scripts/python -E re/tools/luagrep.py re/lua/Content0 re/lua/GameMain -- FireFlare TranquilliseDinosaur
```

## Extract and decompile

```bash
cd /e/Modding/tools/cobra-tools
.venv/Scripts/python ovl_tool_cmd.py extract "<game>/Win64/ovldata/Content0/Main.ovl" -o "<project>/re/lua/Content0" -g "Jurassic World Evolution 3" --type .lua
.venv/Scripts/python "<project>/re/tools/decompile_lua.py" "<project>/re/lua/Content0" "<project>/re/lua/decompiled" \
    "^components\.cameras\." "^editors\.vehicle\." "^vehicletasks\." "^environments\." "^database\." "^helpers\.raycastutils"
```

JWE3 may split game Lua across more packs than JWE2's `Content0`; `re/tools/survey.py` lists the
content packs. Search every `Main.ovl` if a module is missing.

## What was confirmed (Phase 0)

The JWE2 answers below were all re-checked on JWE3; see the main README for the JWE3 results. The one that
differs in an important way: `environment.environment` saves `global.require` when the game starts and loads
`Environments.ParkEnvironment` through it, after content-pack databases have run (GameDatabase init is at
startup, park worlds come later). `jwe3vr.hooks.lua` therefore wraps `Environment._MergePrototype`.

| Question | Where | JWE2 answer |
| --- | --- | --- |
| Does the game call `AddContentToCall` on each pack's `Database.*LuaDatabase`? | `database.main`, `database.gamedatabase` | yes |
| Is `Environments.ParkEnvironment.EnvironmentPrototype.Managers` the manager list? | `environments.parkenvironment` | yes (ACSE relies on it) |
| Are the park modules loaded after content-pack databases run? | `game.basegame`, `environment.environment` | JWE3: yes, but through a `require` saved before ours |
| Camera choke point | `components.cameras.cameramodemanager` `Advance` | `SetTransform(mainCam, tFinal.transformQ)` + `SetFOVY` |
| Mode names | `CameraModeManager.tModeAPIs` | Debug, Focus, Orbit, Standard, FirstPersonVehicle, ThirdPersonVehicle, Gunner, MatchTarget, Animated, Map, Photo, Fixed, FirstPerson, Capture, VehicleLockOn, HatcheryExit, Guest, Panoramic, Dolly, Path |
| Shake switch | `components.cameras.cameralogic` | config `bCameraShake` |
| Walking eye height (units) | `components.cameras.guestcamera` | 1.67 (metres) |
| Vehicle entry | `editors.vehicle.inspectvehicleuimode` | `iUIMode:OnModeEvent("RequestChangeUI","DriveVehicle",{Vehicle=v})` |
| Gunner aim / ammo | `editors.vehicle.vehiclegunneruimode` | `GetCameraAimDirection`, `_SwitchAmmo` |
| Staff orders | `editors.vehicle.selectvehicledestinationuimode` | `VehicleTaskQueue:InsertTask` |
| Cursor ray | `helpers.raycastutils` | `api.input.GetMouseRay` |

## Camera pipeline on JWE2 (for comparison)

`components.cameras.cameramodemanager` — `CameraModeManager.Advance(self, dt, unscaledDt)` runs the active
mode's `Advance`, then for the main camera:

```lua
local transformQ = tData.tModeData.tFinal.transformQ
api.transform.SetTransform(api.camera.GetMainCameraID(), transformQ)
if tData.nFOVY ~= tFinal.nCameraFOV then api.camera.SetFOVY(targetID, nFOVY) end
```

All modes but Debug and MatchTarget build `tFinal.transformQ` in `CameraLogic.ApplyFinal`:
`baseTransQ:ToWorld(TransformQ.FromOrPos(YPR + vOffsetYPR + shake, vCameraPos + vOffsetPosition))`.
Shake is added only when the camera config's `bCameraShake` is true. Default FOV `CameraUtils.nDefaultFOV = math.pi / 3`.
