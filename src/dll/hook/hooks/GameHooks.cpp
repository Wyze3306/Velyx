#include "GameHooks.hpp"

#include "dll/memory/Signatures.hpp"

namespace velyx::hooks {

void declareSignatures() {
    Signatures& registry = Signatures::get();

    const struct {
        const char* name;
        const char* owner;
    } wanted[]{
        {kApplyTurnDelta, "Turn"},
        {kGetFov, "FOV"},
        {kSetupCamera, "FOV"},
        {kFovTangent, "FOV"},
        {kDrawFovTangent, "FOV"},
        {kGetViewPerspective, "Perspective"},
        {kAttack, "Attack"},
        {kAddMessage, "Chat"},
        {kMessageDestroy, "Chat"},
        {kAllocator, "Chat"},
    };

    for (const auto& entry : wanted) {
        SignatureSpec spec;
        spec.name = entry.name;
        spec.owner = entry.owner;
        registry.require(spec);
    }

    for (const char* offset :
         {kProjectionStack, kStackStorage, kStackCapacity, kStackHead, kStackCount}) {
        registry.requireOffset(offset, "FOV");
    }
    registry.requireOffset(kGameModePlayer, "Attack");
    registry.requireOffset(kMessageAuthor, "Chat");
    registry.requireOffset(kMessageText, "Chat");
}

}
