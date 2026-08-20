#include <windows.h>

#include "dll/Velyx.hpp"

namespace {

HMODULE g_self = nullptr;

// Everything runs off the loader lock. Creating a D3D device or touching the
// filesystem from DllMain deadlocks the game.
DWORD WINAPI bootstrapThread(LPVOID) {
    velyx::Velyx::get().start(g_self);

    while (velyx::Velyx::get().running()) {
        Sleep(200);
    }

    FreeLibraryAndExitThread(g_self, 0);
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = module;
            DisableThreadLibraryCalls(module);

            const HANDLE thread = CreateThread(nullptr, 0, &bootstrapThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            break;
        }

        case DLL_PROCESS_DETACH:

            if (!reserved && velyx::Velyx::get().running()) {
                velyx::Velyx::get().requestEject();
            }
            break;

        default:
            break;
    }

    return TRUE;
}
