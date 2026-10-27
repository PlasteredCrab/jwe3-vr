// version.dll proxy for the JWE3 exe. JWE2.exe imported three functions from version.dll (whether
// JWE3 does is a Phase 0 check); this proxy forwards all 17 exports to the real System32 copy and
// loads JWE3VR.dll from the content pack folder (Win64\ovldata\JWE3VR\JWE3VR.dll). Only inside
// JWE3*.exe: the crash reporter in the same folder gets a plain forwarder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>

static HMODULE g_real = nullptr;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\version.dll");
    g_real = LoadLibraryW(path);
    return TRUE;
}

static FARPROC Real(const char* name) {
    InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr);
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

// Every version.dll export takes at most 8 pointer-sized arguments on x64; extra ones are ignored.
using Fwd = UINT_PTR(WINAPI*)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);

#define FORWARD(name)                                                                                           \
    extern "C" UINT_PTR WINAPI P_##name(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d, UINT_PTR e, UINT_PTR f, \
                                        UINT_PTR g, UINT_PTR h) {                                               \
        static Fwd fn = (Fwd)Real(#name);                                                                       \
        return fn ? fn(a, b, c, d, e, f, g, h) : 0;                                                             \
    }

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoByHandle)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)

static void LoadMod() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (!name || _wcsnicmp(name + 1, L"JWE3", 4) != 0) return; // JWE3*.exe (name unverified)
    wchar_t* slash = wcsrchr(exe, L'\\');
    *slash = 0;
    wchar_t dll[MAX_PATH];
    swprintf_s(dll, L"%s\\Win64\\ovldata\\JWE3VR\\JWE3VR.dll", exe);
    if (GetFileAttributesW(dll) != INVALID_FILE_ATTRIBUTES) LoadLibraryW(dll);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        LoadMod();
    }
    return TRUE;
}
