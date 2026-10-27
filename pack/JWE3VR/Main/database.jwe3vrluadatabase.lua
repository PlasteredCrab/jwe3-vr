-----------------------------------------------------------------------
--/  @file   Database.JWE3VRLuaDatabase.lua
--/  @brief  Entry point of the JWE3 VR content pack. The game calls AddContentToCall on every
--/          content pack's Database.*LuaDatabase module (JWE2 behaviour; to verify on JWE3);
--/          that is where the mod installs its own module hooks (JWE3VR.Hooks). No ACSE needed.
--/          Requires JWE3VR.dll in this pack's folder.
-----------------------------------------------------------------------
local global = _G
local table = global.table
local require = global.require

local JWE3VRLuaDatabase = module(...)

JWE3VRLuaDatabase.AddContentToCall = function(_tContentToCall)
    table.insert(_tContentToCall, JWE3VRLuaDatabase)
    require("JWE3VR.Hooks").Install()
end
