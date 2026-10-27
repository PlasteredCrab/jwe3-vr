// Code scanning in JWE3.exe. Only real code sections are scanned (JWE3: ".code", 38.7 MB); the
// Denuvo VM blob (JWE3: executable ".shared", 252 MB) is skipped by size.
#pragma once

#include <cstdint>
#include <functional>

namespace jvr::scan {

// PE timestamp of the JWE3.exe the RE notes were made on (Steam build 25967550, 2026-10-15).
constexpr uint32_t kKnownBuild = 1792088119;
uint32_t GameBuild();

// Pattern like "8B 0D ?? ?? ?? ?? FF C1". Calls fn for every match in executable sections < 128 MB.
void ForEachPattern(const char* pattern, const std::function<void(uint8_t*)>& fn);
uint8_t* FindPattern(const char* pattern);

} // namespace jvr::scan
