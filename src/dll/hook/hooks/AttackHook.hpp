#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Hears a hit of the player's the moment the game registers it: `GameMode::attack`
/// runs with the target when a swing lands on an entity, before anything is sent to
/// the server.
///
/// It runs on the game's thread, so nothing is emitted here. The target's address is
/// handed to `sdk::Signals`, and the next frame turns it into `AttackEvent` and
/// `ActorHurtEvent` with an entity a handler can draw over. Nothing here changes what
/// the server sees: the game's own call goes through exactly as it came.
class AttackHook final : public Hook {
public:
    AttackHook();

    bool install() override;
    void uninstall() override;

    [[nodiscard]] static bool live();
};

}
