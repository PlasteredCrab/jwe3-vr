// Code scanning in the game exe. Only real code sections are scanned (in the JWE2 Denuvo build the
// code was the executable section misnamed ".bss"); a VM blob over 128 MB is skipped by size.
#pragma once

#include <cstdint>
#include <functional>

namespace jvr::scan {

// PE timestamp of the JWE3 exe the RE notes were made on. 0 until Phase 0 has run on a JWE3 install
// (JWE2 VR's was 1749823099); the log then always says "not the build the mod was made on".
constexpr uint32_t kKnownBuild = 0;
uint32_t GameBuild();

// Pattern like "8B 0D ?? ?? ?? ?? FF C1". Calls fn for every match in executable sections < 128 MB.
void ForEachPattern(const char* pattern, const std::function<void(uint8_t*)>& fn);
uint8_t* FindPattern(const char* pattern);

} // namespace jvr::scan
