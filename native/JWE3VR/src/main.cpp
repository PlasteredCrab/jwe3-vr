// JWE3VR.dll entry. Loaded at process start by the version.dll proxy in the game folder, and again
// (same module, refcount only) by the JWE3VR content pack's Lua through package.loadlib.
#include "bridge.h"
#include "common.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "render.h"
#include "scan.h"

#include <dbghelp.h>

namespace jvr {

static HMODULE g_module = nullptr;
static std::wstring g_dir;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

const std::wstring& ModuleDir() {
    if (g_dir.empty()) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(g_module, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    }
    return g_dir;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static bool once = false;
    if (!once) {
        once = true;
        LOGE("crash: exception %08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        std::wstring path = ModuleDir() + L"JWE3VR_crash.dmp";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithIndirectlyReferencedMemory, &mi, nullptr,
                              nullptr);
            CloseHandle(f);
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI InitThread(void*) {
    log::Open();
    LOGI("JWE3VR starting (game build %u%s)", scan::GameBuild(),
         scan::GameBuild() == scan::kKnownBuild ? "" : " -- not the build the mod was made on; features may switch themselves off");
    config::Load();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    // The JWE3 exe name is not verified yet (Phase 0); accept JWE3*.exe.
    if (!name || _wcsnicmp(name + 1, L"JWE3", 4) != 0) {
        LOGI("not running inside JWE3*.exe; hooks stay off (bridge only, for tests)");
        return 0;
    }
    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
    if (!config::Get().enabled) {
        LOGI("disabled in JWE3VR.ini");
        return 0;
    }
    // openxr_loader.dll is delay-loaded from our own folder, not the game's. Without it the first
    // OpenXR call would raise a delay-load exception inside Present, so no render hooks at all.
    if (!LoadLibraryW((ModuleDir() + L"openxr_loader.dll").c_str())) {
        LOGE("openxr_loader.dll missing next to JWE3VR.dll; VR stays off");
        return 0;
    }
    bridge::Init();
    render::InstallHooks();
    input::InstallHooks();
    LOGI("ready; waiting for the game's first frame and the JWE3VR content pack");
    return 0;
}

} // namespace jvr

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        jvr::g_module = module;
        DisableThreadLibraryCalls(module);
        // Pin: Lua's package.loadlib may FreeLibrary on error paths; hooks must outlive it.
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&DllMain, &pinned);
        if (HANDLE t = CreateThread(nullptr, 0, jvr::InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
