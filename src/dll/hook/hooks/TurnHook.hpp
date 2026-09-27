#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// The game's own door for the mouse: `LocalPlayer::applyTurnDelta`, where a movement
/// has already become degrees and is about to become the player's rotation.
///
/// Both input doors — raw input under WineGDK, GameInput on Windows — end up here, so
/// this is the one place a turn can be shaped the same way on every machine, in the
/// game's own units. While it stands, `velyx::turn` shapes here and lets the input
/// doors pass; an injected turn lands exactly, and SnapLook stops needing a
/// calibration.
///
/// It carries a second answer for free. The function is called on the LocalPlayer, so
/// the first turn the game applies names the player object — the offset every reading
/// of the player used to wait for. The SDK adopts it and keeps it for as long as it
/// still starts with the vtable it had.
///
/// Needs the pack to carry the signature; without it the input doors do the shaping,
/// as before, and nothing is lost but exactness.
class TurnHook final : public Hook {
public:
    TurnHook();

    bool install() override;
    void uninstall() override;

    [[nodiscard]] static bool live();
};

}
