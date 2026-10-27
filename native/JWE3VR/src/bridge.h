// The Lua bridge: one C function the game's Lua gets with
//   local vr = package.loadlib(".\\Win64\\ovldata\\JWE3VR\\JWE3VR.dll", "jwe3vr_lua")
// and calls as vr(op, ...) -> numbers. Op codes are listed in bridge.cpp and mirrored in
// pack/JWE3VR/Main/jwe3vr.bridge.lua.
//
// It never links Lua. It reads number arguments and pushes number results directly on the
// game's Lua 5.3 stack, using the layout seen in the decompiled bindings (JWE2 build 1749823099;
// NOT yet verified on JWE3):
//   L+0x10 = top, L+0x20 = CallInfo* (its first field = func slot), 24-byte stack values,
//   float32 value at +0 with tag 3, int value at +0 with tag 0x13, tag at +0x10;
//   every push also bumps a global stack-op counter and stores it at L-0x10.
// Before touching the stack it checks that the func slot holds jwe3vr_lua itself as a light C
// function (tag 0x16); any other layout makes every call return nothing, so VR stays off.
#pragma once

#include <cstdint>

namespace jvr::bridge {

// Finds the stack-op counter in JWE3.exe's code (optional; pushes still work without it).
void Init();

} // namespace jvr::bridge

extern "C" __declspec(dllexport) int jwe3vr_lua(void* L);
