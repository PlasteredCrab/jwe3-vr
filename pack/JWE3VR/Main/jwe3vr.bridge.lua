-----------------------------------------------------------------------
--/  @file   JWE3VR.Bridge.lua
--/  @brief  The only door between the game's Lua and JWE3VR.dll: one C function, vr(op, ...),
--/          numbers in and numbers out. Op codes match native/JWE3VR/src/bridge.cpp.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local package = global.package
local tostring = global.tostring
local pcall = global.pcall

local Bridge = module(...)

Bridge.Kind = {Off = 0, Overview = 1, Ground = 2, Seat = 3, Gunner = 4, Screen = 5}
Bridge.Pad = {Off = 0, Passthrough = 1}
-- Pad override mask bits
Bridge.Ov = {LX = 1, LY = 2, RX = 4, RY = 8, LT = 16, RT = 32, Buttons = 64}
-- Controls button bits
Bridge.Btn = {PrimaryL = 1, SecondaryL = 2, PrimaryR = 4, SecondaryR = 8, ClickL = 16, ClickR = 32, Menu = 64, ActiveL = 128, ActiveR = 256}
-- Config indices
Bridge.Cfg = {OverviewScale = 1, HorizonLock = 2, GunnerAimGain = 3, SteeringWheel = 4, HideHudImmersive = 5, LeftHanded = 6, SnapTurnDeg = 7, Seated = 8}

local Op = {Status = 0, Tick = 1, Hand = 2, Controls = 3, Flags = 4, Pad = 5, Haptic = 6, Mouse = 7, Key = 8, Config = 9,
            Recenter = 10, Wheel = 11, HandLocal = 12, Log = 13, PlaceScreen = 14, HudStatus = 15, Pointer = 16}
local kProtocol = 1
local kPath = ".\\Win64\\ovldata\\JWE3VR\\JWE3VR.dll"

local vr = nil
local bTried = false

Bridge.Load = function()
    if vr then return true end
    if bTried then return false end
    bTried = true
    if not package or not package.loadlib then
        api.debug.Trace("JWE3VR: package.loadlib is not available; VR is off")
        return false
    end
    local fn, sErr = package.loadlib(kPath, "jwe3vr_lua")
    if not fn then
        api.debug.Trace("JWE3VR: could not load " .. kPath .. ": " .. tostring(sErr))
        return false
    end
    local bOk, nProto = pcall(fn, Op.Status)
    if not bOk or nProto ~= kProtocol then
        api.debug.Trace("JWE3VR: bridge protocol mismatch (" .. tostring(nProto) .. "); VR is off")
        return false
    end
    vr = fn
    api.debug.Trace("JWE3VR: bridge loaded")
    return true
end

Bridge.IsLoaded = function() return vr ~= nil end

Bridge.Status = function()
    if not vr then return 0, 0, 0, 0 end
    return vr(Op.Status)
end

-- Returns override, px, py, pz, fx, fy, fz, ux, uy, uz, fovY
Bridge.Tick = function(nKind, nDt, vPos, vR, vU, vF)
    if not vr then return 0 end
    return vr(Op.Tick, nKind, nDt, vPos:GetX(), vPos:GetY(), vPos:GetZ(), vR:GetX(), vR:GetY(), vR:GetZ(),
              vU:GetX(), vU:GetY(), vU:GetZ(), vF:GetX(), vF:GetY(), vF:GetZ())
end

-- Returns valid, ox, oy, oz, fx, fy, fz, ux, uy, uz (game space)
Bridge.Hand = function(nHand)
    if not vr then return 0 end
    return vr(Op.Hand, nHand)
end

-- Returns trigL, trigR, gripL, gripR, sxL, syL, sxR, syR, buttonBits
Bridge.Controls = function()
    if not vr then return 0, 0, 0, 0, 0, 0, 0, 0, 0 end
    return vr(Op.Controls)
end

Bridge.Flags = function(nPad, bTurn, bLaser, bGrab, bTablet)
    if not vr then return end
    vr(Op.Flags, nPad, bTurn and 1 or 0, bLaser and 1 or 0, bGrab and 1 or 0, bTablet and 1 or 0)
end

Bridge.PadOverride = function(nMask, lx, ly, rx, ry, lt, rt, nButtons)
    if not vr then return end
    vr(Op.Pad, nMask, lx or 0, ly or 0, rx or 0, ry or 0, lt or 0, rt or 0, nButtons or 0)
end

Bridge.Haptic = function(nHand, nAmp, nSeconds, nFreq)
    if not vr then return end
    vr(Op.Haptic, nHand, nAmp, nSeconds, nFreq or 0)
end

Bridge.Mouse = function(nButton, bDown)
    if not vr then return end
    vr(Op.Mouse, nButton, bDown and 1 or 0)
end

Bridge.Key = function(nVirtualKey)
    if vr then vr(Op.Key, nVirtualKey) end
end

Bridge.Config = function(nIndex)
    if not vr then return 0 end
    return vr(Op.Config, nIndex)
end

Bridge.Recenter = function()
    if vr then vr(Op.Recenter) end
end

-- Returns valid, angleRad (+right)
Bridge.Wheel = function()
    if not vr then return 0, 0 end
    return vr(Op.Wheel)
end

-- Returns valid, x, y, z (play-space metres relative to the seat / play origin)
Bridge.HandLocal = function(nHand)
    if not vr then return 0 end
    return vr(Op.HandLocal, nHand)
end

Bridge.Log = function(nCode, a, b, c)
    if vr then vr(Op.Log, nCode, a or 0, b or 0, c or 0) end
end

Bridge.PlaceScreen = function()
    if vr then vr(Op.PlaceScreen) end
end

-- 1 while the game's UI is being shown as a panel in the world (in-world HUD), else 0
Bridge.HudStatus = function()
    if not vr then return 0 end
    return vr(Op.HudStatus)
end

-- Pointer dot at a game-space point (laser hit, rifle sight); pass nil to hide it
Bridge.Pointer = function(vPos)
    if not vr then return end
    if vPos then
        vr(Op.Pointer, 1, vPos:GetX(), vPos:GetY(), vPos:GetZ())
    else
        vr(Op.Pointer, 0, 0, 0, 0)
    end
end
