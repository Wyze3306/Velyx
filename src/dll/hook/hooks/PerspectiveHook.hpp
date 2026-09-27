#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Gives `PerspectiveEvent` its producer: the game's own getter for which perspective
/// to draw — first person, third person from behind, or from the front.
///
/// The game asks every time it needs to know, so answering the question is enough to
/// change the view: nothing is written into the game's memory, and letting go of the
/// hook puts the game's own answer back at once.
class PerspectiveHook final : public Hook {
public:
    PerspectiveHook();

    bool install() override;
    void uninstall() override;

    [[nodiscard]] static bool live();
};

}
