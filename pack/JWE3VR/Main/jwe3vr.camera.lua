-----------------------------------------------------------------------
--/  @file   JWE3VR.Camera.lua
--/  @brief  Wraps CameraModeManager.Advance. The game's active camera mode computes its transform
--/          as usual; afterwards that transform becomes the VR anchor and the main camera is set to
--/          anchor x predicted head (or one eye, in alternate-eye stereo) with the culling FOV.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local pcall = global.pcall
local tostring = global.tostring
local require = global.require

local Vector3 = require("Vector3")
local Quaternion = require("Quaternion")
local TransformQ = require("TransformQ")
local Bridge = require("JWE3VR.Bridge")

local Camera = module(...)

local K = Bridge.Kind

-- Game camera mode -> how the VR rig treats it (JWE3 CameraModeManager tModeAPIs, build 1792088119).
-- Cinematic and scripted cameras go to the flat screen. "SceneryPlacement" is requested as Standard.
Camera.tModeKinds = {
    Standard = K.Overview,
    Freelook = K.Overview, -- JWE3: the second default park camera (toggled with GameCamera_FreelookMode)
    MapGenerator = K.Overview, -- JWE3: sandbox island generator
    Focus = K.Overview,
    Orbit = K.Overview,
    Guest = K.Ground,
    FirstPersonVehicle = K.Seat,
    ThirdPersonVehicle = K.Seat,
    VehicleLockOn = K.Seat,
    FirstPerson = K.Seat,
    Capture = K.Seat,
    Gunner = K.Gunner,
    Photo = K.Gunner,
    Map = K.Screen,
    Animated = K.Screen,
    Path = K.Screen,
    Dolly = K.Screen,
    Fixed = K.Screen,
    HatcheryExit = K.Screen,
    NestExit = K.Screen, -- JWE3: hatchling nest exit cinematic
    Panoramic = K.Screen,
    MatchTarget = K.Screen,
    Debug = K.Off,
}

Camera.sMode = nil
Camera.nKind = K.Off
Camera.bForceScreen = false -- set by gameplay (e.g. a menu that needs the flat screen)

Camera.KindFor = function(sMode)
    if Camera.bForceScreen then return K.Screen end
    return Camera.tModeKinds[sMode or ""] or K.Screen
end

-- One camera path (seen in game: which wrapper the game reaches depends on load order, and earlier mode / module
-- paths with their own logic had fallen behind): the rig runs right after the camera manager's own Advance - through
-- a patch of the CameraModeManager module when the instance comes from it, else by wrapping the instance the world
-- API hands out - and, if neither ticks, from the VR manager's Advance (its write does not reach the frame then:
-- the camera manager rewrites the camera after it; see Camera.TryWrapInstance).
Camera.nInstanceTicks, Camera.nInstanceSeen = 0, 0

Camera.OnManagerAdvance = function(nDt, sVia)
    Camera.nInstanceTicks = Camera.nInstanceTicks + 1
    if Camera.sDrivenVia ~= sVia then
        Camera.sDrivenVia = sVia
        Bridge.Text("camera: driven from the camera manager's own Advance (" .. sVia .. ", right after the game's write)")
    end
    local bOk, sErr = pcall(Camera.ManagerTick, nDt, true)
    if not bOk and not Camera.bInstanceError then
        Camera.bInstanceError = true
        Bridge.Text("camera (" .. sVia .. ") failed: " .. tostring(sErr))
    end
end

Camera.Patch = function(tModule)
    if global.rawget(tModule, "__JWE3VRPatched") then return end
    global.rawset(tModule, "__JWE3VRPatched", true)
    local fnAdvance = tModule.Advance
    if global.type(fnAdvance) ~= "function" then return end
    Camera.fnModuleOriginal = fnAdvance
    Camera.fnModuleAdvance = function(self, nDt, nUnscaledDt, ...)
        local a, b, c = fnAdvance(self, nDt, nUnscaledDt, ...)
        Camera.OnManagerAdvance(nUnscaledDt or nDt, "module patch")
        return a, b, c
    end
    tModule.Advance = Camera.fnModuleAdvance
    Bridge.Text("camera manager module patched")
end

local SameTransform = function(a, b)
    if not a or not b then return false end
    local pa, pb = a:GetPos(), b:GetPos()
    local fa, fb = a:GetF(), b:GetF()
    local d = global.math.abs
    -- 1 mm / 0.001: loose enough for the engine's quaternion round trip, far below any real camera move.
    return d(pa:GetX() - pb:GetX()) + d(pa:GetY() - pb:GetY()) + d(pa:GetZ() - pb:GetZ()) < 1e-3 and
           d(fa:GetX() - fb:GetX()) + d(fa:GetY() - fb:GetY()) + d(fa:GetZ() - fb:GetZ()) < 1e-3
end

-- JWE3VR.Gameplay requires this module, so it is required here only when first needed. (A lookup in
-- package.loaded by its written name found nothing in game: the keys are stored differently.)
local tGameplay = nil
Camera.GetGameplay = function()
    if not tGameplay then
        local bOk, t = pcall(require, "JWE3VR.Gameplay")
        if bOk and global.type(t) == "table" then tGameplay = t end
    end
    return tGameplay
end

-- The vehicle the camera follows: from the camera mode's init table, else from the active UI mode.
local CameraVehicle = function(cmm, Gameplay)
    if cmm and cmm.GetCurrentModeInit then
        local bOk, tInit = pcall(cmm.GetCurrentModeInit, cmm)
        if bOk and global.type(tInit) == "table" and tInit.nVehicle then return tInit.nVehicle end
    end
    return Gameplay and Gameplay.ActiveVehicle and Gameplay.ActiveVehicle() or nil
end

-- A first-person seat camera of the vehicle (driving, else riding - the cinematic camera rides any vehicle that
-- way): its usage flag and a name, or nil. Cached per vehicle.
Camera.tSeats = {}
Camera.SeatCamera = function(vehicles, nVehicle)
    local tSeat = Camera.tSeats[nVehicle]
    if not tSeat then
        tSeat = {}
        for _, sUsage in global.ipairs({"Driving", "Riding"}) do
            local nUsage = vehicles and vehicles["FirstPersonCameraUsage_" .. sUsage]
            if nUsage and not tSeat.nUsage then
                local bOk, n = pcall(vehicles.GetNumFirstPersonCameras, vehicles, nVehicle, nUsage)
                if bOk and global.type(n) == "number" and n >= 1 then
                    tSeat.nUsage, tSeat.sWhich = nUsage, global.string.lower(sUsage) .. " seat camera"
                end
            end
        end
        Camera.tSeats[nVehicle] = tSeat
        Bridge.Text("vehicle " .. tostring(nVehicle) .. ": " .. (tSeat.sWhich or "no seat camera"))
    end
    return tSeat.nUsage, tSeat.sWhich
end

-- JWE3 shows helicopters only from behind (VehicleUtils.CanSwitchCamera) - a chase camera is no place for a VR
-- view. If the helicopter has a seat camera, ask the camera manager for the first-person mode on it (what the jeep
-- gets from StartInFirstPerson). Helicopters only: cars switch camera themselves, and a player who picks the
-- chase view or a lock-on there keeps it. A few tries per stay in the chase camera (reset once seated).
Camera.tSeatRequests = {}
Camera.RequestSeatCamera = function(cmm, tWorld, Gameplay)
    local nVehicle = CameraVehicle(cmm, Gameplay)
    if not nVehicle or nVehicle == 0 or not cmm or not cmm.RequestMode then return nil end
    local vehicles = tWorld and tWorld.vehicles
    if not vehicles or not vehicles.GetVehicleType then return nil end
    local bOkType, nType = pcall(vehicles.GetVehicleType, vehicles, nVehicle)
    if not bOkType or nType == nil or nType ~= vehicles.VehicleType_Helicopter then return nil end
    local nUsage, sWhich = Camera.SeatCamera(vehicles, nVehicle)
    if not nUsage then return nil end
    local tReq = Camera.tSeatRequests[nVehicle] or {nTries = 0, nWait = 0}
    Camera.tSeatRequests[nVehicle] = tReq
    tReq.nWait = tReq.nWait - 1
    if tReq.nTries >= 5 or tReq.nWait > 0 then return sWhich end
    tReq.nTries, tReq.nWait = tReq.nTries + 1, 30 -- ticks between tries
    local bOk, sErr = pcall(cmm.RequestMode, cmm, "FirstPersonVehicle", {nVehicle = nVehicle, nCameraIndex = 1, nUsageFlags = nUsage}, true)
    Bridge.Text("asked for the first-person camera on vehicle " .. tostring(nVehicle) .. " (" .. sWhich .. ", try " ..
                tostring(tReq.nTries) .. (bOk and ")" or ("): " .. tostring(sErr))))
    return sWhich
end

-- JWE3: the world API's CameraModeManager is the camera component instance itself (its tEntities are readable),
-- and it writes the main camera every frame after the VR manager, so a write from the manager never reaches the
-- frame - only the FOV does, because the game re-sends its FOV only when it changes (seen in game: IPDScale 40
-- moved neither eye). The module patch (Camera.Patch) does not reach it, the instance was made before; wrapping
-- the instance's own Advance runs the rig right after the game's write.
-- Per instance (a new park makes a new one); the manager path takes over whenever no camera-manager tick came
-- since its last tick.
Camera.TryWrapInstance = function(cmm)
    if global.type(cmm) ~= "table" or Camera.oWrapped == cmm then
        if not Camera.bWrapSkipLogged then
            Camera.bWrapSkipLogged = true
            Bridge.Text("camera: no instance to wrap (" .. global.type(cmm) .. ", same as before: " .. tostring(Camera.oWrapped == cmm) .. ")")
        end
        return
    end
    Camera.oWrapped = cmm
    if global.rawget(cmm, "__JWE3VRWrapped") then
        Bridge.Text("camera: instance already wrapped")
        return
    end
    local fnAdvance = cmm.Advance
    if global.type(fnAdvance) ~= "function" then
        Bridge.Text("camera: the camera manager instance has no Advance to wrap")
        return
    end
    global.rawset(cmm, "__JWE3VRWrapped", true)
    -- Wrapped around the game's own function even when the instance resolves to the patched module one, so the rig
    -- ticks once per frame whichever of the two the game calls. (Run 72: the module patch drove it, 1889 ticks;
    -- run 70's "no module-patch tick" was its first log line being dropped before the bridge loaded.)
    local bModule = fnAdvance == Camera.fnModuleAdvance
    if bModule then fnAdvance = Camera.fnModuleOriginal end
    global.rawset(cmm, "Advance", function(self, nDt, nUnscaledDt, ...)
        local a, b, c = fnAdvance(self, nDt, nUnscaledDt, ...)
        Camera.OnManagerAdvance(nUnscaledDt or nDt, "instance wrapper")
        return a, b, c
    end)
    Bridge.Text("camera: wrapped the camera manager instance's Advance" .. (bModule and " (over the module patch)" or ""))
end

-- The gunner's character sits at the gun mount, right where the VR eye is (seen in game, run 78: their head and cap
-- filled the view). Its model is "./CharacterPivot/Character" under the gunner entity (basegunner prefab; the gunner
-- entity itself is not in the scene graph, run 79). Hidden through the scene graph while the headset view is in the
-- gun seat (as the game hides dinosaurs in storage), shown again otherwise: the flat screen, the mod switched off and
-- every other seat keep the game's own look. Aiming and firing go through the gunner entity, not the model.
-- Tried once per gun-seat stay, hidden or not, so a failure is logged once, not every frame.
Camera.SetGunnerHidden = function(tWorld, nGunner)
    if Camera.nHiddenGunner == nGunner then return end
    local scenegraph = tWorld and tWorld.scenegraph
    if not scenegraph or not scenegraph.SetHidden then return end
    -- Handled in another park's scene graph: that entity is gone, and its id may name something else here.
    if Camera.oHiddenIn ~= scenegraph then Camera.nHiddenGunner, Camera.nHiddenModel = nil, nil end
    if Camera.nHiddenGunner == nGunner then return end
    if Camera.nHiddenModel then
        pcall(scenegraph.SetHidden, scenegraph, Camera.nHiddenModel, false) -- fails harmlessly if it is gone
        Bridge.Text("gunner model " .. tostring(Camera.nHiddenModel) .. " shown again")
    end
    Camera.nHiddenGunner, Camera.nHiddenModel, Camera.oHiddenIn = nGunner, nil, scenegraph
    if not nGunner then return end
    local bFound, nModel = pcall(api.entity.FindEntityByName, "./CharacterPivot/Character", nGunner)
    if not bFound or global.type(nModel) ~= "number" or nModel <= 0 then
        Bridge.Text("gunner " .. tostring(nGunner) .. ": no character model found (" .. tostring(nModel) .. "); left visible")
        return
    end
    local bOk, sErr = pcall(scenegraph.SetHidden, scenegraph, nModel, true)
    if bOk then Camera.nHiddenModel = nModel end
    Bridge.Text("gunner " .. tostring(nGunner) .. ": character model " .. tostring(nModel) ..
                (bOk and " hidden in the gun seat" or (" not hidden: " .. tostring(sErr))))
end

-- The camera manager sends its FOV only when it changes (each camera entity's tData.nFOVY holds the last one sent),
-- so the headset FOV the rig wrote stayed on the flat screen after the VR view stopped (seen in game: the mod
-- switched off live in the guest view kept the wide FOV). Resetting that cache, as the manager's own init does,
-- makes it send its FOV again the next frame.
Camera.RestoreGameFov = function(cmm)
    local tEntities = cmm and cmm.tEntities
    if global.type(tEntities) ~= "table" then return end
    for _, tData in global.pairs(tEntities) do
        if global.type(tData) == "table" then tData.nFOVY = -1 end
    end
    Bridge.Text("camera: the game's own FOV sent again (VR view off)")
end

Camera.ManagerTick = function(nDt, bFromInstance)
    if not Bridge.IsLoaded() then return end
    if not bFromInstance and Camera.nInstanceTicks ~= Camera.nInstanceSeen then
        Camera.nInstanceSeen = Camera.nInstanceTicks -- the camera manager's Advance drives it
        return
    end
    local camID = api.camera.GetMainCameraID()
    if not camID or camID <= 0 then return end
    local current = api.transform.GetTransform(camID)
    if not current then return end
    if SameTransform(current, Camera.lastSet) then
        Camera.nOwnReads = (Camera.nOwnReads or 0) + 1 -- the game did not write the camera since our last write
    else
        Camera.lastAnchor = current
    end
    Camera.nTicks = (Camera.nTicks or 0) + 1
    local anchor = Camera.lastAnchor
    if not anchor then return end
    if not bFromInstance and not Camera.bManagerPathLogged then
        Camera.bManagerPathLogged = true
        Bridge.Text("camera: driven from the VR manager (no camera-manager tick yet)")
    end
    -- Camera mode: the player-facing UI mode first, else what the camera manager reports through the world
    -- API, else Standard.
    local Gameplay = Camera.GetGameplay()
    local sTag = Gameplay and Gameplay.sUIMode
    local bOk, tWorld = pcall(api.world.GetWorldAPIs)
    if not bOk or global.type(tWorld) ~= "table" then tWorld = nil end
    local cmm = tWorld and tWorld.CameraModeManager
    if not bFromInstance then Camera.TryWrapInstance(cmm) end
    -- No camera shake in VR (the gameplay layer turns shakes into haptics instead); once per camera manager.
    if cmm and cmm.GetConfig and Camera.oShakeOff ~= cmm then
        Camera.oShakeOff = cmm
        local bOkC, tConfig = pcall(cmm.GetConfig, cmm)
        if bOkC and global.type(tConfig) == "table" then tConfig.bCameraShake = false end
    end
    local sGameMode = nil
    if cmm and cmm.GetCurrentModeName then
        local bOk2, s = pcall(cmm.GetCurrentModeName, cmm)
        if bOk2 and global.type(s) == "string" then sGameMode = s end
    end
    local sMode = (sTag and Gameplay.tUIModeCamera[sTag]) or (sGameMode and Camera.tModeKinds[sGameMode] and sGameMode) or
                  "Standard"
    -- Entering a vehicle, the game camera follows the UI mode a few frames later: keep the park rig until it has
    -- left the park camera (else the 1:1 seat rig sits at the overview camera for a moment).
    if sTag and sGameMode and Camera.tModeKinds[sGameMode] == K.Overview then sMode = sGameMode end
    local nKind = Camera.KindFor(sMode)
    local vPos, vR, vU, vF = anchor:GetPos(), anchor:GetR(), anchor:GetU(), anchor:GetF()
    local sSeat, nSeatGunner = nil, nil
    if sTag == "drive" and sGameMode == "ThirdPersonVehicle" then
        sSeat = Camera.RequestSeatCamera(cmm, tWorld, Gameplay)
    elseif sGameMode == "FirstPersonVehicle" then
        Camera.tSeatRequests = {} -- seated: a later fall back to the chase camera gets fresh tries
    end
    if nKind == K.Gunner then
        -- Sit at the gun mount, oriented like the vehicle body (the turret must not swing the world).
        local nVehicle, nGunner = nil, nil
        if Gameplay then nVehicle, nGunner = Gameplay.GunnerIDs() end -- not "Gameplay and ...": that keeps one result
        local tWorldAPIs = api.world.GetWorldAPIs()
        local mount = nGunner and tWorldAPIs and tWorldAPIs.weapons and tWorldAPIs.weapons:GetCameraAttachTransform(nGunner)
        local body = nVehicle and api.transform.GetTransform(nVehicle)
        if mount and body then
            local vMount = mount:GetPos()
            if Camera.nGunMount ~= nGunner then
                Camera.nGunMount = nGunner
                local dx, dy, dz = vMount:GetX() - vPos:GetX(), vMount:GetY() - vPos:GetY(), vMount:GetZ() - vPos:GetZ()
                Bridge.Text("gun mount of gunner " .. tostring(nGunner) .. " is " ..
                            global.string.format("%.1f", global.math.sqrt(dx * dx + dy * dy + dz * dz)) .. " m from the game camera")
            end
            vPos, vR, vU, vF = vMount, body:GetR(), body:GetU(), body:GetF()
            nSeatGunner = nGunner
        else
            nKind = K.Seat -- no gun seat known: sit where the game camera is
            if Camera.nGunFallback ~= nGunner then
                Camera.nGunFallback = nGunner
                Bridge.Text("gun seat not found (vehicle " .. tostring(nVehicle) .. ", gunner " .. tostring(nGunner) .. ", mount " ..
                            global.type(mount) .. ", body " .. global.type(body) .. "); sitting at the game camera")
            end
        end
    end
    if sMode ~= Camera.sMode or nKind ~= Camera.nKind or sSeat ~= Camera.sSeat or sGameMode ~= Camera.sGameMode then
        Bridge.Log(Bridge.M.CameraKind, nKind)
        Bridge.Text("camera mode " .. sMode .. " (rig kind " .. tostring(nKind) .. ", ui mode " .. tostring(sTag) ..
                    ", game camera " .. tostring(sGameMode) .. (sSeat and (", asking for its " .. sSeat) or "") .. ")")
    end
    Camera.sMode, Camera.nKind, Camera.sSeat, Camera.sGameMode = sMode, nKind, sSeat, sGameMode
    local bOverride, px, py, pz, fx, fy, fz, ux, uy, uz, nFovY = Bridge.Tick(nKind, nDt or 0, vPos, vR, vU, vF)
    if Camera.nLoggedOverride ~= bOverride then
        Camera.nLoggedOverride = bOverride
        Bridge.Text("camera tick: mode " .. sMode .. ", VR override " .. tostring(bOverride))
    end
    Camera.SetGunnerHidden(tWorld, bOverride == 1 and nSeatGunner or nil)
    if bOverride ~= 1 then
        if Camera.lastSet then Camera.RestoreGameFov(cmm) end -- the rig wrote the camera until now
        Camera.lastSet = nil
        return
    end
    local transform = TransformQ.FromOrPos(Quaternion.FromFU(Vector3:new(fx, fy, fz), Vector3:new(ux, uy, uz)),
                                           Vector3:new(px, py, pz))
    api.transform.SetTransform(camID, transform)
    api.camera.SetFOVY(camID, nFovY)
    Camera.lastSet, Camera.nLastFov, Camera.nLastCam = transform, nFovY, camID
end
