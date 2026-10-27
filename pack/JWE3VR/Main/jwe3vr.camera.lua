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
