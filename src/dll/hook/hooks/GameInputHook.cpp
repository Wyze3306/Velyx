#include "GameInputHook.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <string_view>

#include <MinHook.h>

#include "core/Log.hpp"
#include "dll/event/EventBus.hpp"
#include "dll/event/Events.hpp"
#include "dll/hook/Turn.hpp"
#include "dll/hook/hooks/WindowHook.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "GameInput";

// Only what is needed to reach a handful of methods, rather than a copy of the SDK
// header. On IGameInput, after IUnknown come GetCurrentTimestamp and then the two
// readings — and that start is the same in v0, v1 and v2 of the interface, so which
// one the game asked for cannot land this on the wrong method. (Checked against the
// GameInput header: the versions only diverge further down, where v0 keeps a
// GetTemporalReading the others dropped.)
constexpr size_t kGetCurrentReadingIndex = 4;
constexpr size_t kGetNextReadingIndex = 5;
constexpr size_t kInputMethodCount = 8;

// IGameInputReading is one interface for every kind of device, so it is one class with
// one vtable, and every version of the header lists its methods in the same order:
// what the reading is, then a pair of methods per device. GetInputKind is asked first
// and has to answer with the kind the reading was fetched for — a version that moved
// its methods, or a pointer that is not a reading at all, does not.
constexpr size_t kGetInputKindIndex = 3;
constexpr size_t kGetKeyCountIndex = 14;
constexpr size_t kGetKeyStateIndex = 15;
constexpr size_t kGetMouseStateIndex = 16;

using GameInputKind = uint32_t;
constexpr GameInputKind kKeyboard = 0x10;
constexpr GameInputKind kMouse = 0x20;

// What GameInput itself returns when nothing new has arrived.
constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838a0003);

struct IGameInput;
struct IGameInputDevice;
struct IGameInputReading;

// GameInputMouseState, field for field. The buttons are a bitfield of what is held;
// the four counters are running totals, which the game turns into deltas itself.
struct MouseState {
    uint32_t buttons;
    int64_t positionX;
    int64_t positionY;
    int64_t wheelX;
    int64_t wheelY;
};
static_assert(sizeof(MouseState) == 40, "GameInputMouseState is not laid out as expected");

using GameInputCreateFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput**);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);

using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind,
                                                        IGameInputDevice*, IGameInputReading**);
using GetNextReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, IGameInputReading*, GameInputKind,
                                                     IGameInputDevice*, IGameInputReading**);

using GetInputKindFn = GameInputKind(STDMETHODCALLTYPE*)(IGameInputReading*);
using GetKeyCountFn = uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*);
using GetKeyStateFn = uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, void*);
using GetMouseStateFn = bool(STDMETHODCALLTYPE*)(IGameInputReading*, MouseState*);

GetCurrentReadingFn g_originalCurrentReading = nullptr;
GetNextReadingFn g_originalNextReading = nullptr;

GetKeyCountFn g_originalKeyCount = nullptr;
GetKeyStateFn g_originalKeyState = nullptr;
GetMouseStateFn g_originalMouseState = nullptr;

// The three readers above, patched. Until they are, a reading cannot be emptied and
// has to be withheld whole instead.
std::atomic<bool> g_emptying{false};
std::atomic<void**> g_readingVtable{nullptr};
std::mutex g_patchMutex;
void* g_readingTargets[3]{};

// GameInput hands the mouse over as a running total rather than a delta, so holding
// the counters still is not enough on its own: everything the mouse did behind the
// menu would land on the game in one jump the moment the menu closes. What the game is
// told is therefore kept as a total of its own, advanced by the delta Velyx decided on
// rather than by the one the device counted. Behind a menu that delta is zero, which
// freezes the counter without ever making it jump; in play it is whatever the handlers
// left in the event.
//
// Everything below runs on the thread that collects the readings, and only ever on
// that one — GameInput delivers a reading to whoever asked for it. The single
// exception is the injection, which another thread writes and this one drains, and it
// is the only field here that needs to be atomic for its own sake.
int64_t g_counted[4]{};
int64_t g_reported[4]{};
bool g_seeded = false;

void steadyMouse(MouseState& state, bool capturing) {
    int64_t* const counters[]{&state.positionX, &state.positionY, &state.wheelX, &state.wheelY};

    // The first reading sets the mark rather than moving it. Starting the reported
    // total anywhere else than where the device already is would hand the game one
    // enormous delta for a mouse that has not moved.
    if (!g_seeded) {
        for (size_t i = 0; i < 4; ++i) g_counted[i] = g_reported[i] = *counters[i];
        g_seeded = true;
        if (capturing) state.buttons = 0;
        return;
    }

    int64_t delta[4];
    for (size_t i = 0; i < 4; ++i) {
        const int64_t counted = *counters[i];
        delta[i] = counted - g_counted[i];
        g_counted[i] = counted;
    }

    if (capturing) {
        // Nothing the player did behind the menu reaches the game, and nothing a
        // module asks for does either: a turn belongs to play, not to a menu.
        for (int64_t& d : delta) d = 0;
        turn::forget();
        state.buttons = 0;
    } else {
        turn::shape(delta[0], delta[1]);
    }

    for (size_t i = 0; i < 4; ++i) {
        g_reported[i] += delta[i];
        *counters[i] = g_reported[i];
    }
}

// Said once. Together with the poll below it names how the game comes by its
// readings: emptied without a poll ever being logged means it is handed them by
// callback instead of asking for them.
void noteEmptied() {
    static std::atomic<bool> said{false};
    if (said.exchange(true, std::memory_order_acq_rel)) return;
    Log::info(kLog, "a reading was emptied behind the interface");
}

uint32_t STDMETHODCALLTYPE keyCountDetour(IGameInputReading* self) {
    if (!WindowHook::captureInput()) return g_originalKeyCount(self);
    noteEmptied();
    return 0;
}

uint32_t STDMETHODCALLTYPE keyStateDetour(IGameInputReading* self, uint32_t count, void* states) {
    if (!WindowHook::captureInput()) return g_originalKeyState(self, count, states);
    noteEmptied();
    return 0;
}

bool STDMETHODCALLTYPE mouseStateDetour(IGameInputReading* self, MouseState* state) {
    const bool has = g_originalMouseState(self, state);
    if (!has || !state) return has;

    const bool capturing = WindowHook::captureInput();
    if (capturing) noteEmptied();
    steadyMouse(*state, capturing);
    return has;
}

// Reported once, and only once: a reading that does not hold up its end is a reason to
// leave every reading alone rather than to fill the log.
std::atomic<bool> g_patchRefused{false};

void refuse(std::string_view why) {
    if (g_patchRefused.exchange(true, std::memory_order_acq_rel)) return;
    Log::warn(kLog, "{}; readings will be withheld whole instead", why);
}

// Called with the first reading the game — or the probe below — gets hold of. The
// vtable belongs to the redistributable, not to whoever created the IGameInput, so
// patching it here reaches the readings the game collects too.
bool patchReading(IGameInputReading* reading, GameInputKind asked) {
    if (!reading) return false;

    void** const vtable = *reinterpret_cast<void***>(reading);
    if (!vtable) return false;

    if (void** const known = g_readingVtable.load(std::memory_order_acquire)) {
        return known == vtable;
    }

    const std::lock_guard<std::mutex> guard(g_patchMutex);
    if (void** const known = g_readingVtable.load(std::memory_order_acquire)) {
        return known == vtable;
    }
    if (g_patchRefused.load(std::memory_order_acquire)) return false;

    const auto kindOf = reinterpret_cast<GetInputKindFn>(vtable[kGetInputKindIndex]);
    if (const GameInputKind kind = kindOf(reading); (kind & asked) == 0) {
        refuse(std::format("a reading fetched for kind {:#x} answers to {:#x}", asked, kind));
        return false;
    }

    struct Patch {
        size_t index;
        void* detour;
        void** original;
    };
    const Patch patches[]{
        {kGetKeyCountIndex, reinterpret_cast<void*>(&keyCountDetour),
         reinterpret_cast<void**>(&g_originalKeyCount)},
        {kGetKeyStateIndex, reinterpret_cast<void*>(&keyStateDetour),
         reinterpret_cast<void**>(&g_originalKeyState)},
        {kGetMouseStateIndex, reinterpret_cast<void*>(&mouseStateDetour),
         reinterpret_cast<void**>(&g_originalMouseState)},
    };

    size_t done = 0;
    for (; done < std::size(patches); ++done) {
        void* const target = vtable[patches[done].index];
        if (MH_CreateHook(target, patches[done].detour, patches[done].original) != MH_OK) break;
        if (MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            break;
        }
        g_readingTargets[done] = target;
    }

    if (done != std::size(patches)) {
        for (void*& target : g_readingTargets) {
            if (!target) continue;
            MH_DisableHook(target);
            MH_RemoveHook(target);
            target = nullptr;
        }
        refuse("the readings' own methods could not be hooked");
        return false;
    }

    g_readingVtable.store(vtable, std::memory_order_release);
    g_emptying.store(true, std::memory_order_release);
    Log::info(kLog, "readings will come out empty while an interface is open");
    return true;
}

// The controller is left alone: someone playing on a pad has no cursor to take away,
// and the interface is not driven by one.
bool concerns(GameInputKind kind) { return (kind & (kKeyboard | kMouse)) != 0; }

// Emptying the reading is the whole point, so once that is in place the reading is
// handed over as usual. Withholding is only the fallback.
bool withhold(GameInputKind kind) {
    if (g_emptying.load(std::memory_order_acquire)) return false;
    return WindowHook::captureInput() && concerns(kind);
}

// Whether the game polls for its readings at all is worth knowing once. If this line
// never appears, its input reaches it by a road this hook is not standing on.
void reportPoll(GameInputKind kind) {
    if (!concerns(kind)) return;

    // The game asking is what makes this the door the mouse comes through. Patching
    // the reading's vtable at install time says only that the redistributable is
    // here — under WineGDK it answers and the game never asks, taking its mouse off
    // the message pump instead.
    if ((kind & kMouse) != 0 && !turn::exact()) turn::markCarried("GameInput");

    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_acq_rel)) return;
    Log::info(kLog, "the game polls for readings (kind {:#x})", kind);
}

HRESULT STDMETHODCALLTYPE currentReadingDetour(IGameInput* self, GameInputKind kind,
                                               IGameInputDevice* device,
                                               IGameInputReading** reading) {
    reportPoll(kind);

    if (withhold(kind)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }

    const HRESULT hr = g_originalCurrentReading(self, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading && concerns(kind)) patchReading(*reading, kind);
    return hr;
}

HRESULT STDMETHODCALLTYPE nextReadingDetour(IGameInput* self, IGameInputReading* reference,
                                            GameInputKind kind, IGameInputDevice* device,
                                            IGameInputReading** reading) {
    reportPoll(kind);

    if (withhold(kind)) {
        if (reading) *reading = nullptr;
        return kReadingNotFound;
    }

    const HRESULT hr = g_originalNextReading(self, reference, kind, device, reading);
    if (SUCCEEDED(hr) && reading && *reading && concerns(kind)) patchReading(*reading, kind);
    return hr;
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

    void* methods[kInputMethodCount]{};
    std::memcpy(methods, *reinterpret_cast<void***>(probe), sizeof(methods));

    // A reading of our own, if the devices have one to give this early, so that the
    // emptying is in place before the first interface opens rather than after the
    // game's first poll — and so that a game which only ever takes its readings by
    // callback is covered as well.
    const auto current = reinterpret_cast<GetCurrentReadingFn>(methods[kGetCurrentReadingIndex]);
    IGameInputReading* reading = nullptr;
    if (SUCCEEDED(current(probe, kKeyboard | kMouse, nullptr, &reading)) && reading) {
        patchReading(reading, kKeyboard | kMouse);
        reinterpret_cast<ReleaseFn>((*reinterpret_cast<void***>(reading))[2])(reading);
    }

    // Released through the vtable: the interface is opaque here on purpose.
    reinterpret_cast<ReleaseFn>(methods[2])(probe);

    currentReadingTarget_ = methods[kGetCurrentReadingIndex];
    nextReadingTarget_ = methods[kGetNextReadingIndex];

    const bool ok =
        createAt(currentReadingTarget_, reinterpret_cast<void*>(&currentReadingDetour),
                 reinterpret_cast<void**>(&g_originalCurrentReading)) &&
        createAt(nextReadingTarget_, reinterpret_cast<void*>(&nextReadingDetour),
                 reinterpret_cast<void**>(&g_originalNextReading));

    installed_ = ok;
    if (ok && !g_emptying.load(std::memory_order_acquire)) {
        Log::info(kLog, "readings will pause while an interface is open, until one can be read");
    }
    return ok;
}

void GameInputHook::uninstall() {
    for (void* target : {currentReadingTarget_, nextReadingTarget_}) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }

    for (void*& target : g_readingTargets) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }

    g_emptying.store(false, std::memory_order_release);
    g_readingVtable.store(nullptr, std::memory_order_release);

    // Totals from a hook that is no longer standing are worse than none: reinstalling
    // over them would hand the game the whole gap in one delta.
    g_seeded = false;
    turn::forget();

    currentReadingTarget_ = nextReadingTarget_ = nullptr;
    installed_ = false;
}

}
