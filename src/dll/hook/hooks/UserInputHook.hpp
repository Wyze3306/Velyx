#pragma once

#include <windows.h>

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Everything else user32 will tell the game about the keyboard and the mouse.
///
/// Refusing the window messages only covers a game that reads its input from its own
/// window procedure, and Bedrock plainly does not: the camera kept turning and the
/// clicks kept landing on its own buttons behind an open menu. Two roads lead past
/// that procedure. Raw input is one — an engine picks WM_INPUT out of the message pump
/// itself, so swallowing it in the procedure changes nothing, and reads it with
/// GetRawInputData. The state functions are the other: they answer from the device,
/// not from anything queued. Both are answered here as though the player had let go of
/// everything, for exactly as long as an interface is open.
///
/// A key *released* behind an interface is the one thing still allowed through: raw
/// presses are turned into releases rather than dropped, so nothing the game believed
/// was held survives the menu closing.
///
/// The pump is watched too, for the same reason: a game that picks its input out of
/// the queue itself never gives the window procedure the chance to refuse it.
///
/// The pointer itself is held here too: a game in play hides it every frame, menu on
/// top or not, and an interface cannot be used blind.
///
/// Each of these also says so in the log the first time the game uses it behind an
/// interface, which is how the road it actually takes gets named.
class UserInputHook final : public Hook {
public:
    UserInputHook();

    bool install() override;
    void uninstall() override;

    /// What user32 would have said about a key, taken past the emptying above.
    ///
    /// The detours answer for the game, and the client still needs the truth: a bind
    /// with Ctrl in it has to go on matching while its own menu is open, and the menu
    /// is exactly when the game is being told that nothing is held.
    [[nodiscard]] static SHORT realKeyState(int key);

    // The same for the key as the system holds it, whichever thread asks: GetKeyState
    // only knows what the asking thread's queue has seen, and the frame runs on a thread
    // that sees no keyboard at all.
    [[nodiscard]] static SHORT realAsyncKeyState(int key);

private:
    void* targets_[14]{};
};

}
