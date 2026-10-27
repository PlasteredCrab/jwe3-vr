#include "hooks.h"
#include "common.h"
#include "log.h"

#include "MinHook.h"

namespace jvr::hooks {

static bool g_ready = false;

bool Init() {
    if (g_ready) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    g_ready = true;
    return true;
}

void Shutdown() {
    if (!g_ready) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_ready = false;
}

bool Create(const char* name, void* target, void* detour, void** original) {
    if (!target) {
        LOGE("hook %s: no target", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_ERROR_ALREADY_CREATED) {
        LOGW("hook %s: already hooked at %p", name, target);
        return true;
    }
    if (s != MH_OK) {
        LOGE("hook %s at %p: create failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOGE("hook %s at %p: enable failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    LOGI("hooked %s at %p", name, target);
    return true;
}

} // namespace jvr::hooks
