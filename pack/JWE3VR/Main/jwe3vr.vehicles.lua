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

local Vehicles = module(...)

-- VehicleAIDirector c_nVehicleSubtype_* values
Vehicles.Subtype = {HelicopterACU = 0, CarRanger = 10}

-- Nearest ranger car (or ACU helicopter) to vPos that can be given orders, or nil.
Vehicles.FindNearestTeam = function(vPos, bHelicopter)
    if not vPos then return nil end
    local tWorld = api.world.GetWorldAPIs()
    local director = tWorld and tWorld.VehicleAIDirector
    if not director then return nil end
    local tVehicles = bHelicopter and director.tHelicopters or director.tCars
    if type(tVehicles) ~= "table" then return nil end
    local nWant = bHelicopter and Vehicles.Subtype.HelicopterACU or Vehicles.Subtype.CarRanger
    local bUtils, VehicleUtils = pcall(require, "Helpers.VehicleUtils")
    local vehiclesAPI = director.vehiclesAPI or tWorld.vehicles
    local x, y, z = vPos:GetX(), vPos:GetY(), vPos:GetZ()
    local best, nBest = nil, nil
    for entity, tComponent in pairs(tVehicles) do
        if type(tComponent) == "table" and tComponent.nSubtype == nWant then
            local bCan = true
            if bUtils and VehicleUtils and VehicleUtils.CanVehicleBeIssuedTasks then
                local bOk, b = pcall(VehicleUtils.CanVehicleBeIssuedTasks, entity, vehiclesAPI, director.civilEngAPI)
                bCan = bOk and b == true
            end
            local t = bCan and api.transform.GetTransform(entity)
            if t then
                local p = t:GetPos()
                local dx, dy, dz = p:GetX() - x, p:GetY() - y, p:GetZ() - z
                local d = dx * dx + dy * dy + dz * dz
                if not nBest or d < nBest then best, nBest = entity, d end
            end
        end
    end
    return best
end
