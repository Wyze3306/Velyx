#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Keeps the mouse and the keyboard out of the game while an interface is up.
///
/// Blocking the window messages is not enough: Bedrock reads the mouse through
/// Microsoft GameInput, which never touches the message queue, so the camera kept
/// turning and clicks kept landing behind an open menu. GameInput hands out
/// "readings", and a game asking for one while the client is capturing is told there
/// is none — the same answer it gets when nothing has happened, and the one path it
/// is already written to handle.
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
