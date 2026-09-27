#include "PerspectiveHook.hpp"

#include <atomic>
#include <cstdint>

#include "core/Log.hpp"
#include "dll/event/Events.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/hook/hooks/GameHooks.hpp"
#include "dll/memory/Signatures.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "PerspectiveHook";

// int getViewPerspective(). The answer is one of three small numbers; anything else
// is not a perspective and goes back to the caller exactly as it came.
using GetPerspectiveFn = int64_t(__fastcall*)(uintptr_t self, void* b, void* c, void* d);

GetPerspectiveFn g_original = nullptr;
std::atomic<bool> g_live{false};
std::atomic<bool> g_threw{false};
std::atomic<bool> g_reported{false};

int64_t __fastcall detour(uintptr_t self, void* b, void* c, void* d) {
    if (!g_original) return 0;

    const int64_t raw = g_original(self, b, c, d);
    const int value = static_cast<int>(raw);
    if (value < 0 || value > 2) return raw;

    const crash::Breadcrumb breadcrumb("perspective hook");

    int64_t answer = raw;
    hooks::guarded(kLog, "perspective event", g_threw, [&] {
        if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
            Log::info(kLog, "the game asks which perspective to draw ({})", value);
        }

        PerspectiveEvent event;
        event.perspective = static_cast<Perspective>(value);
        events().emit(event);

        const int wanted = static_cast<int>(event.perspective);
        if (wanted >= 0 && wanted <= 2) answer = wanted;
    });

    return answer;
}

}

PerspectiveHook::PerspectiveHook()
    : Hook(hooks::kGetViewPerspective, sig::address(hooks::kGetViewPerspective)) {}

bool PerspectiveHook::live() { return g_live.load(std::memory_order_acquire); }

bool PerspectiveHook::install() {
    if (target() == 0) {
        Log::info(kLog, "no {} in the pack: the perspective stays the game's",
                  hooks::kGetViewPerspective);
        return false;
    }

    if (!create(reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_original))) {
        return false;
    }

    g_live.store(true, std::memory_order_release);
    Log::info(kLog, "standing on {}", hooks::kGetViewPerspective);
    return true;
}

void PerspectiveHook::uninstall() {
    Hook::uninstall();
    g_live.store(false, std::memory_order_release);
}

}
