-----------------------------------------------------------------------
--/  @file   Managers.JWE3VRManager.lua
--/  @brief  Park manager added to the park environment by JWE3VR.Hooks. Loads the VR bridge when
--/          a park loads, then runs the VR gameplay layer every frame. The camera itself is driven
--/          from the patched CameraModeManager (JWE3VR.Camera), which runs whether or not this
--/          manager exists.
-----------------------------------------------------------------------
local global = _G
local api = global.api
local pcall = global.pcall
local tostring = global.tostring
local require = global.require
local module = global.module

local Mutators = require("Environment.ModuleMutators")
local Bridge = require("JWE3VR.Bridge")
local Gameplay = require("JWE3VR.Gameplay")

local JWE3VRManager = module(..., Mutators.Manager())

JWE3VRManager.Init = function(self, _tProperties, _tEnvironment)
    api.debug.Trace("JWE3VRManager:Init()")
    self.nPendingTimer = 0
    self.bHooksDone = false
    -- A park runs in its own Lua VM: load the bridge here first, so what the sweep patches gets logged.
    self.bReady = Bridge.Load()
    -- Catch hooked modules that were loaded through a require captured before ours was installed.
    local Hooks = require("JWE3VR.Hooks")
    Hooks.Sweep()
    if not self.bReady then return end
    Bridge.Log(Bridge.M.ManagerInit)
    local tState = {}
    for sKey, tHook in global.pairs(Hooks.tByKey) do
        tState[#tState + 1] = tHook.sName .. "=" .. (Hooks.tDone[sKey] and "patched" or "pending")
    end
    Bridge.Text("hooks in this VM: installed=" .. tostring(Hooks.bInstalled == true) .. "; " .. global.table.concat(tState, ", "))
    -- Which hooked modules this Lua state can see (diagnostic for the first JWE3 runs).
    local tLoaded = global.package and global.package.loaded or {}
    for _, sName in global.ipairs({"Components.Cameras.CameraModeManager", "Environments.ParkEnvironment",
                                   "Editors.Vehicle.DriveVehicleUIMode", "Environment.Environment"}) do
        local t = tLoaded[sName] or tLoaded[global.string.lower(sName)]
        Bridge.Text("manager init: package.loaded " .. sName .. " = " .. global.type(t))
    end
    local bOk, sErr = pcall(Gameplay.Init, _tEnvironment)
    if bOk then
        Bridge.Log(Bridge.M.GameplayInit)
    else
        api.debug.Trace("JWE3VR gameplay init: " .. tostring(sErr))
        Bridge.Log(Bridge.M.LuaError, 1)
        Bridge.Text("gameplay init failed: " .. tostring(sErr))
    end

    self.tReceivers = {}
    local messaging = api.messaging
    if messaging and messaging.MsgType_WeaponFiredMessage then
        local fn = function() pcall(Gameplay.OnWeaponFired) end
        messaging.RegisterReceiver(messaging.MsgType_WeaponFiredMessage, fn)
        self.tReceivers[messaging.MsgType_WeaponFiredMessage] = fn
    end
end

JWE3VRManager.Advance = function(self, _nDeltaTime, _nUnscaledDeltaTime)
    if not self.bReady then return end
    -- JWE3 environments pass only the (scaled) delta time to the Advance phase.
    _nUnscaledDeltaTime = _nUnscaledDeltaTime or _nDeltaTime or 0
    -- Diagnostics for the first JWE3 runs (all protected: an error here must not stop Advance).
    if not self.bFirstAdvanceLogged then
        self.bFirstAdvanceLogged = true
        pcall(function()
            local co, bMain = global.coroutine.running()
            Bridge.Text("manager Advance: first call, dt " .. tostring(_nDeltaTime) .. ", coroutine " ..
                        tostring(co ~= nil and not bMain) .. ", startup globals visible " .. tostring(global.__JWE3VR_StartupToken))
        end)
    end
    -- Drive the camera from here when no camera wrapper ever runs (JWE3: camera component out of reach).
    local Camera = require("JWE3VR.Camera")
    if not Camera.bModeDriven and not Camera.bManagerDriven then
        local bOk, sErr = pcall(Camera.ManagerTick, _nUnscaledDeltaTime)
        if not bOk and not self.bManagerCameraError then
            self.bManagerCameraError = true
            Bridge.Text("camera (manager path) failed: " .. tostring(sErr))
        end
    end
    self.nDiagFrames = (self.nDiagFrames or 0) + 1 -- frames, not time: a paused park passes dt 0
    if not self.bCameraDiag and self.nDiagFrames > 300 then
        self.bCameraDiag = true
        local bOk, sErr = pcall(function()
            local Camera = require("JWE3VR.Camera")
            local bW, tWorld = pcall(api.world.GetWorldAPIs)
            local cmm = bW and global.type(tWorld) == "table" and tWorld.CameraModeManager or nil
            local sCmm = global.type(cmm)
            local sEntities = sCmm == "table" and global.type(global.rawget(cmm, "tEntities")) or "n/a"
            Bridge.Text("camera diag: mode Advance calls " .. tostring(Camera.nModeCalls or 0) .. ", last skip " ..
                        tostring(Camera.sSkip) .. ", driven " .. tostring(Camera.bModeDriven) .. ", mode " ..
                        tostring(Camera.sMode) .. "; world CameraModeManager is " .. sCmm .. ", tEntities " .. sEntities ..
                        "; manager-path ticks " .. tostring(Camera.nTicks or 0) .. ", of which read back our own write " ..
                        tostring(Camera.nOwnReads or 0))
        end)
        if not bOk then Bridge.Text("camera diag failed: " .. tostring(sErr)) end
    end
    -- Hooked modules loaded after park start (UI modes on first use) through a captured require.
    if not self.bHooksDone then
        self.nPendingTimer = self.nPendingTimer - (_nUnscaledDeltaTime or 0)
        if self.nPendingTimer <= 0 then
            self.nPendingTimer = 1
            self.bHooksDone = require("JWE3VR.Hooks").SweepPending()
        end
    end
    local bOk, sErr = pcall(Gameplay.Advance, _nUnscaledDeltaTime)
    if not bOk and not self.bErrorLogged then
        self.bErrorLogged = true
        api.debug.Trace("JWE3VR gameplay: " .. tostring(sErr))
        Bridge.Log(Bridge.M.LuaError, 2)
        Bridge.Text("gameplay advance failed: " .. tostring(sErr))
    end
end

JWE3VRManager.Shutdown = function(self)
    if self.tReceivers then
        for nType, fn in global.pairs(self.tReceivers) do api.messaging.UnregisterReceiver(nType, fn) end
        self.tReceivers = nil
    end
    pcall(Gameplay.Shutdown)
end

Mutators.VerifyManagerModule(JWE3VRManager)
