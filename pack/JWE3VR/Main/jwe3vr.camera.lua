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
Camera.tModeData = nil
Camera.bForceScreen = false -- set by gameplay (e.g. a menu that needs the flat screen)

Camera.KindFor = function(sMode)
    if Camera.bForceScreen then return K.Screen end
    return Camera.tModeKinds[sMode or ""] or K.Screen
end

-- Camera mode modules (CameraModeManager tModeAPIs). On JWE3 the camera manager is created natively in the
-- park's own Lua VM before our sweep can patch it, and the engine keeps the original Advance; but the manager
-- calls each mode's Advance through the mode module every frame (tData.tModeAPI.Advance), so wrapping the
-- mode modules always works. The wrapper rewrites tFinal.transformQ / nCameraFOV, which the manager then
-- applies itself (SetTransform, SetFOVY(nCameraFOV / nMagnify)).
Camera.tModeModules = {
    Debug = "Components.Cameras.CombinedDebugCamera", Focus = "Components.Cameras.FocusCamera",
    Orbit = "Components.Cameras.OrbitCamera", Standard = "Components.Cameras.StandardCamera",
    FirstPersonVehicle = "Components.Cameras.FirstPersonVehicleCamera",
    ThirdPersonVehicle = "Components.Cameras.ThirdPersonVehicleCamera", Gunner = "Components.Cameras.GunnerCamera",
    MatchTarget = "Components.Cameras.MatchTargetCamera", Animated = "Components.Cameras.AnimatedCamera",
    Map = "Components.Cameras.MapCamera", Photo = "Components.Cameras.PhotoCamera", Fixed = "Components.Cameras.FixedCamera",
    FirstPerson = "Components.Cameras.FirstPersonCamera", Capture = "Components.Cameras.CaptureCamera",
    VehicleLockOn = "Components.Cameras.VehicleLockOnCamera", HatcheryExit = "Components.Cameras.HatcheryExitCamera",
    Guest = "Components.Cameras.GuestCamera", Panoramic = "Components.Cameras.PanoramicCamera",
    Dolly = "Components.Cameras.DollyCamera", Path = "Components.Cameras.PathCamera",
    Freelook = "Components.Cameras.FreelookCamera", NestExit = "Components.Cameras.NestExitCamera",
    MapGenerator = "Components.Cameras.MapGeneratorCamera",
}

Camera.bModeDriven = false

-- True when tModeData belongs to the main camera entity (each camera entity runs its mode's Advance).
local IsMainModeData = function(tModeData, tWorldAPIs)
    local cmm = tWorldAPIs and tWorldAPIs.CameraModeManager
    local tEntities = global.type(cmm) == "table" and global.rawget(cmm, "tEntities")
    if global.type(tEntities) ~= "table" then return true end -- cannot tell: assume the main camera
    local tData = tEntities[global.rawget(cmm, "mainEntityID")]
    return tData == nil or tData.tModeData == tModeData, tData and tData.sModeName
end

Camera.PatchMode = function(tModule, sMode)
    if global.type(tModule) ~= "table" or tModule.__JWE3VRPatched or global.type(tModule.Advance) ~= "function" then return end
    tModule.__JWE3VRPatched = true
    local fnAdvance = tModule.Advance
    tModule.Advance = function(tModeData, tWorldAPIs, _nUnscaledDeltaTime, ...)
        local r = {fnAdvance(tModeData, tWorldAPIs, _nUnscaledDeltaTime, ...)}
        Camera.nModeCalls = (Camera.nModeCalls or 0) + 1
        if Camera.nModeCalls == 1 then
            -- This may be a Lua VM where nothing has loaded the bridge yet.
            if not Bridge.IsLoaded() then pcall(Bridge.Load) end
            Bridge.Text("camera mode " .. sMode .. ": first Advance call in this VM")
        end
        local bOk, sErr = pcall(Camera.AfterModeAdvance, sMode, tModeData, tWorldAPIs, _nUnscaledDeltaTime)
        if not bOk and not Camera.bErrorLogged then
            Camera.bErrorLogged = true
            Bridge.Log(Bridge.M.LuaError, 3)
            Bridge.Text("camera mode " .. sMode .. " failed: " .. tostring(sErr))
        end
        return global.table.unpack(r)
    end
end

Camera.Patch = function(tModule)
    if tModule.__JWE3VRPatched then return end
    tModule.__JWE3VRPatched = true
    local fnAdvance = tModule.Advance
    tModule.Advance = function(self, _nDeltaTime, _nUnscaledDeltaTime)
        fnAdvance(self, _nDeltaTime, _nUnscaledDeltaTime)
        Camera.bManagerDriven = true
        if Camera.bModeDriven then return end -- the mode wrappers already drive the camera
        local bOk, sErr = pcall(Camera.AfterAdvance, self, _nUnscaledDeltaTime)
        if not bOk and not Camera.bErrorLogged then
            Camera.bErrorLogged = true
            api.debug.Trace("JWE3VR camera: " .. tostring(sErr))
            Bridge.Log(Bridge.M.LuaError, 3)
            Bridge.Text("camera failed: " .. tostring(sErr))
        end
    end
    api.debug.Trace("JWE3VR: camera manager patched")
    Bridge.Text("camera manager patched")
end

local GunMountTransform = function(tModeData)
    if not tModeData.gunnerID then return nil end
    local tWorldAPIs = api.world.GetWorldAPIs()
    if not tWorldAPIs or not tWorldAPIs.weapons then return nil end
    return tWorldAPIs.weapons:GetCameraAttachTransform(tModeData.gunnerID)
end

Camera.AfterAdvance = function(self, _nDt)
    if not Bridge.IsLoaded() then return end
    local tData = self.tEntities and self.tEntities[self.mainEntityID]
    if not tData or not tData.tModeData or not tData.tModeData.tFinal then return end
    local camID = api.camera.GetMainCameraID()
    if not camID or camID <= 0 then return end

    local sMode = tData.sModeName
    local nKind = Camera.KindFor(sMode)
    if sMode ~= Camera.sMode then Bridge.Log(Bridge.M.CameraKind, nKind) end
    Camera.sMode = sMode
    Camera.nKind = nKind
    Camera.tModeData = tData.tModeData

    -- No camera shake in VR (the gameplay layer turns shakes into haptics instead).
    local tConfig = self:GetConfig()
    if tConfig then tConfig.bCameraShake = false end

    local anchor = tData.tModeData.tFinal.transformQ
    if not anchor then return end
    local vPos, vR, vU, vF = anchor:GetPos(), anchor:GetR(), anchor:GetU(), anchor:GetF()
    if nKind == K.Gunner then
        -- The gunner camera looks along the gun; anchoring on it would swing the world with your aim.
        -- Sit at the gun mount, oriented like the vehicle body (which does not turn with the turret).
        local bOk, mount = pcall(GunMountTransform, tData.tModeData)
        if bOk and mount then vPos = mount:GetPos() end
        local bOk2, body = pcall(require("JWE3VR.Gameplay").GunVehicleTransform)
        if bOk2 and body then
            vR, vU, vF = body:GetR(), body:GetU(), body:GetF()
        elseif bOk and mount then
            vR, vU, vF = mount:GetR(), mount:GetU(), mount:GetF()
        end
    end

    local bOverride, px, py, pz, fx, fy, fz, ux, uy, uz, nFovY = Bridge.Tick(nKind, _nDt or 0, vPos, vR, vU, vF)
    if Camera.nLoggedOverride ~= bOverride then
        Camera.nLoggedOverride = bOverride
        Bridge.Text("camera tick: mode " .. tostring(sMode) .. ", rig kind " .. tostring(nKind) .. ", VR override " .. tostring(bOverride))
    end
    if bOverride ~= 1 then return end

    local vF = Vector3:new(fx, fy, fz)
    local vU = Vector3:new(ux, uy, uz)
    local transform = TransformQ.FromOrPos(Quaternion.FromFU(vF, vU), Vector3:new(px, py, pz))
    api.transform.SetTransform(camID, transform)
    api.camera.SetFOVY(camID, nFovY)
    -- Make the manager re-send the game's FOV the moment VR stops overriding it.
    tData.nFOVY = nil
end

-- JWE3 fallback when neither wrapper is ever called (the camera component runs in a Lua state the mod
-- cannot reach): from the VR manager's Advance, take the camera transform the game produced as the anchor
-- and write the VR camera over it. What we wrote last time is remembered, so a read-back of our own write
-- never becomes the anchor (that would compound the head pose every frame).
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

-- JWE3 shows helicopters only from behind (VehicleUtils.CanSwitchCamera), and that camera writes the transform
-- after the VR manager, so the rig cannot move it. If the vehicle has a seat camera, ask the camera manager for
-- the first-person mode on it - what the jeep gets from StartInFirstPerson. Retried a few times per vehicle.
Camera.tSeatRequests = {}
Camera.RequestSeatCamera = function(cmm, tWorld, Gameplay)
    local nVehicle = CameraVehicle(cmm, Gameplay)
    if not nVehicle or nVehicle == 0 or not cmm or not cmm.RequestMode then return nil end
    local nUsage, sWhich = Camera.SeatCamera(tWorld and tWorld.vehicles, nVehicle)
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
-- Per instance (a new park makes a new one); the manager path takes over whenever no instance tick came since its
-- last tick.
Camera.bInstanceDriven = false
Camera.nInstanceTicks, Camera.nInstanceSeen = 0, 0
Camera.TryWrapInstance = function(cmm)
    if global.type(cmm) ~= "table" or Camera.oWrapped == cmm then return end
    Camera.oWrapped = cmm
    if global.rawget(cmm, "__JWE3VRWrapped") then return end
    local fnAdvance = cmm.Advance
    if global.type(fnAdvance) ~= "function" then
        Bridge.Text("camera: the camera manager instance has no Advance to wrap")
        return
    end
    global.rawset(cmm, "__JWE3VRWrapped", true)
    cmm.Advance = function(self, nDt, nUnscaledDt, ...)
        local a, b, c = fnAdvance(self, nDt, nUnscaledDt, ...)
        Camera.nInstanceTicks = Camera.nInstanceTicks + 1
        if not Camera.bInstanceDriven then
            Camera.bInstanceDriven = true
            Bridge.Text("camera: driven from the camera manager's own Advance (right after the game's write)")
        end
        local bOk, sErr = pcall(Camera.ManagerTick, nUnscaledDt or nDt, true)
        if not bOk and not Camera.bInstanceError then
            Camera.bInstanceError = true
            Bridge.Text("camera (instance path) failed: " .. tostring(sErr))
        end
        return a, b, c
    end
    Bridge.Text("camera: wrapped the camera manager instance's Advance")
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
    if not Camera.bManagerPathLogged then
        Camera.bManagerPathLogged = true
        Bridge.Text("camera: driven from the VR manager (camera component not reachable)")
    end
    -- Camera mode: the player-facing UI mode first, else what the camera manager reports through the world
    -- API, else Standard.
    local Gameplay = Camera.GetGameplay()
    local sTag = Gameplay and Gameplay.sUIMode
    local bOk, tWorld = pcall(api.world.GetWorldAPIs)
    if not bOk or global.type(tWorld) ~= "table" then tWorld = nil end
    local cmm = tWorld and tWorld.CameraModeManager
    if not bFromInstance then Camera.TryWrapInstance(cmm) end
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
    local sSeat = nil
    if sTag == "drive" and (sGameMode == "ThirdPersonVehicle" or sGameMode == "VehicleLockOn") then
        sSeat = Camera.RequestSeatCamera(cmm, tWorld, Gameplay)
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
        Bridge.Text("camera tick (manager path): mode " .. sMode .. ", VR override " .. tostring(bOverride))
    end
    if bOverride ~= 1 then
        Camera.lastSet = nil
        return
    end
    local transform = TransformQ.FromOrPos(Quaternion.FromFU(Vector3:new(fx, fy, fz), Vector3:new(ux, uy, uz)),
                                           Vector3:new(px, py, pz))
    api.transform.SetTransform(camID, transform)
    api.camera.SetFOVY(camID, nFovY)
    Camera.lastSet, Camera.nLastFov, Camera.nLastCam = transform, nFovY, camID
end


-- The same rig step from a camera mode's Advance (JWE3 path): rewrites the mode's final transform and FOV.
Camera.AfterModeAdvance = function(sMode, tModeData, tWorldAPIs, _nDt)
    if not Bridge.IsLoaded() then Camera.sSkip = "bridge not loaded"; return end
    local tFinal = tModeData and tModeData.tFinal
    if not tFinal or not tFinal.transformQ then Camera.sSkip = "no tFinal.transformQ"; return end
    local bMain, sManagerMode = IsMainModeData(tModeData, tWorldAPIs)
    if not bMain then Camera.sSkip = "not the main camera"; return end
    Camera.sSkip = nil
    if not Camera.bModeDriven then
        Camera.bModeDriven = true
        Bridge.Text("camera: driven from the camera mode modules (first mode " .. tostring(sMode) .. ")")
    end
    sMode = sManagerMode or sMode
    local nKind = Camera.KindFor(sMode)
    if sMode ~= Camera.sMode then Bridge.Log(Bridge.M.CameraKind, nKind) end
    Camera.sMode = sMode
    Camera.nKind = nKind
    Camera.tModeData = tModeData

    -- No camera shake in VR (the gameplay layer turns shakes into haptics instead).
    local cmm = tWorldAPIs and tWorldAPIs.CameraModeManager
    if cmm and cmm.GetConfig then
        local bOk, tConfig = pcall(cmm.GetConfig, cmm)
        if bOk and global.type(tConfig) == "table" then tConfig.bCameraShake = false end
    end

    local anchor = tFinal.transformQ
    local vPos, vR, vU, vF = anchor:GetPos(), anchor:GetR(), anchor:GetU(), anchor:GetF()
    if nKind == K.Gunner then
        local bOk, mount = pcall(GunMountTransform, tModeData)
        if bOk and mount then vPos = mount:GetPos() end
        local bOk2, body = pcall(require("JWE3VR.Gameplay").GunVehicleTransform)
        if bOk2 and body then
            vR, vU, vF = body:GetR(), body:GetU(), body:GetF()
        elseif bOk and mount then
            vR, vU, vF = mount:GetR(), mount:GetU(), mount:GetF()
        end
    end

    local bOverride, px, py, pz, fx, fy, fz, ux, uy, uz, nFovY = Bridge.Tick(nKind, _nDt or 0, vPos, vR, vU, vF)
    if Camera.nLoggedOverride ~= bOverride then
        Camera.nLoggedOverride = bOverride
        Bridge.Text("camera tick: mode " .. tostring(sMode) .. ", rig kind " .. tostring(nKind) .. ", VR override " .. tostring(bOverride))
    end
    if bOverride ~= 1 then return end
    tFinal.transformQ = TransformQ.FromOrPos(Quaternion.FromFU(Vector3:new(fx, fy, fz), Vector3:new(ux, uy, uz)),
                                             Vector3:new(px, py, pz))
    tFinal.nCameraFOV = nFovY * (tFinal.nMagnify or 1)
end
