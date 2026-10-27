// Calls the built JWE3VR.dll's jwe3vr_lua on a fake lua_State laid out like JWE2's (24-byte stack
// values, tag at +0x10, top at L+0x10, CallInfo* at L+0x20, header field at L-0x10). Outside
// JWE3.exe the DLL installs no hooks, so this runs anywhere. Exit code 0 = all passed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using LuaFn = int (*)(void*);

static int g_fail = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                              \
        }                                                          \
    } while (0)

struct FakeLua {
    alignas(16) uint8_t block[0x200]{};  // [0x10 header][lua_State...]
    alignas(16) uint8_t stack[0x18 * 64]{};
    uint8_t* ciFunc = nullptr;           // CallInfo: first field = func slot
    void* self = nullptr;                // the bridge function (slot 0, light C function)
    uint8_t* L() { return block + 0x10; }

    void Begin() {
        memset(stack, 0, sizeof(stack));
        ciFunc = stack;                       // slot 0 = the C function itself
        *(void**)stack = self;
        *(int*)(stack + 0x10) = 0x16;
        *(uint8_t**)(L() + 0x20) = (uint8_t*)&ciFunc;
        *(uint8_t**)(L() + 0x10) = stack + 0x18;
    }
    void PushInt(int v) {
        uint8_t*& top = *(uint8_t**)(L() + 0x10);
        *(int32_t*)top = v;
        *(int*)(top + 0x10) = 0x13;
        top += 0x18;
    }
    void PushFloat(float v) {
        uint8_t*& top = *(uint8_t**)(L() + 0x10);
        *(float*)top = v;
        *(int*)(top + 0x10) = 3;
        top += 0x18;
    }
    // Result i (0-based) of n results ending at top.
    float Result(int n, int i, int* tag = nullptr) {
        uint8_t* top = *(uint8_t**)(L() + 0x10);
        uint8_t* s = top - 0x18 * (n - i);
        if (tag) *tag = *(int*)(s + 0x10);
        return *(float*)s;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: bridge_test <path to JWE3VR.dll>\n");
        return 2;
    }
    HMODULE m = LoadLibraryA(argv[1]);
    CHECK(m != nullptr);
    if (!m) return 1;
    auto fn = (LuaFn)GetProcAddress(m, "jwe3vr_lua");
    CHECK(fn != nullptr);
    if (!fn) return 1;
    Sleep(200); // let the DLL's init thread load the config

    FakeLua f;
    f.self = (void*)fn;
    // Status: protocol 1 and three more numbers, all floats.
    f.Begin();
    f.PushInt(0);
    int n = fn(f.L());
    CHECK(n == 4);
    int tag = 0;
    CHECK(f.Result(n, 0, &tag) == 1.f && tag == 3);

    // Config index 1 = Rig.OverviewScale (default 30).
    f.Begin();
    f.PushInt(9);
    f.PushInt(1);
    n = fn(f.L());
    CHECK(n == 1 && f.Result(n, 0) > 0.f);

    // Tick without a headset: no override, but all 11 results are pushed.
    f.Begin();
    f.PushInt(1);
    f.PushInt(1);
    f.PushFloat(0.016f);
    float args[12] = {1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (float a : args) f.PushFloat(a);
    n = fn(f.L());
    CHECK(n == 11 && f.Result(n, 0) == 0.f);

    // A malformed stack (top before func) must be refused without touching anything.
    f.Begin();
    *(uint8_t**)(f.L() + 0x10) = f.stack;
    n = fn(f.L());
    CHECK(n == 0);

    // A different layout (func slot not holding the bridge as a light C function) is refused too.
    f.Begin();
    *(int*)(f.stack + 0x10) = 0x26;
    f.PushInt(0);
    uint8_t* before = *(uint8_t**)(f.L() + 0x10);
    n = fn(f.L());
    CHECK(n == 0 && *(uint8_t**)(f.L() + 0x10) == before);

    if (g_fail == 0) printf("bridge checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
