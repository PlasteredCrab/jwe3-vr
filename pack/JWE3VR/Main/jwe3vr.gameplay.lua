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
    end
    Gameplay.PatchMouseRay()
end

------------------------------------------------------------------------
-- Laser picking: every Lua cursor raycast in the game goes through api.input.GetMouseRay
-- (Helpers.RaycastUtils.GetCursorRaycast, SelectVehicleDestination, build modes...).
------------------------------------------------------------------------
Gameplay.PatchMouseRay = function()
    local InputAPI = api.input
    if not InputAPI or InputAPI.__JWE3VRPatched then return end
    InputAPI.__JWE3VRPatched = true
    local fnRay = InputAPI.GetMouseRay
    local fnRayUnclamped = InputAPI.GetMouseRayUnclamped
    local HandRay = function()
        if not Gameplay.bLaser then return nil end
        local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(Gameplay.nDominant)
        if bValid ~= 1 then return nil end
        return Vector3:new(ox, oy, oz), Vector3:new(fx, fy, fz)
    end
    InputAPI.GetMouseRay = function(...)
        local vStart, vDir = HandRay()
        if vStart then return api.camera.GetMainCameraID(), vStart, vDir end
        return fnRay(...)
    end
    if fnRayUnclamped then
        InputAPI.GetMouseRayUnclamped = function(...)
            local vStart, vDir = HandRay()
            if vStart then return api.camera.GetMainCameraID(), vStart, vDir end
            return fnRayUnclamped(...)
        end
    end
end

------------------------------------------------------------------------
-- Driving: always sit in the driver's seat when a vehicle is entered.
------------------------------------------------------------------------
-- JWE3's DriveVehicleUIMode.TransitionIn(self, _tInitData) switches to the first-person camera itself
-- when _tInitData.StartInFirstPerson is set and the vehicle can switch camera; ask for that in VR.
Gameplay.PatchDriveMode = function(tModule)
    if tModule.__JWE3VRPatched then return end
    tModule.__JWE3VRPatched = true
    local fnIn = tModule.TransitionIn
    tModule.TransitionIn = function(self, _tInitData, ...)
        if Bridge.IsLoaded() and global.type(_tInitData) == "table" and _tInitData.StartInFirstPerson == nil then
            _tInitData.StartInFirstPerson = true
        end
        return fnIn(self, _tInitData, ...)
    end
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

Gameplay.GunnerAim = function()
    local tModeData = Camera.tModeData
    if not tModeData or not tModeData.gunnerID then return 0, 0, false end
    local WeaponsAPI = api.world.GetWorldAPIs().weapons
    -- Measure both directions in the vehicle body's frame: a frame that turned with the turret would
    -- move with the aim and the error would never close.
    local frame = Gameplay.GunVehicleTransform() or WeaponsAPI:GetCameraAttachTransform(tModeData.gunnerID)
    local vAim = WeaponsAPI:GetCameraAimDirection(tModeData.gunnerID)
    local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(Gameplay.nDominant)
    if not frame or not vAim or bValid ~= 1 then return 0, 0, false end
    local vR, vU, vF = frame:GetR(), frame:GetU(), frame:GetF()
    local hYaw, hPitch = Angles(Vector3:new(fx, fy, fz), vR, vU, vF)
    local aYaw, aPitch = Angles(vAim, vR, vU, vF)
    local nErrX, nErrY = WrapPi(hYaw - aYaw), hPitch - aPitch
    local nGain = Bridge.Config(Bridge.Cfg.GunnerAimGain)
    if nGain <= 0 then nGain = 6 end
    local t = Gameplay.tAim
    local nCmdX = Clamp(nErrX * nGain * t.signX, -1, 1)
    local nCmdY = Clamp(nErrY * nGain * t.signY, -1, 1)
    AdaptSign(t, "X", nErrX, nCmdX)
    AdaptSign(t, "Y", nErrY, nCmdY)
    return nCmdX, nCmdY, true
end

-- The game's own gunner UI mode does the switch so the HUD, the dinosaur status filter and the
-- whistle mode change stay consistent (VehicleGunnerUIMode._SwitchAmmo).
Gameplay.oGunnerMode = nil

Gameplay.PatchGunnerMode = function(tModule)
    if tModule.__JWE3VRPatched then return end
    tModule.__JWE3VRPatched = true
    local fnIn, fnOut = tModule.TransitionIn, tModule.TransitionOut
    tModule.TransitionIn = function(self, ...)
        Gameplay.oGunnerMode = self
        return fnIn(self, ...)
    end
    tModule.TransitionOut = function(self, ...)
        if Gameplay.oGunnerMode == self then Gameplay.oGunnerMode = nil end
        return fnOut(self, ...)
    end
end

Gameplay.SwitchAmmo = function(bNext)
    local oMode = Gameplay.oGunnerMode
    if not oMode or not oMode.bCanSwitchWeapon or not oMode._SwitchAmmo then return end
    oMode:_SwitchAmmo(not bNext)
    Bridge.Haptic(1 - Gameplay.nDominant, 0.4, 0.05, 0)
end

-- Vehicle body transform of the active gun seat (does not turn with the turret).
Gameplay.GunVehicleTransform = function()
    local oMode = Gameplay.oGunnerMode
    if not oMode or not oMode.vehicle then return nil end
    return api.transform.GetTransform(oMode.vehicle)
end

Gameplay.OnWeaponFired = function()
    Bridge.Haptic(Gameplay.nDominant, 0.9, 0.08, 0)
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

Gameplay.SendTeam = function(bHelicopter)
    local entity, vPos = Gameplay.EntityUnderLaser()
    if not vPos or not Gameplay.iUIMode then return false end
    local vehicle = Vehicles.FindNearestTeam(vPos, bHelicopter)
    if not vehicle then
        Bridge.Haptic(Gameplay.nDominant, 0.2, 0.3, 0) -- long weak buzz: nobody free
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

Gameplay.Advance = function(_nDt)
    if not Bridge.IsLoaded() then return end
    local tl, tr, gl, gr, sxl, syl, sxr, syr, nBits = Bridge.Controls()
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

    -- Tablet: off-hand B/Y shows the flat game screen (with its HUD) in front of you.
    if Pressed(secondaryOff) and bImmersive then
        Gameplay.bTablet = not Gameplay.bTablet
        if Gameplay.bTablet then Bridge.PlaceScreen() end
    end
    if not bImmersive then Gameplay.bTablet = false end

    local nPad, bTurn, bLaser, bGrab = Bridge.Pad.Off, false, false, false
    local nMask, lx, ly, rx, ry, nButtons = 0, 0, 0, 0, 0, 0

    if Gameplay.bTablet then
        -- Pointing at the tablet drives the mouse (native side); keep the pad quiet.
        nPad = Bridge.Pad.Off
        Bridge.Pointer(nil)
    elseif nKind == K.Overview then
        bLaser, bGrab = true, true
        if Pressed(primaryDom) then pcall(Gameplay.TakeControl) end
        if Pressed(secondaryDom) then pcall(Gameplay.SendTeam, gripOff > 0.6) end
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
        local bHeld = Has(nBits, secondaryDom)
        Gameplay.nBHeld = bHeld and (Gameplay.nBHeld or 0) + (_nDt or 0) or 0
        if Gameplay.nBHeld > 0.8 and not Gameplay.bEscSent then
            Gameplay.bEscSent = true
            Bridge.Key(0x1B) -- Escape: leave the First Person View
        elseif not bHeld then
            Gameplay.bEscSent = false
        end
        nButtons = Gameplay.PadButtons(nBits, gl, gr, dom)
        nButtons = nButtons & ~XI.B
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
            end
        end
    elseif nKind == K.Gunner then
        nPad = Bridge.Pad.Passthrough
        local bOk, cx, cy, bValid = pcall(Gameplay.GunnerAim)
        if bOk and bValid then
            nMask = nMask + Ov.RX + Ov.RY
            rx, ry = cx, cy
        end
        -- Off-hand stick left/right cycles dart / medicine / scanner.
        nMask = nMask + Ov.LX
        if not tPrev.ammoLatch and math.abs(stickOffX) > 0.7 then
            tPrev.ammoLatch = true
            pcall(Gameplay.SwitchAmmo, stickOffX > 0)
        elseif math.abs(stickOffX) < 0.3 then
            tPrev.ammoLatch = false
        end
    end

    Gameplay.bLaser = bLaser
    Bridge.Flags(nPad, bTurn, bLaser, bGrab, Gameplay.bTablet)
    if nMask ~= 0 then Bridge.PadOverride(nMask, lx, ly, rx, ry, 0, 0, nButtons) end

    -- HUD: hidden while you are in the 3D world (it would sit flat at infinity); shown on the tablet and in menus.
    if nKind ~= K.Ground and nKind ~= K.Overview then Bridge.Pointer(nil) end
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
