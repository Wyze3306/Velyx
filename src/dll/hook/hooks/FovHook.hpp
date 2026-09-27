#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Gives `FovEvent` the producer it never had, through whichever door the build has.
///
/// Older builds ask `LevelRendererPlayer::getFov` for a number and draw with it: the
/// hook stands on the getter, hands the number to the handlers and returns theirs.
///
/// Newer builds compute the field of view into a camera component and never ask
/// anyone; what is left to stand on is `LevelRendererPlayer::setupCamera`, which
/// turns that component into the projection the frame is drawn through. So the hook
/// lets the game set the camera up, reads the field of view back out of the
/// projection it just built — a perspective matrix carries it in one entry — hands it
/// to the handlers, and rescales the two entries that depend on it. The frustum, the
/// near and far planes and everything else in the matrix stay the game's.
///
/// Rescaling the matrix afterwards turned out to be too late on 1.26.51: setupCamera
/// hands the projection on before it returns, and the copy it hands on is what the
/// frame is drawn with. What it builds the projection from is the tangent of half the
/// field of view, taken through the C runtime's `tanf`. So where the pack names that
/// call, the hook stands on `tanf` itself and answers differently for that one caller
/// only — recognised by the address it returns to — and the game builds its projection,
/// and every copy of it, with the field of view the handlers asked for.
///
/// Either way the event carries degrees, a handler sets 90 and gets 90, and what comes
/// back is checked before the game sees it. The value the game computed is also handed
/// to `sdk::Camera`, which then projects through the real field of view on a pack that
/// carries no offset for it.
class FovHook final : public Hook {
public:
    FovHook();

    bool install() override;
    void uninstall() override;

    [[nodiscard]] static bool live();

private:
    void* target_ = nullptr;
};

}
