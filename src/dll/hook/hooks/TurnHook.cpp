#include "TurnHook.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

#include "core/Log.hpp"
#include "core/Math.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/hook/Turn.hpp"
#include "dll/hook/hooks/GameHooks.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "TurnHook";

// LocalPlayer::applyTurnDelta(Vec2 const& delta). The Vec2 is laid out the way the
// game lays out a rotation — x the pitch, y the yaw — and holds degrees. Two spare
// register parameters ride along so that whatever the game passes beyond the pair
// reaches the real function untouched, and the return travels the same way.
using ApplyTurnDeltaFn = int64_t(__fastcall*)(uintptr_t player, Vec2* delta, void* c, void* d);

ApplyTurnDeltaFn g_original = nullptr;
std::atomic<bool> g_live{false};
std::atomic<bool> g_threw{false};

// How the game calls it is worth one line in the log: every tick with a zero movement
// in it, or only when the mouse moved. The first means an injected turn lands on its
// own, the second that it waits for the player's hand.
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_empty{0};

int64_t __fastcall detour(uintptr_t player, Vec2* delta, void* c, void* d) {
    if (!g_original) return 0;
    if (!delta) return g_original(player, delta, c, d);

    const crash::Breadcrumb breadcrumb("turn hook");

    hooks::guarded(kLog, "turn shaping", g_threw, [&] {
        sdk::game().adoptLocalPlayer(player, "the turn hook");
        turn::markCarried(hooks::kApplyTurnDelta);

        const uint32_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (delta->x == 0.f && delta->y == 0.f) g_empty.fetch_add(1, std::memory_order_relaxed);

        if (calls == 1) {
            Log::debug(kLog, "first turn: pitch {:.3f}, yaw {:.3f}", delta->x, delta->y);
        } else if (calls == 600) {
            Log::info(kLog, "applyTurnDelta: {} calls, {} with no movement in them", calls,
                      g_empty.load(std::memory_order_relaxed));
        }

        float yaw = delta->y;
        float pitch = delta->x;
        turn::shapeDegrees(yaw, pitch);
        delta->y = yaw;
        delta->x = pitch;
    });

    return g_original(player, delta, c, d);
}

}

TurnHook::TurnHook() : Hook(hooks::kApplyTurnDelta, sig::address(hooks::kApplyTurnDelta)) {}

bool TurnHook::live() { return g_live.load(std::memory_order_acquire); }

bool TurnHook::install() {
    if (target() == 0) {
        Log::info(kLog, "no {} in the pack: turns are shaped at the input door",
                  hooks::kApplyTurnDelta);
        return false;
    }

    if (!create(reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_original))) {
        return false;
    }

    g_live.store(true, std::memory_order_release);
    turn::markExact(true);
    sdk::game().expectPlayerFromHook(true);
    Log::info(kLog, "standing on {}: exact turns, and the player announces itself",
              hooks::kApplyTurnDelta);
    return true;
}

void TurnHook::uninstall() {
    Hook::uninstall();

    g_live.store(false, std::memory_order_release);
    turn::markExact(false);
    turn::forget();
    sdk::game().expectPlayerFromHook(false);
}

}
