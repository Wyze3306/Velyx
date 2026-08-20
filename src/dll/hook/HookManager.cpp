#include "HookManager.hpp"

#include <MinHook.h>

#include "core/Log.hpp"

namespace velyx {
namespace {
constexpr const char* kLog = "Hooks";
}

HookManager& HookManager::get() {
    static HookManager instance;
    return instance;
}

bool HookManager::installAll() {
    if (!initialised_) {
        const MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            Log::fatal(kLog, "MH_Initialize a échoué : {}", MH_StatusToString(status));
            return false;
        }
        initialised_ = true;
    }

    int succeeded = 0;
    for (auto& hook : hooks_) {
        if (hook->install()) ++succeeded;
    }

    const MH_STATUS applied = MH_ApplyQueued();
    if (applied != MH_OK) {
        Log::fatal(kLog, "MH_ApplyQueued a échoué : {}", MH_StatusToString(applied));
        return false;
    }

    Log::info(kLog, "{}/{} hooks actifs", succeeded, hooks_.size());

    if (succeeded != static_cast<int>(hooks_.size())) {
        Log::warn(kLog, "inactifs : {}", [this] {
            std::string list;
            for (const auto& name : failed()) {
                if (!list.empty()) list += ", ";
                list += name;
            }
            return list;
        }());
    }

    return true;
}

void HookManager::uninstallAll() {
    for (auto& hook : hooks_) hook->uninstall();

    if (initialised_) {
        MH_Uninitialize();
        initialised_ = false;
    }

    hooks_.clear();
    Log::info(kLog, "hooks retirés");
}

std::vector<std::string> HookManager::failed() const {
    std::vector<std::string> names;
    for (const auto& hook : hooks_) {
        if (!hook->installed()) names.push_back(hook->name());
    }
    return names;
}

}
