#pragma once

#include <windows.h>

#include <string>

#include <json/json.hpp>

#include "dll/module/Setting.hpp"

namespace velyx {

class ClientConfig {
public:
    static ClientConfig& get();

    void load();
    void save() const;

    std::string activeProfile = "Global";
    std::string language = "en";
    std::string updateChannel = "stable";

    // The look of the client is the client's, not a profile's: the theme and the
    // interfaces' own settings stay the same whichever profile is active.
    std::string theme = "Velyx";
    nlohmann::json interfaceState = nlohmann::json::object();

    bool onboardingCompleted = false;
    bool telemetry = false;

    // Whether opening an interface puts the game to sleep behind it, the way alt-tabbing
    // does. Off leaves the game running with its input refused, which is not the same
    // thing: see the note in WindowHook::setCaptureInput.
    bool suspendGame = true;

    Keybind guiKey{VK_INSERT, false, false, false, Keybind::Mode::Toggle};

    // The menu's second way in: it opens on the module list with the search field
    // already focused.
    Keybind searchKey{'K', true, false, false, Keybind::Mode::Toggle};
    Keybind hudEditorKey{VK_F6, false, false, false, Keybind::Mode::Toggle};
    Keybind instantReplayKey{VK_F9, false, false, false, Keybind::Mode::Once};
    Keybind screenshotKey{VK_F2, false, false, false, Keybind::Mode::Once};
    Keybind clipMarkerKey{VK_F8, false, false, false, Keybind::Mode::Once};

    bool cleanShutdown = true;

    int crashStreak = 0;

    std::string lastCrashModule;
    std::string lastCrashReason;

    void markSessionStarted();
    void markSessionEnded();

    [[nodiscard]] bool shouldStartInSafeMode() const;

private:
    ClientConfig() = default;
};

inline ClientConfig& config() { return ClientConfig::get(); }

}
