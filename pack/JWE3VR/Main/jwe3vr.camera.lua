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

-- Game camera mode -> how the VR rig treats it. Cinematic and scripted cameras go to the flat screen.
Camera.tModeKinds = {
    Standard = K.Overview,
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

Camera.Patch = function(tModule)
    if tModule.__JWE3VRPatched then return end
    tModule.__JWE3VRPatched = true
    local fnAdvance = tModule.Advance
    tModule.Advance = function(self, _nDeltaTime, _nUnscaledDeltaTime)
        fnAdvance(self, _nDeltaTime, _nUnscaledDeltaTime)
        local bOk, sErr = pcall(Camera.AfterAdvance, self, _nUnscaledDeltaTime)
        if not bOk and not Camera.bErrorLogged then
            Camera.bErrorLogged = true
            api.debug.Trace("JWE3VR camera: " .. tostring(sErr))
        end
    end
    api.debug.Trace("JWE3VR: camera manager patched")
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
    if bOverride ~= 1 then return end

    local vF = Vector3:new(fx, fy, fz)
    local vU = Vector3:new(ux, uy, uz)
    local transform = TransformQ.FromOrPos(Quaternion.FromFU(vF, vU), Vector3:new(px, py, pz))
    api.transform.SetTransform(camID, transform)
    api.camera.SetFOVY(camID, nFovY)
    -- Make the manager re-send the game's FOV the moment VR stops overriding it.
    tData.nFOVY = nil
end
