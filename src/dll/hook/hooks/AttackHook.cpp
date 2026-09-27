#include "AttackHook.hpp"

#include <atomic>
#include <cstdint>

#include "core/Log.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/hook/hooks/GameHooks.hpp"
#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/sdk/Signals.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "AttackHook";

// GameMode::attack(Actor& target, ...). Past the target every register is carried
// whole: on 1.26 the third one is a pointer, and a narrower type here hands the game
// its low byte back — the fault at 0x34 and 0x74 that a first hit used to end in.
using AttackFn = int64_t(__fastcall*)(uintptr_t gameMode, uintptr_t actor, uintptr_t c,
                                      uintptr_t d, uintptr_t e, uintptr_t f);

AttackFn g_original = nullptr;
std::atomic<bool> g_live{false};
std::atomic<bool> g_threw{false};
std::atomic<bool> g_reported{false};

int64_t __fastcall detour(uintptr_t gameMode, uintptr_t actor, uintptr_t c, uintptr_t d,
                          uintptr_t e, uintptr_t f) {
    if (!g_original) return 0;

    if (actor != 0 && gameMode != 0) {
        const crash::Breadcrumb breadcrumb("attack hook");
        hooks::guarded(kLog, "attack note", g_threw, [&] {
            // Every player has a GameMode, and in a world of your own the integrated
            // server runs this again for its copy of you. Only the local player's hit
            // is the player's hit.
            const int offset = sig::offset(hooks::kGameModePlayer);
            const uintptr_t player =
                offset >= 0 ? memory::read<uintptr_t>(gameMode + static_cast<uintptr_t>(offset))
                            : 0;
            if (offset >= 0 && !sdk::game().isLocalPlayer(player)) return;

            if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
                Log::info(kLog, "the game registered a hit of the player's");
            }

            if (player != 0) sdk::game().adoptLocalPlayer(player, "the attack hook");
            sdk::signals().noteAttack(actor);
        });
    }

    return g_original(gameMode, actor, c, d, e, f);
}

}

AttackHook::AttackHook() : Hook(hooks::kAttack, sig::address(hooks::kAttack)) {}

bool AttackHook::live() { return g_live.load(std::memory_order_acquire); }

bool AttackHook::install() {
    if (target() == 0) {
        Log::info(kLog, "no {} in the pack: hits are read off the health instead", hooks::kAttack);
        return false;
    }

    if (!create(reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_original))) {
        return false;
    }

    g_live.store(true, std::memory_order_release);
    sdk::signals().setAttackDoor(true);
    Log::info(kLog, "standing on {}", hooks::kAttack);
    return true;
}

void AttackHook::uninstall() {
    Hook::uninstall();
    g_live.store(false, std::memory_order_release);
    sdk::signals().setAttackDoor(false);
}

}
