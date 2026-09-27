#pragma once

#include "dll/hook/Hook.hpp"

namespace velyx {

/// Gives `ChatReceiveEvent` the producer it never had.
///
/// It stands on the last call a line goes through before the chat window draws it,
/// in whichever of two shapes the build has:
///
///   * `GuiData::addMessage`, which every display call on newer builds ends in. It
///     takes the whole GuiMessage by value and destroys it, so a dropped line is
///     destroyed here instead, and a rewritten one gets strings from the game's own
///     heap;
///   * `GuiData::displayChatMessage` on older builds, which takes the sender and the
///     line as two `const&` and so can be handed strings Velyx owns.
///
/// Either way a handler can badge a name, censor a word, or drop a line by
/// cancelling, and what it does not touch reaches the game byte for byte.
///
/// Without either signature the hook does not install, and every chat listener simply
/// hears nothing, which is exactly where the client already was.
class ChatHook final : public Hook {
public:
    ChatHook();

    bool install() override;
};

}
