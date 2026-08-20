#pragma once

#include <windows.h>

#include <string>

#include "dll/module/Setting.hpp"

namespace velyx {

class ClientConfig {
public:
    static ClientConfig& get();

    void load();
    void save() const;

    std::string activeProfile = "Global";
    std::string language = "fr";
    std::string updateChannel = "stable";

    bool onboardingCompleted = false;
    bool telemetry = false;

    Keybind guiKey{VK_INSERT, false, false, false, Keybind::Mode::Toggle};
    Keybind paletteKey{'K', true, false, false, Keybind::Mode::Toggle};
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
