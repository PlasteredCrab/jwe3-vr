-----------------------------------------------------------------------
--/  @file   JWE3VR.Vehicles.lua
--/  @brief  Finds park team vehicles for the staff wand and the radio. JWE3 dropped JWE2's
--/          VehicleAIDirector:FindAvailableRangerCar / FindAvailableACUHelicopter, so this walks the
--/          director's own tables (tCars / tHelicopters, each entry tagged with nSubtype) and keeps
--/          the nearest vehicle that can take orders (Helpers.VehicleUtils.CanVehicleBeIssuedTasks).
--/          Field names and subtype values from components.vehicleaidirector, JWE3 build 1792088119.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local pairs = global.pairs
local pcall = global.pcall
local type = global.type
local require = global.require
local tostring = global.tostring

local Vehicles = module(...)

-- VehicleAIDirector c_nVehicleSubtype_* values
Vehicles.Subtype = {HelicopterACU = 0, CarRanger = 10}

-- Nearest ranger car (or ACU helicopter) to vPos that can be given orders, or nil and why not. bAnyState: any
-- such vehicle, orders or not (taking direct control needs no working facility; in the test park the game refuses
-- orders for every team, reason 2 = FoundationNotOperational, run 91).
Vehicles.FindNearestTeam = function(vPos, bHelicopter, bAnyState)
    if not vPos then return nil, "no position" end
    local tWorld = api.world.GetWorldAPIs()
    local director = tWorld and tWorld.VehicleAIDirector
    if not director then return nil, "no VehicleAIDirector world API" end
    local sTable = bHelicopter and "tHelicopters" or "tCars"
    local tVehicles = director[sTable]
    if type(tVehicles) ~= "table" then return nil, "VehicleAIDirector (" .. type(director) .. ") has no " .. sTable end
    local nWant = bHelicopter and Vehicles.Subtype.HelicopterACU or Vehicles.Subtype.CarRanger
    local bUtils, VehicleUtils = pcall(require, "Helpers.VehicleUtils")
    local vehiclesAPI = director.vehiclesAPI or tWorld.vehicles
    local x, y, z = vPos:GetX(), vPos:GetY(), vPos:GetZ()
    local best, nBest = nil, nil
    local nAll, nKind, nFree, sRefused = 0, 0, 0, nil
    local tSubtypes = {}
    for entity, tComponent in pairs(tVehicles) do
        nAll = nAll + 1
        if type(tComponent) == "table" then tSubtypes[tostring(tComponent.nSubtype)] = true end
        if type(tComponent) == "table" and tComponent.nSubtype == nWant then
            nKind = nKind + 1
            local bCan = true
            if not bAnyState and bUtils and VehicleUtils and VehicleUtils.CanVehicleBeIssuedTasks then
                local bOk, b, reason = pcall(VehicleUtils.CanVehicleBeIssuedTasks, entity, vehiclesAPI, director.civilEngAPI)
                bCan = bOk and b == true
                if not bCan and not sRefused then
                    sRefused = bOk and ("reason " .. tostring(reason)) or ("error: " .. tostring(b))
                end
            end
            if bCan then nFree = nFree + 1 end
            local t = bCan and api.transform.GetTransform(entity)
            if t then
                local p = t:GetPos()
                local dx, dy, dz = p:GetX() - x, p:GetY() - y, p:GetZ() - z
                local d = dx * dx + dy * dy + dz * dz
                if not nBest or d < nBest then best, nBest = entity, d end
            end
        end
    end
    if best then return best end
    local sSeen = ""
    for s in pairs(tSubtypes) do sSeen = sSeen .. " " .. s end
    return nil, sTable .. ": " .. nAll .. " vehicles (subtypes" .. sSeen .. "), " .. nKind .. " of subtype " .. nWant ..
                ", " .. nFree .. " can take orders" .. (sRefused and (" (first refusal: " .. sRefused .. ")") or "")
end
