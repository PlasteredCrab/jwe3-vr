-----------------------------------------------------------------------
--/  @file   JWE3VR.Gameplay.lua
--/  @brief  Per-tick VR gameplay on top of the camera rig:
--/            * which controller behaviour each camera mode gets (virtual pad, laser, grab, turn)
--/            * hand-aimed dart / medicine guns (closed loop on the game's own gunner aim)
--/            * steering wheel, weapon haptics, tablet + HUD, recentre
--/            * laser picking: the game's cursor ray becomes the hand's ray
--/            * staff wand: take over a vehicle, or send the nearest team to what you point at
-----------------------------------------------------------------------
local global = _G
local api = global.api
local math = global.math
local pcall = global.pcall
local tostring = global.tostring
local require = global.require

local Vector3 = require("Vector3")
local Bridge = require("JWE3VR.Bridge")
local Camera = require("JWE3VR.Camera")
local Ranger = require("JWE3VR.Ranger")
local Vehicles = require("JWE3VR.Vehicles")

local Gameplay = module(...)

local K = Bridge.Kind
local Ov = Bridge.Ov
local Btn = Bridge.Btn

-- XInput button bits (for overrides)
local XI = {A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000}

Gameplay.bLaser = false
Gameplay.bTablet = false
Gameplay.nDominant = 1
Gameplay.tPrev = {buttons = 0, ammoLatch = false}
Gameplay.tAim = {signX = 1, signY = 1, lastErrX = 0, lastErrY = 0, growX = 0, growY = 0}
Gameplay.bHudShown = nil
Gameplay.iUIMode = nil

local Clamp = function(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local WrapPi = function(a)
    while a > math.pi do a = a - 2 * math.pi end
    while a < -math.pi do a = a + 2 * math.pi end
    return a
end

local Has = function(bits, flag) return (math.floor(bits / flag) % 2) == 1 end

-- Yaw/pitch of a direction in a frame given by its right/up/forward vectors (handedness-free).
local Angles = function(vDir, vR, vU, vF)
    local x, y, z = Vector3.Dot(vDir, vR), Vector3.Dot(vDir, vU), Vector3.Dot(vDir, vF)
    return math.atan(x, z), math.atan(y, math.sqrt(x * x + z * z))
end

Gameplay.Init = function(_tEnvironment)
    Gameplay.nDominant = (Bridge.Config(Bridge.Cfg.LeftHanded) == 1) and 0 or 1
    if _tEnvironment then
        local bOk, iUIMode = pcall(function() return _tEnvironment:RequireInterface("Editors.Shared.IUIMode") end)
        if bOk then Gameplay.iUIMode = iUIMode end
        local bOk2, iDisease = pcall(function() return _tEnvironment:RequireInterface("Interfaces.IDiseaseManager") end)
        if bOk2 then Gameplay.iDiseaseManager = iDisease end
    end
    Gameplay.PatchMouseRay()
end

------------------------------------------------------------------------
-- Laser picking. JWE3 makes the api tables read-only ("Scripts may NOT modify the API, by design"), so
-- api.input.GetMouseRay cannot be replaced as on JWE2. Helpers.RaycastUtils.GetCursorRaycast (the cursor
-- ray behind GetTerrainPositionUnderCursor, GetEntityUnderCursor, ...) is a Lua module function called through
-- the module table, so the hand ray goes in there; anything that calls GetMouseRay directly follows the OS
-- cursor, which the laser also drives (input.cpp).
------------------------------------------------------------------------
Gameplay.PatchMouseRay = function()
    local bOk, RaycastUtils = pcall(require, "Helpers.RaycastUtils")
    if not bOk or global.type(RaycastUtils) ~= "table" or RaycastUtils.__JWE3VRPatched then return end
    local fnCursor = RaycastUtils.GetCursorRaycast
    if global.type(fnCursor) ~= "function" then return end
    RaycastUtils.__JWE3VRPatched = true
    RaycastUtils.GetCursorRaycast = function(...)
        if Gameplay.bLaser then
            local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(Gameplay.nDominant)
            if bValid == 1 then return Vector3:new(ox, oy, oz), Vector3:new(fx, fy, fz) end
        end
        return fnCursor(...)
    end
end

------------------------------------------------------------------------
-- Driving: always sit in the driver's seat when a vehicle is entered.
------------------------------------------------------------------------
-- JWE3's DriveVehicleUIMode.TransitionIn(self, _tInitData) switches to the first-person camera itself
-- when _tInitData.StartInFirstPerson is set and the vehicle can switch camera; ask for that in VR.
-- The player-facing UI mode decides the rig kind on JWE3: the camera component runs in a Lua state the mod
-- cannot reach, but UI modes are Lua objects in ours (their TransitionIn/Out are looked up on the module).
-- Their transitions run as coroutines (TransitionIn waits for the vehicle), so the log shows another lua_State
-- pointer for them; it is a thread of the same state (seen in game: the manager sees sUIMode).
-- Tag -> camera mode name (Camera.tModeKinds); nil = whatever the camera manager reports.
Gameplay.sUIMode = nil
Gameplay.tUIModeCamera = {drive = "FirstPersonVehicle", gunner = "Gunner", photo = "FirstPersonVehicle",
                          guest = "Guest", cinematic = "Animated"}

Gameplay.oUIMode = nil -- the active UI mode object (its nVehicleEntityID is the vehicle being driven)

local SetUIMode = function(sTag, bOn, self)
    if bOn then
        Gameplay.sUIMode = sTag
        Gameplay.oUIMode = self
        if sTag == "gunner" then
            Gameplay.nAimLogs, Gameplay.bGameFireLogged = 0, false
            Gameplay.tAim.iX, Gameplay.tAim.iY = 0, 0
        end
        if sTag == "drive" then Gameplay.bWheelLogged = false end
    elseif Gameplay.sUIMode == sTag then
        Gameplay.sUIMode = nil
        Gameplay.oUIMode = nil
    end
    Bridge.Text("ui mode " .. sTag .. (bOn and " in" or " out"))
end

-- Wraps a UI mode module's TransitionIn/Out to track sTag (fnBeforeIn may adjust the init data).
Gameplay.PatchUIMode = function(tModule, sTag, fnBeforeIn)
    if global.type(tModule) ~= "table" or tModule.__JWE3VRUIPatched then return end
    local fnIn, fnOut = tModule.TransitionIn, tModule.TransitionOut
    if global.type(fnIn) ~= "function" or global.type(fnOut) ~= "function" then return end
    tModule.__JWE3VRUIPatched = true
    tModule.TransitionIn = function(self, ...)
        if fnBeforeIn then pcall(fnBeforeIn, self, ...) end
        pcall(SetUIMode, sTag, true, self)
        return fnIn(self, ...)
    end
    tModule.TransitionOut = function(self, ...)
        pcall(SetUIMode, sTag, false, self)
        return fnOut(self, ...)
    end
end

-- JWE3's DriveVehicleUIMode.TransitionIn(self, _tInitData) switches to the first-person camera itself when
-- _tInitData.StartInFirstPerson is set and the vehicle can switch camera; ask for that in VR. Drones are
-- driven through the same mode.
Gameplay.PatchDriveMode = function(tModule)
    Gameplay.PatchUIMode(tModule, "drive", function(self, _tInitData)
        if Bridge.IsLoaded() and global.type(_tInitData) == "table" and _tInitData.StartInFirstPerson == nil then
            _tInitData.StartInFirstPerson = true
        end
    end)
end

------------------------------------------------------------------------
-- Gunner: steer the game's gun toward the hand with the right stick (closed loop). The stick's
-- effect direction depends on the player's invert settings, so each axis learns its own sign.
------------------------------------------------------------------------
local AdaptSign = function(t, axis, nErr, nCmd)
    local sLast, sGrow, sSign = "lastErr" .. axis, "grow" .. axis, "sign" .. axis
    if math.abs(nCmd) > 0.3 then
        if math.abs(nErr) > math.abs(t[sLast]) + 0.002 then t[sGrow] = t[sGrow] + 1 else t[sGrow] = 0 end
        if t[sGrow] > 12 then
            t[sSign] = -t[sSign]
            t[sGrow] = 0
        end
    end
    t[sLast] = nErr
end

-- Helicopter or aviary drone (the game's drive / gunner mode objects carry bIsDrone and nVehicleType).
Gameplay.IsAirVehicle = function(oMode)
    if global.type(oMode) ~= "table" then return false end
    if oMode.bIsDrone then return true end
    local V = oMode.VehiclesAPI
    return V ~= nil and oMode.nVehicleType ~= nil and oMode.nVehicleType == V.VehicleType_Helicopter
end

-- Photo mode from the active drive or gun seat, on the press of the dominant stick click. The game's own switch:
-- the drive mode's _OnSwitchToPhotoButtonPressed (which checks VehicleUtils.CanSwitchToPhoto), or in a gun seat the
-- same "RequestChangeUI" event its Advance sends when its photo button is pressed.
Gameplay.PhotoClick = function(bDown, tPrev)
    if not bDown then
        tPrev.photoLatch = false
        return
    end
    if tPrev.photoLatch then return end
    tPrev.photoLatch = true
    local bOk, sErr = pcall(function()
        local oGun = Gameplay.oGunnerMode
        if Gameplay.sUIMode == "photo" then
            -- The same click leaves photo mode (the game's Helicopter_SwitchToPhoto toggles too).
            local oPhoto = Gameplay.oUIMode
            if oPhoto and oPhoto._ReturnToPreviousMode and not oPhoto.tPhoto then
                oPhoto:_ReturnToPreviousMode()
                Bridge.Text("photo: asked the game to leave photo mode")
            end
            return
        elseif oGun and Gameplay.sUIMode == "gunner" then
            local VehicleUtils = require("Helpers.VehicleUtils")
            if not VehicleUtils.CanSwitchToPhoto(oGun.nVehicleEntityID, oGun.VehiclesAPI) then
                Bridge.Text("photo: this vehicle has no photo mode")
                return
            end
            oGun.iUIMode:OnModeEvent("RequestChangeUI", "VehiclePhoto", {Vehicle = oGun.nVehicleEntityID,
                DeleteVehicleWhenDone = oGun.bDeleteVehicleWhenDone, bDeleteVehicleOffScreenWhenDone = oGun.bDeleteVehicleOffScreenWhenDone,
                bWasFirstPerson = oGun.bWasFirstPerson, tGuestModeState = oGun.tGuestModeState})
        else
            local oDrive = Gameplay.oUIMode
            if not oDrive or not oDrive._OnSwitchToPhotoButtonPressed then return end
            oDrive:_OnSwitchToPhotoButtonPressed()
        end
        Bridge.Text("photo: asked the game for photo mode")
    end)
    if not bOk then Bridge.Text("photo: switch failed: " .. tostring(sErr)) end
end

-- Sight dot in the gun seats: where the gun's own aim ray meets the world (not the hand's: the gun lags the hand,
-- and the dart goes where the gun points). The game's crosshair is part of its UI, which the in-world HUD moves onto
-- the panel, so in the headset nothing else shows where a dart will land. Returns the point and what it is on.
Gameplay.GunSight = function()
    local _, gunnerID = Gameplay.GunnerIDs()
    if not gunnerID then return nil end
    local WeaponsAPI = api.world.GetWorldAPIs().weapons
    local mount = WeaponsAPI:GetCameraAttachTransform(gunnerID)
    local vAim = WeaponsAPI:GetCameraAimDirection(gunnerID)
    if not mount or not vAim then return nil end
    local p = mount:GetPos()
    local vDir = Vector3:new(vAim:GetX(), vAim:GetY(), vAim:GetZ()):Normalised()
    -- Start clear of the vehicle's own body (the mount sits inside the helicopter and the drone).
    local vStart = Vector3:new(p:GetX(), p:GetY(), p:GetZ()) + vDir * 2
    local physicsWorld = api.entity.FindEntityByName("MainPhysicsWorld")
    local t = api.physics.RayCast(physicsWorld, vStart, vStart + vDir * 300, Ranger.ShotFlags(), 1)
    if t and t[1] then return vStart + vDir * t[1].nDistance, t[1].nEntityID end
    return nil
end

-- Debug aim (JWE3VR_drive.req "aimdino") in a gun seat: the direction from the gun mount to the nearest dinosaur,
-- in place of the hand's, so the aim loop follows the animal (a scripted hand cannot at the remote 2-3 fps).
Gameplay.DebugGunTarget = function(gunnerID, WeaponsAPI)
    local mount = WeaponsAPI:GetCameraAttachTransform(gunnerID)
    if not mount then return nil end
    local p = mount:GetPos()
    local vFrom = Vector3:new(p:GetX(), p:GetY(), p:GetZ())
    -- Stay on one dinosaur while it lives (until "aimdino" is sent again): following the nearest one swung the
    -- gun between animals that were about as far away (run 143).
    local dino, vDino = Gameplay.nDebugGunDino, nil
    if dino then
        local dinos = api.world.GetWorldAPIs().dinosaurs
        local bOk, bDead = pcall(dinos.IsDead, dinos, dino)
        local bP, v = pcall(api.transform.CalculateWorldPosition, dino)
        if bOk and bDead == false and bP and v then vDino = Vector3:new(v:GetX(), v:GetY(), v:GetZ()) end
    end
    if not vDino then
        local nDist
        dino, vDino, nDist = Ranger.NearestDinosaur(vFrom)
        if not vDino then return nil end
        Gameplay.nDebugGunDino = dino
        Bridge.Text(global.string.format("gun aim: debug target dinosaur %s at %.1f m", tostring(dino), nDist))
    end
    return (vDino + Vector3:new(0, 0.5, 0) - vFrom):Normalised()
end

Gameplay.GunnerAim = function()
    local _, gunnerID = Gameplay.GunnerIDs()
    if not gunnerID then return 0, 0, false end
    local WeaponsAPI = api.world.GetWorldAPIs().weapons
    -- Measure both directions in the vehicle body's frame: a frame that turned with the turret would
    -- move with the aim and the error would never close.
    local frame = Gameplay.GunVehicleTransform() or WeaponsAPI:GetCameraAttachTransform(gunnerID)
    local vAim = WeaponsAPI:GetCameraAimDirection(gunnerID)
    local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(Gameplay.nDominant)
    if not frame or not vAim or bValid ~= 1 then return 0, 0, false end
    local vR, vU, vF = frame:GetR(), frame:GetU(), frame:GetF()
    local vHand = Vector3:new(fx, fy, fz)
    if Ranger.bAimDino then vHand = Gameplay.DebugGunTarget(gunnerID, WeaponsAPI) or vHand end
    local hYaw, hPitch = Angles(vHand, vR, vU, vF)
    local aYaw, aPitch = Angles(vAim, vR, vU, vF)
    local nErrX, nErrY = WrapPi(hYaw - aYaw), hPitch - aPitch
    local nGain = Bridge.Config(Bridge.Cfg.GunnerAimGain)
    if nGain <= 0 then nGain = 6 end
    local t = Gameplay.tAim
    -- Integral term: the game ignores small stick deflections, so the proportional command alone settled about
    -- 3 degrees low in pitch (run 142, stick -0.3 with no motion) and the drone's darts landed short. It only
    -- integrates while the error is small (no wind-up during a big swing) and restarts when a sign flips.
    local nNow = Gameplay.nLastNow or 0
    local nDt = math.min(0.25, math.max(0, nNow - (t.nAt or nNow)))
    t.nAt = nNow
    local kI, kIMax, kISmall = 2.0, 0.35, 0.1
    for _, s in global.ipairs({"X", "Y"}) do
        local nErr = s == "X" and nErrX or nErrY
        if math.abs(nErr) < kISmall and t["iSign" .. s] == t["sign" .. s] then
            t["i" .. s] = Clamp((t["i" .. s] or 0) + nErr * kI * nDt, -kIMax, kIMax)
        else
            t["i" .. s], t["iSign" .. s] = 0, t["sign" .. s]
        end
    end
    local nCmdX = Clamp((nErrX * nGain + t.iX) * t.signX, -1, 1)
    local nCmdY = Clamp((nErrY * nGain + t.iY) * t.signY, -1, 1)
    AdaptSign(t, "X", nErrX, nCmdX)
    AdaptSign(t, "Y", nErrY, nCmdY)
    -- Aim error now and then (at most 20 lines per gun-seat stay): a working loop shows it shrinking.
    if (Gameplay.nAimLogs or 0) < 20 and nNow - (Gameplay.nAimLogAt or -10) >= 2 then
        Gameplay.nAimLogs, Gameplay.nAimLogAt = (Gameplay.nAimLogs or 0) + 1, nNow
        local nOn, sOn = Gameplay.nSightOn, "nothing"
        if nOn then
            local dinos = api.world.GetWorldAPIs().dinosaurs
            local bOk, bDead = pcall(dinos.IsDead, dinos, nOn)
            sOn = tostring(nOn) .. ((bOk and bDead == false) and " (dinosaur)" or "")
        end
        Bridge.Text(global.string.format("gun aim: hand vs gun yaw %.1f, pitch %.1f deg; stick %.2f %.2f (integral %.2f %.2f); sight on %s",
                                         nErrX * 57.3, nErrY * 57.3, nCmdX, nCmdY, t.iX, t.iY, sOn))
    end
    return nCmdX, nCmdY, true
end

-- The game's own gunner UI mode does the switch so the HUD, the dinosaur status filter and the
-- whistle mode change stay consistent (VehicleGunnerUIMode._SwitchAmmo).
Gameplay.oGunnerMode = nil

Gameplay.PatchGunnerMode = function(tModule)
    if global.type(tModule) ~= "table" or tModule.__JWE3VRPatched then return end
    tModule.__JWE3VRPatched = true
    local fnIn, fnOut = tModule.TransitionIn, tModule.TransitionOut
    tModule.TransitionIn = function(self, ...)
        Gameplay.oGunnerMode = self
        pcall(SetUIMode, "gunner", true, self)
        return fnIn(self, ...)
    end
    tModule.TransitionOut = function(self, ...)
        if Gameplay.oGunnerMode == self then Gameplay.oGunnerMode = nil end
        pcall(SetUIMode, "gunner", false, self)
        return fnOut(self, ...)
    end
    -- The game's own handler of its gunner's shots (looked up through self when a message arrives, so wrapping
    -- the module function reaches it): proof that a shot happened, independent of the mod's own receiver.
    local fnFired = tModule._HandleWeaponFiredMessage
    if global.type(fnFired) == "function" then
        tModule._HandleWeaponFiredMessage = function(self, tMessages, ...)
            if not Gameplay.bGameFireLogged then
                Gameplay.bGameFireLogged = true
                pcall(Bridge.Text, "gun seat: the game reports a shot")
            end
            return fnFired(self, tMessages, ...)
        end
    end
end

-- The active gun seat's entities (JWE3 VehicleGunnerUIMode: nVehicleEntityID, nGunnerEntityID).
Gameplay.GunnerIDs = function()
    local oMode = Gameplay.oGunnerMode
    if not oMode then return nil, nil end
    return oMode.nVehicleEntityID or oMode.vehicle, oMode.nGunnerEntityID
end

-- The vehicle of the active drive / gun seat / photo mode, or nil.
Gameplay.ActiveVehicle = function()
    local oMode = Gameplay.oUIMode
    return global.type(oMode) == "table" and oMode.nVehicleEntityID or nil
end

Gameplay.SwitchAmmo = function(bNext)
    local oMode = Gameplay.oGunnerMode
    if not oMode or not oMode.bCanSwitchWeapon or not oMode._SwitchAmmo then return end
    oMode:_SwitchAmmo(not bNext)
    Bridge.Haptic(1 - Gameplay.nDominant, 0.4, 0.05, 0)
end

-- Vehicle body transform of the active gun seat (does not turn with the turret).
Gameplay.GunVehicleTransform = function()
    local nVehicle = Gameplay.GunnerIDs()
    if not nVehicle then return nil end
    return api.transform.GetTransform(nVehicle)
end

Gameplay.OnWeaponFired = function()
    Bridge.Haptic(Gameplay.nDominant, 0.9, 0.08, 0)
    Gameplay.nShots = (Gameplay.nShots or 0) + 1
    Bridge.Text("gun seat: weapon fired (haptic kick), shot " .. Gameplay.nShots)
end

-- Where the darts and flares land (any gun: jeep, helicopter, drone), for remote tests: what was hit and whether
-- it is a dinosaur, and the dinosaur's state afterwards.
Gameplay.OnProjectileImpact = function(tMessages)
    local dinos = api.world.GetWorldAPIs().dinosaurs
    for _, tMessage in global.ipairs(tMessages or {}) do
        local hit = tMessage.nHitEntity
        local bOk, bDead = pcall(dinos.IsDead, dinos, hit)
        local sWhat = (bOk and bDead == false) and "dinosaur" or "not a dinosaur"
        local sState = ""
        if sWhat == "dinosaur" then
            local bC, bAwake = pcall(dinos.IsConscious, dinos, hit)
            local bS, bSick = pcall(dinos.IsDiseased, dinos, hit)
            sState = ", conscious " .. tostring(bC and bAwake) .. ", diseased " .. tostring(bS and bSick)
        end
        Bridge.Text("projectile type " .. tostring(tMessage.eProjectileType) .. " hit " .. tostring(hit) .. " (" .. sWhat .. sState .. ")")
    end
end

------------------------------------------------------------------------
-- Staff wand (overview): A on a vehicle takes control of it; B on anything else sends the nearest
-- free ranger team (or ACU helicopter with the grip held) into the game's own order screen, where
-- the laser picks the target and the trigger confirms.
------------------------------------------------------------------------
Gameplay.EntityUnderLaser = function()
    local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(Gameplay.nDominant)
    if bValid ~= 1 then return nil end
    local RaycastUtils = require("Helpers.RaycastUtils")
    local vStart, vDir = Vector3:new(ox, oy, oz), Vector3:new(fx, fy, fz)
    return RaycastUtils.GetEntityUnderRaycast(vStart, vDir), RaycastUtils.GetTerrainPositionUnderRaycast(vStart, vDir)
end

Gameplay.TakeControl = function()
    local entity = Gameplay.EntityUnderLaser()
    local tWorldAPIs = api.world.GetWorldAPIs()
    if not entity or not Gameplay.iUIMode or not tWorldAPIs.vehicles:IsVehicle(entity) then return false end
    Gameplay.iUIMode:OnModeEvent("RequestChangeUI", "DriveVehicle", {Vehicle = entity, DeleteVehicleWhenDone = false})
    Bridge.Haptic(Gameplay.nDominant, 0.6, 0.1, 0)
    return true
end

-- Debug (JWE3VR_drive.req): take control of the team vehicle nearest the camera, found as the radio and staff wand
-- find one (Vehicles.FindNearestTeam, but in any state: direct control needs no working facility), entered as the
-- laser's take-control enters one.
Gameplay.DebugDrive = function(bHelicopter)
    local camID = api.camera.GetMainCameraID()
    local t = camID and api.transform.GetTransform(camID)
    local vehicle, sWhy = nil, "no camera"
    if t then vehicle, sWhy = Vehicles.FindNearestTeam(t:GetPos(), bHelicopter, true) end
    Bridge.Text("debug drive: nearest " .. (bHelicopter and "ACU helicopter" or "ranger jeep") .. " is " .. tostring(vehicle) ..
                (vehicle and "" or (" (" .. tostring(sWhy) .. ")")))
    if not vehicle or not Gameplay.iUIMode then return false end
    Gameplay.iUIMode:OnModeEvent("RequestChangeUI", "DriveVehicle", {Vehicle = vehicle, DeleteVehicleWhenDone = false})
    return true
end

-- Debug (JWE3VR_drive.req "aimdino" / "sick"): "aimdino" switches the on-foot tools' aim to the nearest dinosaur
-- and back (scripted controllers cannot follow a moving animal at the remote 2-3 fps); "sick" gives the nearest
-- dinosaur a disease through the game's disease manager, for the medicine gun. Both log the dinosaur.
Gameplay.DebugDinosaur = function(bSick)
    local cam = api.transform.CalculateWorldTransform(api.camera.GetMainCameraID())
    local cp = cam:GetPos()
    local dino, vDino, nDist = Ranger.NearestDinosaur(Vector3:new(cp:GetX(), cp:GetY(), cp:GetZ()))
    local sDino = "nearest dinosaur " .. tostring(dino) .. (dino and global.string.format(" at %.1f m", nDist) or "")
    if vDino then
        -- Where it is from the camera (degrees right of where the camera faces, metres below): to steer remotely.
        local dx, dy, dz = vDino:GetX() - cp:GetX(), vDino:GetY() - cp:GetY(), vDino:GetZ() - cp:GetZ()
        local f, r = cam:GetF(), cam:GetR()
        local nAhead = dx * f:GetX() + dz * f:GetZ()
        local nRight = dx * r:GetX() + dz * r:GetZ()
        sDino = sDino .. global.string.format(" (bearing %.0f deg right, %.0f m below)", math.atan(nRight, nAhead) * 57.3, -dy)
    end
    if not bSick then
        Ranger.bAimDino = not Ranger.bAimDino
        Gameplay.nDebugGunDino = nil
        Bridge.Text("debug: on-foot aim at the nearest dinosaur " .. (Ranger.bAimDino and "on" or "off") .. "; " .. sDino)
        return
    end
    if not dino or not Gameplay.iDiseaseManager then
        Bridge.Text("debug sick: " .. sDino .. ", disease manager " .. tostring(Gameplay.iDiseaseManager))
        return
    end
    -- AddDiseaseToDino(id, dino, bIgnoreResistance, sCause) returns false for a disease this island does not
    -- configure, one already vaccinated against, or a dinosaur it cannot infect: try the island's own diseases,
    -- first those the game treats with medicine darts (it has a "<name>Dart" for them; run 140: the first one
    -- alphabetically, algae poisoning, is an unmet-needs disease that the game cures through the habitat).
    local iDisease = Gameplay.iDiseaseManager
    local tDart = {"DDT_Roundworm", "DDT_EColi", "DDT_Cryptosporidiosis", "DDT_Coccidioidomycosis", "DDT_Rabies",
                   "DDT_InfectedWound", "DDT_IvyPoisoning", "DDT_AvianPox"}
    local tIDs, tSeen = {}, {}
    local tConfig = global.type(iDisease.tDiseaseConfig) == "table" and iDisease.tDiseaseConfig or nil
    for _, sID in global.ipairs(tDart) do
        if not tConfig or tConfig[sID] then tIDs[#tIDs + 1], tSeen[sID] = sID, true end
    end
    if tConfig then
        local tRest = {}
        for sID in global.pairs(tConfig) do if not tSeen[sID] then tRest[#tRest + 1] = sID end end
        global.table.sort(tRest)
        for _, sID in global.ipairs(tRest) do tIDs[#tIDs + 1] = sID end
    end
    local dinos = api.world.GetWorldAPIs().dinosaurs
    local sResult = "no disease could be added (tried " .. #tIDs .. ")"
    for _, sID in global.ipairs(tIDs) do
        local bOk, bAdded = pcall(iDisease.AddDiseaseToDino, iDisease, sID, dino, true, "JWE3VR")
        if bOk and bAdded then
            sResult = "added " .. sID
            break
        end
    end
    local bOk2, bSickNow = pcall(dinos.IsDiseased, dinos, dino)
    Bridge.Text("debug sick: " .. sDino .. ", " .. sResult .. ", diseased " .. tostring(bOk2 and bSickNow))
end

-- Debug (JWE3VR_drive.req "paths"): for the response facility (the spawn point of the nearest ranger jeep, and its
-- parents) and every arrival building, whether the game counts its required path connections as made, and where its
-- path join ("Path_Junction" child: where a path must end to connect) is in the world and on the flat screen (u,v from
-- the top left) - to aim the path tool at it remotely. World transforms throughout (the camera is parented).
Gameplay.DebugPaths = function()
    local tWorld = api.world.GetWorldAPIs()
    local civ = tWorld.civileng
    local camID = api.camera.GetMainCameraID()
    local cam = api.transform.CalculateWorldTransform(camID)
    local nFovY, nAspect = 0.873, 16 / 9
    local bFov, vFov = pcall(api.camera.GetFOV, camID)
    local sx, sy = global.string.match(tostring(vFov), "%(([%d%.%-]+),([%d%.%-]+)%)")
    if bFov and sx then
        local nX, nY = global.tonumber(sx), global.tonumber(sy)
        if nX and nY and nX > 0 and nY > 0 then nFovY, nAspect = nY, math.tan(nX * 0.5) / math.tan(nY * 0.5) end
    end
    local cp, cR, cU, cF = cam:GetPos(), cam:GetR(), cam:GetU(), cam:GetF()
    local Screen = function(v)
        local dx, dy, dz = v:GetX() - cp:GetX(), v:GetY() - cp:GetY(), v:GetZ() - cp:GetZ()
        local x = dx * cR:GetX() + dy * cR:GetY() + dz * cR:GetZ()
        local y = dx * cU:GetX() + dy * cU:GetY() + dz * cU:GetZ()
        local z = dx * cF:GetX() + dy * cF:GetY() + dz * cF:GetZ()
        if z <= 0.1 then return "behind" end
        local tn = math.tan(nFovY * 0.5)
        return global.string.format("%.3f,%.3f", 0.5 + 0.5 * x / (z * tn * nAspect), 0.5 - 0.5 * y / (z * tn))
    end
    local Where = function(e)
        local bP, v = pcall(api.transform.CalculateWorldPosition, e)
        if not bP or not v then return "?" end
        return global.string.format("(%.1f %.1f %.1f) screen %s", v:GetX(), v:GetY(), v:GetZ(), Screen(v))
    end
    local Junction = function(nEntity)
        for _, sName in global.ipairs({"./Path_Junction", "./Root/Path_Junction", "./Foundation/Path_Junction"}) do
            local bF, e = pcall(api.entity.FindEntityByName, sName, nEntity)
            if bF and global.type(e) == "number" and e > 0 then return e, sName end
        end
        return nil
    end
    local Report = function(sWhat, nEntity)
        if not nEntity then
            Bridge.Text("paths: " .. sWhat .. ": none")
            return
        end
        local bOk, bHas = pcall(civ.HasRequiredPathConnections, civ, nEntity)
        local bOk2, bOper = pcall(civ.IsFoundationOperational, civ, nEntity)
        local nJ, sJ = Junction(nEntity)
        local Links = function(e)
            local bC, n = pcall(civ.GetJunctionConnectionCount, civ, e)
            return bC and tostring(n) or "?"
        end
        Bridge.Text("paths: " .. sWhat .. " " .. tostring(nEntity) .. " at " .. Where(nEntity) .. ": paths connected " ..
                    tostring(bOk and bHas) .. ", operational " .. tostring(bOk2 and bOper) .. "; join " ..
                    (nJ and (sJ .. " " .. tostring(nJ) .. " at " .. Where(nJ) .. ", " .. Links(nJ) .. " path links") or "not found"))
        -- Every junction the game lists for this foundation, with its links (0 = nothing joined there).
        local bJ, tJ = pcall(civ.GetJunctionsFromFoundation, civ, nEntity)
        if bJ and global.type(tJ) == "table" then
            for _, e in global.pairs(tJ) do
                if global.type(e) == "number" then
                    Bridge.Text("paths:   junction " .. tostring(e) .. " at " .. Where(e) .. ", " .. Links(e) .. " path links")
                end
            end
        elseif bJ then
            Bridge.Text("paths:   GetJunctionsFromFoundation: " .. tostring(tJ))
        end
    end
    Bridge.Text("paths: camera at " .. global.string.format("(%.1f %.1f %.1f)", cp:GetX(), cp:GetY(), cp:GetZ()) ..
                ", fovY " .. global.string.format("%.3f", nFovY))
    local veh = Vehicles.FindNearestTeam(cp, false, true)
    local bSp, nSpawn = pcall(tWorld.vehicles.GetSpawnPoint, tWorld.vehicles, veh)
    local nEntity = bSp and nSpawn or nil
    for i = 1, 4 do
        if not nEntity then break end
        Report("response facility (level " .. i .. ")", nEntity)
        local bPar, nPar = pcall(api.transform.GetParent, nEntity)
        nEntity = (bPar and global.type(nPar) == "number" and nPar > 0 and nPar ~= nEntity) and nPar or nil
    end
    for _, sApi in global.ipairs({"park", "buildings", "building"}) do
        local o = tWorld[sApi]
        if o and o.GetAllArrivalBuildings then
            local bA, tAll = pcall(o.GetAllArrivalBuildings, o)
            if bA and global.type(tAll) == "table" then
                for k, v in global.pairs(tAll) do
                    Report("arrival building", global.type(v) == "number" and v or (global.type(k) == "number" and k or nil))
                end
            end
            break
        end
    end
    -- No walk over the path graph here: civileng:GetJunctionConnections / GetConnectionJunctions crashed the game
    -- (access violation in native code, run 137) - pcall cannot catch that. Only the junction-count call is safe.
end

Gameplay.SendTeam = function(bHelicopter)
    local entity, vPos = Gameplay.EntityUnderLaser()
    if not vPos or not Gameplay.iUIMode then
        Bridge.Text("send team: " .. (vPos and "no UI mode interface" or "the laser is not on the ground"))
        return false
    end
    local vehicle, sWhy = Vehicles.FindNearestTeam(vPos, bHelicopter)
    if not vehicle then
        Bridge.Haptic(Gameplay.nDominant, 0.2, 0.3, 0) -- long weak buzz: nobody free
        Bridge.Text("send team: no " .. (bHelicopter and "ACU helicopter" or "ranger team") .. " can take orders (" ..
                    tostring(sWhy) .. ")")
        return false
    end
    Gameplay.iUIMode:OnModeEvent("RequestChangeUI", "SelectVehicleDestination", {Vehicle = vehicle, OrderPriority = 1})
    Bridge.Haptic(Gameplay.nDominant, 0.6, 0.1, 0)
    return true
end

------------------------------------------------------------------------
-- Per-tick
------------------------------------------------------------------------
Gameplay.SetHud = function(bShow)
    if Gameplay.bHudShown == bShow then return end
    Gameplay.bHudShown = bShow
    if api.render and api.render.SetShouldRenderGUI then api.render.SetShouldRenderGUI(bShow) end
end

Gameplay.Advance = function(_nDt, _nGameDt)
    if not Bridge.IsLoaded() then return end
    local nDebug = Bridge.DebugCommand()
    if nDebug == 1 or nDebug == 2 then
        local bOk, sErr = pcall(Gameplay.DebugDrive, nDebug == 2)
        if not bOk then Bridge.Text("debug drive failed: " .. tostring(sErr)) end
    elseif nDebug == 3 then
        local bOk, sErr = pcall(Gameplay.DebugPaths)
        if not bOk then Bridge.Text("debug paths failed: " .. tostring(sErr)) end
    elseif nDebug == 4 or nDebug == 5 then
        local bOk, sErr = pcall(Gameplay.DebugDinosaur, nDebug == 5)
        if not bOk then Bridge.Text("debug dinosaur failed: " .. tostring(sErr)) end
    end
    local tl, tr, gl, gr, sxl, syl, sxr, syr, nBits, nNow = Bridge.Controls()
    -- Hold and pulse timers run on the wall clock: the game's time step is clamped to 1/15 s on slow frames and
    -- stops while paused (run 106: a 3 s hold of dominant B counted as 0.6 s at 3 fps and never left walking).
    if nNow then
        local nReal = Gameplay.nLastNow and (nNow - Gameplay.nLastNow) or nil
        Gameplay.nLastNow = nNow
        if nReal and nReal >= 0 and nReal < 1 then _nDt = nReal end
    end
    nBits = math.floor(nBits or 0)
    local dom = Gameplay.nDominant
    local tPrev = Gameplay.tPrev
    local Pressed = function(flag) return Has(nBits, flag) and not Has(tPrev.buttons, flag) end
    local primaryDom = dom == 1 and Btn.PrimaryR or Btn.PrimaryL
    local secondaryDom = dom == 1 and Btn.SecondaryR or Btn.SecondaryL
    local secondaryOff = dom == 1 and Btn.SecondaryL or Btn.SecondaryR
    local gripDom = dom == 1 and gr or gl
    local trigOff, gripOff = (dom == 1) and tl or tr, (dom == 1) and gl or gr
    local stickOffX = dom == 1 and sxl or sxr

    -- Recentre: both stick clicks.
    if Has(nBits, Btn.ClickL) and Has(nBits, Btn.ClickR) and not (Has(tPrev.buttons, Btn.ClickL) and Has(tPrev.buttons, Btn.ClickR)) then
        Bridge.Recenter()
    end

    local nKind = Camera.nKind
    local bImmersive = nKind == K.Overview or nKind == K.Ground or nKind == K.Seat or nKind == K.Gunner
    -- The pad-B pulse that leaves walking ends with walking: the game leaves the guest view on the pulse's first
    -- frames, and what was left of it must not press B the next time you walk.
    if nKind ~= K.Ground then
        Gameplay.nBackPulse, Gameplay.nBackTicks, Gameplay.nBDownAt = 0, 0, nil
        Ranger.nDownAt, Ranger.nHeld = nil, 0
    end

    -- Tablet: off-hand B/Y shows the flat game screen (with its HUD) in front of you. In a vehicle the off-hand
    -- grip must be held too: there plain off-hand Y is pad Y, the game's Aim Mode (JWE3: into the gun seat).
    local bVehicle = nKind == K.Seat or nKind == K.Gunner
    local bTabletChord = bVehicle and gripOff > 0.6
    if Pressed(secondaryOff) and bImmersive and (not bVehicle or bTabletChord) then
        Gameplay.bTablet = not Gameplay.bTablet
        if Gameplay.bTablet then Bridge.PlaceScreen() end
        Bridge.Text("tablet " .. (Gameplay.bTablet and "up" or "away"))
    end
    if not bImmersive then Gameplay.bTablet = false end

    local nPad, bTurn, bLaser, bGrab, bFire = Bridge.Pad.Off, false, false, false, false
    local nMask, lx, ly, rx, ry, nButtons = 0, 0, 0, 0, 0, 0

    -- The pause menu, opened with the menu button where the pad is otherwise off (below): while the game stays
    -- paused (its scaled time step is 0; the VR manager still ticks), the controllers drive the menu as a pad -
    -- A selects, B backs out and resumes, the sticks move - as in the seats. The laser on the HUD panel still clicks.
    Gameplay.nMenuGrace = math.max(0, (Gameplay.nMenuGrace or 0) - (_nDt or 0))
    if Gameplay.bMenuPause and (_nGameDt or 0) > 0 and Gameplay.nMenuGrace == 0 then
        Gameplay.bMenuPause = false
        Bridge.Text("pause menu: the game runs again; controllers back to the park")
    end

    if Gameplay.bMenuPause then
        nPad = Bridge.Pad.Passthrough
        nButtons = Gameplay.PadButtons(nBits, gl, gr, dom)
        nMask = Ov.Buttons
        bLaser = true -- the HUD panel takes the laser (native side); no park pointer behind the menu
        Bridge.Pointer(nil)
    elseif Gameplay.bTablet then
        -- Pointing at the tablet drives the mouse (native side); keep the pad quiet.
        nPad = Bridge.Pad.Off
        Bridge.Pointer(nil)
    elseif nKind == K.Overview then
        bLaser, bGrab = true, true
        if Pressed(primaryDom) then
            local bOk, sErr = pcall(Gameplay.TakeControl)
            if not bOk then Bridge.Text("take control failed: " .. tostring(sErr)) end
        end
        if Pressed(secondaryDom) then
            local bOk, sErr = pcall(Gameplay.SendTeam, gripOff > 0.6)
            if not bOk then Bridge.Text("send team failed: " .. tostring(sErr)) end
        end
        -- Pointer dot where the laser meets the park.
        local RaycastUtils = require("Helpers.RaycastUtils")
        local bOk, _, vHit = pcall(Ranger.Aim, dom, RaycastUtils.nRayLength or 1000)
        Bridge.Pointer(bOk and vHit or nil)
    elseif nKind == K.Ground then
        nPad, bTurn = Bridge.Pad.Passthrough, true
        nMask, ry = nMask + Ov.RY, 0 -- pitch comes from your head
        -- On-foot ranger tools own dominant B (cycle; hold to leave walking) and, with a tool out, the trigger.
        local tCtl = {tl = tl, tr = tr, gl = gl, gr = gr, bits = nBits}
        local bOk, nToolMask = pcall(Ranger.Advance, _nDt, dom, tCtl, Pressed, Gameplay)
        if bOk and nToolMask and nToolMask ~= 0 then nMask = nMask + nToolMask end
        -- How long dominant B has been held: from the first tick that saw it down, on the wall clock (adding up
        -- tick intervals also counted the time before the press: at 2 fps a 0.6 s tap read as a 1.0 s hold, run 112).
        local bHeld = Has(nBits, secondaryDom)
        if bHeld then Gameplay.nBDownAt = Gameplay.nBDownAt or (Gameplay.nLastNow or 0) else Gameplay.nBDownAt = nil end
        Gameplay.nBHeld = bHeld and ((Gameplay.nLastNow or 0) - Gameplay.nBDownAt) or 0
        if Gameplay.nBHeld > 0.8 and not Gameplay.bEscSent then
            -- Leave the First Person View with the game's own pad B (seen in game, run 85: pad B leaves the guest
            -- view), not a synthetic Escape key: the mod sends no OS-level input. A pulse of at least 0.25 s and
            -- 3 game ticks, so a slow frame cannot swallow it.
            Gameplay.bEscSent, Gameplay.nBackPulse, Gameplay.nBackTicks = true, 0.25, 3
            Bridge.Text("walking: dominant B held " .. global.string.format("%.1f", Gameplay.nBHeld) .. " s, pad B to leave")
        elseif not bHeld then
            Gameplay.bEscSent = false
        end
        nButtons = Gameplay.PadButtons(nBits, gl, gr, dom)
        nButtons = nButtons & ~XI.B
        if (Gameplay.nBackPulse or 0) > 0 or (Gameplay.nBackTicks or 0) > 0 then
            nButtons = nButtons | XI.B
            Gameplay.nBackPulse = (Gameplay.nBackPulse or 0) - (_nDt or 0)
            Gameplay.nBackTicks = (Gameplay.nBackTicks or 0) - 1
        end
        if Ranger.nTool == Ranger.Tool.Radio then nButtons = nButtons & ~XI.A end
        nMask = nMask + Ov.Buttons
    elseif nKind == K.Seat then
        nPad = Bridge.Pad.Passthrough
        nMask = nMask + Ov.RX + Ov.RY -- look around with your head, not the stick
        if Bridge.Config(Bridge.Cfg.SteeringWheel) == 1 and gl > 0.6 and gr > 0.6 then
            local bValid, nAngle = Bridge.Wheel()
            if bValid == 1 then
                nMask = nMask + Ov.LX
                lx = Clamp(nAngle / (math.pi * 0.5), -1, 1)
                if not Gameplay.bWheelLogged and math.abs(nAngle) > 0.17 then
                    Gameplay.bWheelLogged = true -- once per drive: the hands steer
                    Bridge.Text(global.string.format("steering wheel: hands turned %.0f deg, steering %.2f", nAngle * 57.3, lx))
                end
            end
        end
        -- Buttons as mapped natively, but the grips held for the wheel are not LB / RB (JWE3: LB opens the site
        -- map), and the tablet chord (off-hand grip + Y) is not Aim Mode.
        nButtons = Gameplay.PadButtons(nBits, gl, gr, dom)
        if gl > 0.6 and gr > 0.6 then nButtons = nButtons & ~0x0300 end
        if bTabletChord then nButtons = nButtons & ~XI.Y end
        -- JWE3 switches to photo mode with pad d-pad right (up / down / left switch to the chase camera; seen in
        -- game, run 92), and the VR controllers have no d-pad: here the dominant stick click is d-pad right (pad RS
        -- does nothing in the driver's seat). Not with both clicks, which recentre. In photo mode, A takes the photo.
        -- Helicopters and the aviary drone have their own photo switch (Helicopter_SwitchToPhoto; their d-pad
        -- cycles the drone's equipment, run 148), so there the click asks the game's drive mode directly.
        local bClickDom = Has(nBits, dom == 1 and Btn.ClickR or Btn.ClickL)
        local bClickOff = Has(nBits, dom == 1 and Btn.ClickL or Btn.ClickR)
        local bAir = Gameplay.IsAirVehicle(Gameplay.oUIMode)
        if bClickDom then
            nButtons = nButtons & ~(0x0040 | 0x0080)
            if not bClickOff and not bAir then nButtons = nButtons | 0x0008 end -- XINPUT_GAMEPAD_DPAD_RIGHT
        end
        if bAir then Gameplay.PhotoClick(bClickDom and not bClickOff, tPrev) end
        nMask = nMask + Ov.Buttons
    elseif nKind == K.Gunner then
        nPad = Bridge.Pad.Passthrough
        bFire = true -- the dominant trigger fires (as the left mouse button, native side)
        local bOk, cx, cy, bValid = pcall(Gameplay.GunnerAim)
        if bOk and bValid then
            nMask = nMask + Ov.RX + Ov.RY
            rx, ry = cx, cy
        end
        -- Dominant stick click: photo mode, as in the driver's seat (the gun seats have no d-pad mapping for it).
        local bClickDom = Has(nBits, dom == 1 and Btn.ClickR or Btn.ClickL)
        local bClickOff = Has(nBits, dom == 1 and Btn.ClickL or Btn.ClickR)
        if bClickDom then nButtons = nButtons & ~(0x0040 | 0x0080) end
        Gameplay.PhotoClick(bClickDom and not bClickOff, tPrev)
        local bSight, vSight, nOn = pcall(Gameplay.GunSight)
        Bridge.Pointer(bSight and vSight or nil)
        Gameplay.nSightOn = bSight and nOn or nil
        nButtons = Gameplay.PadButtons(nBits, gl, gr, dom)
        if bTabletChord then nButtons = nButtons & ~XI.Y end
        nMask = nMask + Ov.Buttons
        -- Off-hand stick left/right cycles dart / medicine / scanner.
        nMask = nMask + Ov.LX
        if not tPrev.ammoLatch and math.abs(stickOffX) > 0.7 then
            tPrev.ammoLatch = true
            pcall(Gameplay.SwitchAmmo, stickOffX > 0)
        elseif math.abs(stickOffX) < 0.3 then
            tPrev.ammoLatch = false
        end
    end

    -- The menu button reaches the game everywhere. Where the pad is otherwise off (park overview, tablet, flat
    -- screen), it alone is passed through - pad START, or BACK with the off-hand grip - with everything else at rest
    -- (seen with the scripted controllers, run 102: before, the menu button did nothing in the park overview).
    if nPad == Bridge.Pad.Off and Has(nBits, Btn.Menu) then
        nPad = Bridge.Pad.Passthrough
        nMask, lx, ly, rx, ry = Ov.LX + Ov.LY + Ov.RX + Ov.RY + Ov.LT + Ov.RT + Ov.Buttons, 0, 0, 0, 0
        nButtons = (gripOff > 0.6) and 0x0020 or 0x0010 -- XINPUT_GAMEPAD_BACK / START
        -- The game pauses a frame or two later: give it a second before the time step says it did not.
        if gripOff <= 0.6 and not Gameplay.bMenuPause then
            Gameplay.bMenuPause, Gameplay.nMenuGrace = true, 1.0
            Bridge.Text("pause menu: menu button passed to the game; the controllers drive the menu while it is paused")
        end
    end

    Gameplay.bLaser = bLaser
    Bridge.Flags(nPad, bTurn, bLaser, bGrab, Gameplay.bTablet, bFire and not Gameplay.bTablet)
    if nMask ~= 0 then Bridge.PadOverride(nMask, lx, ly, rx, ry, 0, 0, nButtons) end

    -- HUD: hidden while you are in the 3D world (it would sit flat at infinity); shown on the tablet and in menus.
    if nKind ~= K.Ground and nKind ~= K.Overview and nKind ~= K.Gunner then Bridge.Pointer(nil) end
    if nKind ~= K.Ground and Ranger.nTool ~= Ranger.Tool.None then Ranger.Reset() end

    -- HUD: with the in-world HUD the game's UI is a panel in the world, so it stays on. Without it,
    -- the HUD would sit flat at infinity in both eyes, so it is hidden while immersive (tablet shows it).
    -- The GUI stays on for the first seconds of immersion so the in-world HUD gets a chance to engage.
    if bImmersive then Gameplay.nImmersiveTime = (Gameplay.nImmersiveTime or 0) + (_nDt or 0) else Gameplay.nImmersiveTime = 0 end
    local bWorldHud = Bridge.HudStatus() == 1
    local bProbe = Gameplay.nImmersiveTime < 5
    local bHideHud = Bridge.Config(Bridge.Cfg.HideHudImmersive) == 1 and not bWorldHud and not bProbe
    Gameplay.SetHud(not (bHideHud and bImmersive and not Gameplay.bTablet and nKind ~= K.Overview))

    tPrev.buttons = nBits
end

-- The virtual pad's buttons as the native side maps them (input.cpp), so a mode can drop some.
Gameplay.PadButtons = function(nBits, gl, gr, dom)
    local off = 1 - dom
    local b = 0
    local function Add(bOn, flag) if bOn then b = b | flag end end
    Add(Has(nBits, dom == 1 and Btn.PrimaryR or Btn.PrimaryL), XI.A)
    Add(Has(nBits, dom == 1 and Btn.SecondaryR or Btn.SecondaryL), XI.B)
    Add(Has(nBits, off == 1 and Btn.PrimaryR or Btn.PrimaryL), XI.X)
    Add(Has(nBits, off == 1 and Btn.SecondaryR or Btn.SecondaryL), XI.Y)
    local gOff, gDom = off == 0 and gl or gr, dom == 0 and gl or gr
    Add(gOff > 0.6, 0x0100) -- LB
    Add(gDom > 0.6, 0x0200) -- RB
    Add(Has(nBits, off == 1 and Btn.ClickR or Btn.ClickL), 0x0040)
    Add(Has(nBits, dom == 1 and Btn.ClickR or Btn.ClickL), 0x0080)
    if Has(nBits, Btn.Menu) then b = b | ((gOff > 0.6) and 0x0020 or 0x0010) end
    return b
end

Gameplay.Shutdown = function()
    Gameplay.SetHud(true)
    Gameplay.bLaser = false
    pcall(Ranger.Reset)
    if Bridge.IsLoaded() then Bridge.Flags(Bridge.Pad.Off, false, false, false, false) end
end
