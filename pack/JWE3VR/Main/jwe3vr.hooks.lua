-----------------------------------------------------------------------
--/  @file   JWE3VR.Hooks.lua
--/  @brief  The mod's own module hooks, so it needs no script extender (ACSE for JWE3 is
--/          unmaintained). A patch runs once per game module: when the module is first required,
--/          or straight away if it was loaded before the hooks were installed.
--/
--/          Verified against JWE3 build 1792088119 (decompiled Lua):
--/            * Database.Main calls AddContentToCall on Database.<Pack>LuaDatabase of every loaded
--/              pack during GameDatabase init at startup, before any park world exists.
--/            * Environment.Environment saved global.require when the game started, so it loads
--/              Environments.ParkEnvironment past our wrap. Its _MergePrototype (called through
--/              the module table) is wrapped to load the prototype through our require first; the
--/              park environment patch then adds the VR manager to EnvironmentPrototype.Managers,
--/              and _ExpandPrototypeModuleNames (run on every merge) resolves it.
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

-- Load every environment prototype through our require, so the park environment gets its patch
-- before the game merges and expands it.
local PatchEnvironmentLoader = function(tModule)
    local fnMerge = tModule._MergePrototype
    if type(fnMerge) ~= "function" then
        api.debug.Trace("JWE3VR: Environment._MergePrototype not found; the VR manager may be missing")
        return
    end
    tModule._MergePrototype = function(_sPrototypeModule, ...)
        if type(_sPrototypeModule) == "string" then pcall(global.require, _sPrototypeModule) end
        return fnMerge(_sPrototypeModule, ...)
    end
end

-- Canonical module name -> patch(tModule). Matching is case-insensitive (the game requires modules
-- in mixed case; the files are lower case).
Hooks.tPatches = {
    ["Environment.Environment"] = PatchEnvironmentLoader,
    ["Environments.ParkEnvironment"] = AddManager,
    -- Every camera mode ends in CameraModeManager.Advance -> SetTransform(main camera). Wrap it.
    ["Components.Cameras.CameraModeManager"] = function(tModule)
        global.require("JWE3VR.Camera").Patch(tModule)
    end,
    -- Sit in the driver's seat (first-person camera) whenever a vehicle is entered.
    ["Editors.Vehicle.DriveVehicleUIMode"] = function(tModule)
        global.require("JWE3VR.Gameplay").PatchDriveMode(tModule)
    end,
    -- Track the active gun seat (its vehicle anchors the view; its _SwitchAmmo keeps the HUD in sync).
    ["Editors.Vehicle.VehicleGunnerUIMode"] = function(tModule)
        global.require("JWE3VR.Gameplay").PatchGunnerMode(tModule)
    end,
}

Hooks.tByKey = {}
for sName, fn in pairs(Hooks.tPatches) do Hooks.tByKey[string.lower(sName)] = {sName = sName, fn = fn} end
Hooks.tDone = {}

local Apply = function(sName, tModule)
    if type(sName) ~= "string" or type(tModule) ~= "table" then return end
    local sKey = string.lower(sName)
    local tHook = Hooks.tByKey[sKey]
    if not tHook or Hooks.tDone[sKey] then return end
    Hooks.tDone[sKey] = true
    local bOk, sErr = pcall(tHook.fn, tModule)
    if not bOk then api.debug.Trace("JWE3VR: patching " .. sName .. " failed: " .. tostring(sErr)) end
end

-- Patches every hooked module that is already loaded (walks all of package.loaded).
Hooks.Sweep = function()
    if not package or type(package.loaded) ~= "table" then return end
    for sName, tModule in pairs(package.loaded) do
        Apply(sName, tModule)
    end
end

-- Cheap per-frame catch-up: direct lookups of the hooks not applied yet. Returns true when none remain.
Hooks.SweepPending = function()
    if not package or type(package.loaded) ~= "table" then return true end
    local bAll = true
    for sKey, tHook in pairs(Hooks.tByKey) do
        if not Hooks.tDone[sKey] then
            Apply(tHook.sName, package.loaded[tHook.sName] or package.loaded[sKey])
            if not Hooks.tDone[sKey] then bAll = false end
        end
    end
    return bAll
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
