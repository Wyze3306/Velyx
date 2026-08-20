#include "Hook.hpp"

#include <MinHook.h>

#include "core/Log.hpp"

namespace velyx {
namespace {
constexpr const char* kLog = "Hook";
}

Hook::Hook(std::string name, uintptr_t target) : name_(std::move(name)), target_(target) {}

Hook::~Hook() { uninstall(); }

bool Hook::create(void* detour, void** original) {
    if (target_ == 0) {
        Log::warn(kLog, "{}: null address, skipping", name_);
        return false;
    }
    return createAt(reinterpret_cast<void*>(target_), detour, original);
}

bool Hook::createAt(void* address, void* detour, void** original) {
    if (!address) {
        Log::warn(kLog, "{}: null address, skipping", name_);
        return false;
    }

    const MH_STATUS status = MH_CreateHook(address, detour, original);
    if (status != MH_OK) {
        Log::error(kLog, "{}: MH_CreateHook failed ({})", name_, MH_StatusToString(status));
        return false;
    }

    const MH_STATUS queued = MH_QueueEnableHook(address);
    if (queued != MH_OK) {
        Log::error(kLog, "{}: MH_QueueEnableHook failed ({})", name_, MH_StatusToString(queued));
        MH_RemoveHook(address);
        return false;
    }

    hooked_ = address;
    installed_ = true;
    Log::debug(kLog, "{} -> {:#x}", name_, reinterpret_cast<uintptr_t>(address));
    return true;
}

void Hook::uninstall() {
    if (!installed_ || !hooked_) return;

    MH_DisableHook(hooked_);
    MH_RemoveHook(hooked_);

    installed_ = false;
    hooked_ = nullptr;
}

}
