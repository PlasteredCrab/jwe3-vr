// Thin wrapper over MinHook. Every hook is installed by target address so the same code hooks
// exported functions (xinput1_4!XInputGetState) and COM vtable entries of the real system
// d3d12/dxgi objects (beneath NVIDIA Streamline's interposer).
#pragma once

namespace jvr::hooks {

bool Init();
void Shutdown();

// Hooks `target`, stores the trampoline in *original. Logs and returns false on failure.
bool Create(const char* name, void* target, void* detour, void** original);

// Returns the function pointer stored in slot `index` of a COM object's vtable.
inline void* VtableEntry(void* object, int index) { return (*reinterpret_cast<void***>(object))[index]; }

} // namespace jvr::hooks
