#pragma once

#include <atomic>
#include <exception>

#include "core/Log.hpp"

namespace velyx::hooks {

/// The game's own functions the hooks below stand on, under the names a signature
/// pack knows them by. None of them is required: a pack that lacks one costs the
/// modules that listen for its event, and nothing else.
constexpr const char* kApplyTurnDelta = "LocalPlayer::applyTurnDelta";
constexpr const char* kGetFov = "LevelRendererPlayer::getFov";
constexpr const char* kSetupCamera = "LevelRendererPlayer::setupCamera";

/// The call inside setupCamera that takes the tangent of half the field of view, a
/// `call [rip+x]` through the import of the C runtime's tanf. The field of view goes in
/// there and nowhere else before the projection is built from it.
constexpr const char* kFovTangent = "LevelRendererPlayer::fovTangent";

/// The same call a second time, in the function setupCamera hands the camera component
/// to: it builds the projection the frame is actually drawn with, from the same field
/// of view, and takes the tangent through the same import.
constexpr const char* kDrawFovTangent = "LevelRendererPlayer::drawFovTangent";
constexpr const char* kGetViewPerspective = "Options::getViewPerspective";
constexpr const char* kAttack = "GameMode::attack";

/// The chat, in the one of two shapes the build has. Older builds hand the sender and
/// the line to displayChatMessage as two `const&`; newer ones build a GuiMessage and
/// pass it by value to the function every display call ends in, which destroys it.
constexpr const char* kDisplayChatMessage = "GuiData::displayChatMessage";
constexpr const char* kAddMessage = "GuiData::addMessage";
constexpr const char* kMessageDestroy = "GuiMessage::~GuiMessage";
constexpr const char* kMessageAuthor = "GuiMessage::author";
constexpr const char* kMessageText = "GuiMessage::message";

/// The global the game's allocator lives behind. A string handed to a function that
/// frees it has to come from there; see memory::assignGameString.
constexpr const char* kAllocator = "Bedrock::allocator";

/// Where a GameMode keeps the player it belongs to. With it, the first hit the player
/// lands names the player object — a road to it that needs no offset from the client
/// instance and no turn hook.
constexpr const char* kGameModePlayer = "GameMode::player";

/// Where the camera keeps the projection it was just set up with, for a build that
/// computes its field of view into a camera component rather than through a getter:
/// the projection stack inside mce::Camera, and the ring the stack is made of.
constexpr const char* kProjectionStack = "mce::Camera::projectionStack";
constexpr const char* kStackStorage = "MatrixStack::storage";
constexpr const char* kStackCapacity = "MatrixStack::capacity";
constexpr const char* kStackHead = "MatrixStack::head";
constexpr const char* kStackCount = "MatrixStack::count";

/// Declares them to the registry so the pack can fill them in and the Diagnostics page
/// lists them with everything else. Before Signatures::resolveAll().
void declareSignatures();

/// Every detour in this directory is a boundary the game calls across, on a thread of
/// its own. An exception that escapes one reaches no handler and ends the process
/// through std::terminate — so nothing may leave, and the first time something tries
/// is said once rather than once per call.
template <typename Fn>
void guarded(const char* log, const char* what, std::atomic<bool>& said, Fn&& body) {
    try {
        body();
    } catch (const std::exception& error) {
        if (!said.exchange(true, std::memory_order_acq_rel)) {
            Log::error(log, "{} threw: {}; the game's own call goes on untouched", what,
                       error.what());
        }
    } catch (...) {
        if (!said.exchange(true, std::memory_order_acq_rel)) {
            Log::error(log, "{} threw; the game's own call goes on untouched", what);
        }
    }
}

}
