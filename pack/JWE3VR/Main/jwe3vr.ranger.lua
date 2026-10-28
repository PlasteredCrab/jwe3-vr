-----------------------------------------------------------------------
--/  @file   JWE3VR.Ranger.lua
--/  @brief  On-foot ranger, while walking in the First Person View:
--/            * dart rifle and medicine gun in your hand: each hit counts toward the game's own
--/              dart estimate for that dinosaur, then it is tranquillised / treated
--/            * radio: point at a dinosaur or building to send the nearest team (game order screen),
--/              point at a vehicle to take it over, or call the nearest free jeep to where you stand
--/          Tools cycle on the dominant B/Y button; the pointer dot shows where you aim.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local math = global.math
local pcall = global.pcall
local pairs = global.pairs
local require = global.require

local Vector3 = require("Vector3")
local Bridge = require("JWE3VR.Bridge")

local Ranger = module(...)

Ranger.Tool = {None = 0, Tranq = 1, Medicine = 2, Radio = 3}
Ranger.nTool = 0
Ranger.nCooldown = 0
Ranger.tHits = {} -- [entity] = {tranq = n, medicine = n}

local kRange = 80        -- metres
local kShotInterval = 1.2 -- seconds between darts (reload)

local WorldAPIs = function() return api.world.GetWorldAPIs() end
local Has = function(bits, flag) return (math.floor((bits or 0) / flag) % 2) == 1 end

-- What a dart (and the hand's pointer) can hit: the game's "everything" set without foliage (grass and bushes
-- should not stop a dart) and the invisible camera-obstacle and scenery-selection volumes. Fences do stop it.
Ranger.ShotFlags = function()
    if Ranger.nShotFlags then return Ranger.nShotFlags end
    local P = api.physics
    local n = 0
    for _, sFlag in global.ipairs({"Flag_Landscape", "Flag_Water", "Flag_Prop", "Flag_PhysicsDisabledProp", "Flag_Gate",
                                   "Flag_Building", "Flag_Wall", "Flag_Fence", "Flag_Track", "Flag_Pylon", "Flag_Vehicle",
                                   "Flag_Dinosaur", "Flag_Guest", "Flag_LagoonFloor"}) do
        local i = global.type(P[sFlag]) == "number" and math.tointeger(P[sFlag]) or nil
        if i then n = n | i end
    end
    Ranger.nShotFlags = n ~= 0 and n or require("Helpers.RaycastUtils").nEverythingFlags
    return Ranger.nShotFlags
end

-- The nearest living dinosaur to vFrom and its world position (debug aim; nil when there is none).
Ranger.NearestDinosaur = function(vFrom)
    local dinos = WorldAPIs().dinosaurs
    local bOk, tDinos = pcall(dinos.GetDinosaurs, dinos, true)
    if not bOk or global.type(tDinos) ~= "table" then return nil end
    local best, vBest, nBest = nil, nil, math.huge
    for _, e in pairs(tDinos) do
        local bP, v = pcall(api.transform.CalculateWorldPosition, e)
        if bP and v then
            local vPos = Vector3:new(v:GetX(), v:GetY(), v:GetZ())
            local d = (vPos - vFrom):Length()
            if d < nBest then best, vBest, nBest = e, vPos, d end
        end
    end
    return best, vBest, nBest
end

-- Hand ray and what it hits: entity, hit point (both may be nil). With Ranger.bAimDino (remote tests:
-- JWE3VR_drive.req "aimdino") the ray from the hand goes to the nearest dinosaur, at the first of a few heights
-- above its origin whose ray reaches it (a tranquillised dinosaur lies below the 1 m a standing one is hit at).
Ranger.Aim = function(nHand, nRange)
    local bValid, ox, oy, oz, fx, fy, fz = Bridge.Hand(nHand)
    if bValid ~= 1 then return nil end
    local vStart, vDir = Vector3:new(ox, oy, oz), Vector3:new(fx, fy, fz)
    local physicsWorld = api.entity.FindEntityByName("MainPhysicsWorld")
    local Cast = function(v)
        return api.physics.RayCast(physicsWorld, vStart, vStart + v * (nRange or kRange), Ranger.ShotFlags(), 1)
    end
    if Ranger.bAimDino then
        local dino, vDino = Ranger.NearestDinosaur(vStart)
        if vDino then
            for _, nUp in global.ipairs({1.0, 0.6, 0.3, 1.5}) do
                local v = (vDino + Vector3:new(0, nUp, 0) - vStart):Normalised()
                local t = Cast(v)
                if nUp == 1.0 then vDir = v end
                if t and t[1] and t[1].nEntityID == dino then
                    vDir = v
                    break
                end
            end
        end
    end
    local tResults = Cast(vDir)
    if tResults and #tResults > 0 then
        local hit = tResults[1]
        return hit.nEntityID, vStart + vDir * hit.nDistance, vStart, vDir
    end
    return nil, nil, vStart, vDir
end

local IsDinosaur = function(entity)
    if not entity then return false end
    local dinos = WorldAPIs().dinosaurs
    local bOk, bDead = pcall(dinos.IsDead, dinos, entity)
    return bOk and bDead == false
end

local DartsNeeded = function(entity, nTool)
    local dinos = WorldAPIs().dinosaurs
    local names = nTool == Ranger.Tool.Tranq and {"GetApproxNumDartsToTranquillise"} or {"GetApproxNumDartsToCure", "GetApproxNumDartsToTreat"}
    for _, sName in global.ipairs(names) do
        local fn = dinos[sName]
        if fn then
            local bOk, n = pcall(fn, dinos, entity)
            if bOk and global.type(n) == "number" and n > 0 then return math.ceil(n), "game estimate" end
        end
    end
    return nTool == Ranger.Tool.Tranq and 3 or 2, "mod default"
end

Ranger.Fire = function(nHand)
    local entity = Ranger.Aim(nHand)
    Bridge.Haptic(nHand, 0.9, 0.08, 0) -- the shot
    local sTool = Ranger.nTool == Ranger.Tool.Tranq and "dart rifle" or "medicine gun"
    if not IsDinosaur(entity) then
        Bridge.Text(sTool .. ": missed (hit " .. global.tostring(entity) .. ", not a living dinosaur)")
        return
    end
    local t = Ranger.tHits[entity] or {tranq = 0, medicine = 0}
    Ranger.tHits[entity] = t
    local dinos = WorldAPIs().dinosaurs
    if Ranger.nTool == Ranger.Tool.Tranq then
        t.tranq = t.tranq + 1
        local nNeeded, sSource = DartsNeeded(entity, Ranger.Tool.Tranq)
        Bridge.Text(sTool .. ": hit dinosaur " .. global.tostring(entity) .. ", dart " .. t.tranq .. " of " .. nNeeded .. " (" .. sSource .. ")")
        if t.tranq >= nNeeded then
            t.tranq = 0
            if Ranger.Call(dinos, "Tranquillise", entity) then
                Bridge.Haptic(nHand, 0.5, 0.4, 0) -- long buzz: down
                local bOk2, bAwake = pcall(dinos.IsConscious, dinos, entity)
                Bridge.Text(sTool .. ": dinosaur " .. global.tostring(entity) .. " tranquillised (conscious: " .. global.tostring(bOk2 and bAwake) .. ")")
            end
        else
            Bridge.Haptic(nHand, 0.4, 0.05, 0) -- hit
        end
    elseif Ranger.nTool == Ranger.Tool.Medicine then
        -- Medicine darts cure disease (injuries still need the paleo-medical facility), as in the game.
        local bOk, bSick = pcall(dinos.IsDiseased, dinos, entity)
        if not bOk or not bSick then
            Bridge.Haptic(nHand, 0.15, 0.15, 0) -- nothing to treat
            Bridge.Text(sTool .. ": dinosaur " .. global.tostring(entity) .. " is not sick (nothing to treat)")
            return
        end
        t.medicine = t.medicine + 1
        local nNeeded, sSource = DartsNeeded(entity, Ranger.Tool.Medicine)
        Bridge.Text(sTool .. ": hit sick dinosaur " .. global.tostring(entity) .. ", dart " .. t.medicine .. " of " .. nNeeded .. " (" .. sSource .. ")")
        if t.medicine >= nNeeded then
            t.medicine = 0
            if Ranger.Call(dinos, "RemoveDisease", entity) then
                Bridge.Haptic(nHand, 0.5, 0.4, 0) -- treated
                local bOk2, bStill = pcall(dinos.IsDiseased, dinos, entity)
                Bridge.Text(sTool .. ": dinosaur " .. global.tostring(entity) .. " treated (still sick: " .. global.tostring(bOk2 and bStill) .. ")")
            end
        else
            Bridge.Haptic(nHand, 0.4, 0.05, 0)
        end
    end
end

-- Calls api:sName(entity) if it exists; logs (once per name) when it doesn't or fails.
Ranger.tWarned = {}
Ranger.Call = function(tApi, sName, entity)
    local fn = tApi[sName]
    local bOk, sErr = false, "missing"
    if fn then bOk, sErr = pcall(fn, tApi, entity) end
    if not bOk and not Ranger.tWarned[sName] then
        Ranger.tWarned[sName] = true
        Bridge.Text("ranger: " .. sName .. " failed: " .. global.tostring(sErr))
    end
    return bOk
end

-- The nearest free ranger jeep drives to where you stand (the game's own "flare" order).
Ranger.CallJeep = function(nHand)
    local tWorld = WorldAPIs()
    local camT = api.transform.GetTransform(api.camera.GetMainCameraID())
    if not camT or not tWorld.VehicleAIDirector or not tWorld.VehicleTaskQueue then return false end
    local vHere = camT:GetPos()
    local vehicle, sWhy = require("JWE3VR.Vehicles").FindNearestTeam(vHere, false)
    if not vehicle then
        Bridge.Haptic(nHand, 0.2, 0.3, 0)
        Bridge.Text("radio: no ranger jeep to call (" .. global.tostring(sWhy) .. ")")
        return false
    end
    local nIndex = 1
    local bOk, VehicleUtils = pcall(require, "Helpers.VehicleUtils")
    if bOk and VehicleUtils and VehicleUtils.GetTaskQueueInsertionIndex then
        local bOk2, n = pcall(VehicleUtils.GetTaskQueueInsertionIndex, vehicle, 0, tWorld.VehicleTaskQueue)
        if bOk2 and n then nIndex = n end
    end
    tWorld.VehicleTaskQueue:InsertTask(vehicle, nIndex, "FireFlare", {vTarget = vHere})
    Bridge.Haptic(nHand, 0.6, 0.1, 0)
    Bridge.Text("radio: jeep " .. global.tostring(vehicle) .. " called to where you stand")
    return true
end

-- Pulses on tool change: one per tool index (0 = hands empty: one short tick).
local tToolNames = {[0] = "empty hands", "dart rifle", "medicine gun", "radio"}
local AnnounceTool = function(nHand, nTool)
    Bridge.Haptic(nHand, 0.3 + 0.15 * nTool, 0.04 + 0.05 * nTool, 0)
    Bridge.Text("on foot: " .. (tToolNames[nTool] or global.tostring(nTool)))
end

-- Called each tick while walking. Returns the pad overrides it needs (mask, rt, clearB).
Ranger.Advance = function(nDt, nDom, tCtl, fnPressed, Gameplay)
    Ranger.nCooldown = math.max(0, Ranger.nCooldown - (nDt or 0))
    local Btn = Bridge.Btn
    local secondaryDom = nDom == 1 and Btn.SecondaryR or Btn.SecondaryL
    local primaryDom = nDom == 1 and Btn.PrimaryR or Btn.PrimaryL
    local nTrigger = nDom == 1 and tCtl.tr or tCtl.tl
    local nGripOff = nDom == 1 and tCtl.gl or tCtl.gr

    -- Short press (released within 0.8 s) cycles the tool; holding it leaves walking (Gameplay). Timed from the
    -- first tick that saw it down, on the bridge's wall clock (Gameplay.nLastNow).
    local bHeld = Has(tCtl.bits, secondaryDom)
    local nNow = (Gameplay and Gameplay.nLastNow) or 0
    if bHeld then
        Ranger.nDownAt = Ranger.nDownAt or nNow
        Ranger.nHeld = nNow - Ranger.nDownAt
    else
        if Ranger.nDownAt and (Ranger.nHeld or 0) < 0.8 then
            Ranger.nTool = (Ranger.nTool + 1) % 4
            AnnounceTool(nDom, Ranger.nTool)
        end
        Ranger.nDownAt, Ranger.nHeld = nil, 0
    end
    if Ranger.nTool == Ranger.Tool.None then
        Bridge.Pointer(nil)
        return 0
    end

    local entity, vHit = Ranger.Aim(nDom)
    Bridge.Pointer(vHit)

    local bFire = nTrigger > 0.7 and not Ranger.bTriggerHeld
    Ranger.bTriggerHeld = nTrigger > 0.5
    if Ranger.nTool == Ranger.Tool.Radio then
        if bFire then
            local tWorld = WorldAPIs()
            if entity and tWorld.vehicles:IsVehicle(entity) then
                pcall(Gameplay.TakeControl)
            else
                pcall(Gameplay.SendTeam, nGripOff > 0.6)
            end
        end
        if fnPressed(primaryDom) then pcall(Ranger.CallJeep, nDom) end
    elseif bFire and Ranger.nCooldown <= 0 then
        Ranger.nCooldown = kShotInterval
        pcall(Ranger.Fire, nDom)
    end
    -- The trigger and B (and A with the radio) belong to the tools now, not to the game.
    return Bridge.Ov.RT
end

Ranger.Reset = function()
    Ranger.nTool = Ranger.Tool.None
    Ranger.tHits = {}
    Bridge.Pointer(nil)
end
