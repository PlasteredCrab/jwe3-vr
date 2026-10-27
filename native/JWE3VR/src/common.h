// Shared includes for JWE3VR.dll. Everything runs inside JWE3.exe (x64, D3D12).
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace jvr {

// Directory that holds JWE3VR.dll, JWE3VR.ini and JWE3VR.log (Win64\ovldata\JWE3VR\).
const std::wstring& ModuleDir();

template <typename T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace jvr
