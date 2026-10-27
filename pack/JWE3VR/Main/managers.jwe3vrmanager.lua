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
    -- Catch hooked modules that were loaded through a require captured before ours was installed.
    require("JWE3VR.Hooks").Sweep()
    self.bReady = Bridge.Load()
    if not self.bReady then return end
    local bOk, sErr = pcall(Gameplay.Init, _tEnvironment)
    if not bOk then api.debug.Trace("JWE3VR gameplay init: " .. tostring(sErr)) end

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
    local bOk, sErr = pcall(Gameplay.Advance, _nUnscaledDeltaTime)
    if not bOk and not self.bErrorLogged then
        self.bErrorLogged = true
        api.debug.Trace("JWE3VR gameplay: " .. tostring(sErr))
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
