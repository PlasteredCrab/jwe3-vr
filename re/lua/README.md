# JWE3 game Lua (extracted locally, never committed)

Do not redistribute: these are Frontier's game scripts, kept here for reference only. Everything in
this folder except this README is ignored by git.

**Status: not extracted yet.** JWE3 was not installed on the dev machine on 2026-10-26.

## Setup (once)

cobra-tools pins Python 3.11 (`requires-python = "==3.11.*"` in its pyproject):

```bash
git clone https://github.com/OpenNaja/cobra-tools /e/Modding/tools/cobra-tools
cd /e/Modding/tools/cobra-tools
py -3.11 -m venv .venv
.venv/Scripts/python -m pip install imageio "numpy<2" pillow "bitarray~=2.9.2"
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

## What to confirm (Phase 0)

| Question | Where | JWE2 answer |
| --- | --- | --- |
| Does the game call `AddContentToCall` on each pack's `Database.*LuaDatabase`? | `database.main`, `database.gamedatabase` | yes |
| Is `Environments.ParkEnvironment.EnvironmentPrototype.Managers` the manager list? | `environments.parkenvironment` | yes (ACSE relies on it) |
| Are the park modules loaded after content-pack databases run? | `database.main` load order | unknown; ACSE hooked earlier, from `Init.ovl` |
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
