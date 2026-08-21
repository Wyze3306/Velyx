#include "GameInputHook.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include <MinHook.h>

#include "core/Log.hpp"
#include "dll/hook/hooks/WindowHook.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "GameInput";

// Only what is needed to reach two methods, rather than a copy of the SDK header.
// After IUnknown come GetCurrentTimestamp, then the two readings — and that start is
// the same in v0, v1 and v2 of the interface, so which one the game asked for cannot
// land this on the wrong method. (Checked against the GameInput header: the versions
// only diverge further down, where v0 keeps a GetTemporalReading the others dropped.)
constexpr size_t kGetCurrentReadingIndex = 4;
constexpr size_t kGetNextReadingIndex = 5;
constexpr size_t kMethodCount = 8;

using GameInputKind = uint32_t;
constexpr GameInputKind kKeyboard = 0x10;
constexpr GameInputKind kMouse = 0x20;

// What GameInput itself returns when nothing new has arrived.
constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838a0003);

struct IGameInput;
struct IGameInputDevice;
struct IGameInputReading;

using GameInputCreateFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput**);

using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind,
                                                        IGameInputDevice*, IGameInputReading**);
using GetNextReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, IGameInputReading*, GameInputKind,
                                                     IGameInputDevice*, IGameInputReading**);

GetCurrentReadingFn g_originalCurrentReading = nullptr;
GetNextReadingFn g_originalNextReading = nullptr;

// The controller is left alone: someone playing on a pad has no cursor to take away,
// and the interface is not driven by one.
bool blocked(GameInputKind kind) {
    return WindowHook::captureInput() && (kind & (kKeyboard | kMouse)) != 0;
}

HRESULT STDMETHODCALLTYPE currentReadingDetour(IGameInput* self, GameInputKind kind,
                                               IGameInputDevice* device,
                                               IGameInputReading** reading) {
    if (blocked(kind)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }
    return g_originalCurrentReading(self, kind, device, reading);
}

HRESULT STDMETHODCALLTYPE nextReadingDetour(IGameInput* self, IGameInputReading* reference,
                                            GameInputKind kind, IGameInputDevice* device,
                                            IGameInputReading** reading) {
    if (blocked(kind)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }
    return g_originalNextReading(self, reference, kind, device, reading);
}

// The game reaches GameInput through the redistributable, which the loader has
// already brought in by the time the client starts. Nothing is loaded here that the
// process was not going to load anyway.
HMODULE findGameInput() {
    for (const wchar_t* name : {L"GameInputRedist.dll", L"gameinput.dll"}) {
        if (const HMODULE module = GetModuleHandleW(name)) return module;
    }
    return nullptr;
}

}

GameInputHook::GameInputHook() : Hook("gameinput", 0) {}

bool GameInputHook::install() {
    const HMODULE module = findGameInput();
    if (!module) {
        Log::info(kLog, "not in use by this game; the window messages are the whole story");
        return false;
    }

    const auto create =
        reinterpret_cast<GameInputCreateFn>(GetProcAddress(module, "GameInputCreate"));
    if (!create) {
        Log::warn(kLog, "GameInputCreate is missing, input will reach the game behind the menu");
        return false;
    }

    // An instance of our own, only to read the vtable off it: every IGameInput in the
    // process shares it, the game's included.
    IGameInput* probe = nullptr;
    const HRESULT hr = create(&probe);
    if (FAILED(hr) || !probe) {
        Log::warn(kLog, "GameInputCreate failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    void* methods[kMethodCount]{};
    std::memcpy(methods, *reinterpret_cast<void***>(probe), sizeof(methods));

    // Release through the vtable: the interface is opaque here on purpose.
    using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IGameInput*);
    reinterpret_cast<ReleaseFn>(methods[2])(probe);

    currentReadingTarget_ = methods[kGetCurrentReadingIndex];
    nextReadingTarget_ = methods[kGetNextReadingIndex];

    const bool ok =
        createAt(currentReadingTarget_, reinterpret_cast<void*>(&currentReadingDetour),
                 reinterpret_cast<void**>(&g_originalCurrentReading)) &&
        createAt(nextReadingTarget_, reinterpret_cast<void*>(&nextReadingDetour),
                 reinterpret_cast<void**>(&g_originalNextReading));

    installed_ = ok;
    if (ok) Log::info(kLog, "readings will pause while an interface is open");
    return ok;
}

void GameInputHook::uninstall() {
    for (void* target : {currentReadingTarget_, nextReadingTarget_}) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }

    currentReadingTarget_ = nextReadingTarget_ = nullptr;
    installed_ = false;
}

}
