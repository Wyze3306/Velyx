#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Keeps the mouse and the keyboard out of the game while an interface is up.
///
/// Blocking the window messages is not enough: Bedrock reads the mouse and the
/// keyboard through Microsoft GameInput, which never touches the message queue, so
/// the camera kept turning and clicks kept landing behind an open menu.
///
/// What GameInput hands out is a *reading*, and a reading is a state rather than an
/// event: refuse to give one and the game simply keeps the last one it had, which is
/// how a menu opened on Ctrl+K left the player sprinting behind it. So the readings
/// are emptied rather than withheld — no keys down, no buttons down, a mouse that has
/// not moved — which says in the game's own terms that the player has let go of
/// everything. Emptying the reading also covers the game whichever way it collects
/// them, polled or by callback.
///
/// Only if those methods cannot be reached does the hook fall back to withholding the
/// reading altogether, which is better than nothing and worse than this.
///
/// Standing on the mouse reading also makes this one of the two doors a turn can come
/// through: every reading's movement goes to `velyx::turn` before the game is told
/// about it, so a module can scale it, smooth it or cancel it. The other door is raw
/// input, in `UserInputHook`, and which one the game actually walks through is not the
/// same on every machine — Windows takes GameInput, WineGDK takes the message pump.
///
/// Only a reading the *game* asked for counts as this hook carrying the mouse. The
/// probe in `install` patches the reading's vtable without proving anything about who
/// goes on to use it, and reading that as proof is how a turn ends up shaped on a door
/// nobody opens.
class GameInputHook final : public Hook {
public:
    GameInputHook();

    bool install() override;
    void uninstall() override;

private:
    void* currentReadingTarget_ = nullptr;
    void* nextReadingTarget_ = nullptr;
};

}
