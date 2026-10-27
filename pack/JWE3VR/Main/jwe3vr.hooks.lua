-----------------------------------------------------------------------
--/  @file   JWE3VR.Hooks.lua
--/  @brief  The mod's own module hooks, so it needs no script extender (ACSE for JWE3 is
--/          unmaintained). A patch runs once per game module: when the module is first required,
--/          or straight away if it was loaded before the hooks were installed. The park manager
--/          is added by patching the park environment's prototype, as ACSE did on JWE2.
--/
--/          UNVERIFIED on JWE3: module names, EnvironmentPrototype.Managers, and whether
--/          the game calls our database's AddContentToCall before the park modules load.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local pairs = global.pairs
local pcall = global.pcall
local type = global.type
local tostring = global.tostring
local string = global.string
local package = global.package

local Hooks = module(...)

local kManager = "Managers.JWE3VRManager"

local AddManager = function(tModule)
    local tProto = tModule.EnvironmentPrototype
    local tManagers = tProto and tProto.Managers
    if type(tManagers) ~= "table" then
        api.debug.Trace("JWE3VR: park environment has no EnvironmentPrototype.Managers; manager not added")
        return
    end
    if tManagers[kManager] == nil then tManagers[kManager] = {} end
    api.debug.Trace("JWE3VR: manager added to the park environment")
end

-- Lower-case module name -> patch(tModule). Names are matched case-insensitively (the game
-- requires modules in mixed case; the files are lower case).
Hooks.tPatches = {
    ["environments.parkenvironment"] = AddManager,
    -- Every camera mode ends in CameraModeManager.Advance -> SetTransform(main camera). Wrap it.
    ["components.cameras.cameramodemanager"] = function(tModule)
        global.require("JWE3VR.Camera").Patch(tModule)
    end,
    -- Sit in the driver's seat (first-person camera) whenever a vehicle is entered.
    ["editors.vehicle.drivevehicleuimode"] = function(tModule)
        global.require("JWE3VR.Gameplay").PatchDriveMode(tModule)
    end,
    -- Track the active gun seat (its vehicle anchors the view; its _SwitchAmmo keeps the HUD in sync).
    ["editors.vehicle.vehiclegunneruimode"] = function(tModule)
        global.require("JWE3VR.Gameplay").PatchGunnerMode(tModule)
    end,
}

Hooks.tDone = {}

local Apply = function(sName, tModule)
    if type(sName) ~= "string" or type(tModule) ~= "table" then return end
    local sKey = string.lower(sName)
    local fn = Hooks.tPatches[sKey]
    if not fn or Hooks.tDone[sKey] then return end
    Hooks.tDone[sKey] = true
    local bOk, sErr = pcall(fn, tModule)
    if not bOk then api.debug.Trace("JWE3VR: patching " .. sName .. " failed: " .. tostring(sErr)) end
end

-- Patches every hooked module that is already loaded.
Hooks.Sweep = function()
    if not package or type(package.loaded) ~= "table" then return end
    for sName, tModule in pairs(package.loaded) do
        Apply(sName, tModule)
    end
end

local Wrap = function(sGlobal)
    local fnOrig = global[sGlobal]
    if type(fnOrig) ~= "function" then return end
    global[sGlobal] = function(sName, ...)
        local tModule = fnOrig(sName, ...)
        Apply(sName, tModule)
        return tModule
    end
end

Hooks.Install = function()
    if Hooks.bInstalled then return end
    Hooks.bInstalled = true
    Wrap("require")
    Wrap("tryrequire")
    Hooks.Sweep()
    api.debug.Trace("JWE3VR: hooks installed")
end
