#pragma once

#include <string>

namespace velyx::tuning {

// Three settings Windows keeps that cost a game frames, none of which the game can
// reach from inside itself. Everything here is written under HKEY_CURRENT_USER or
// through the power API, so none of it needs an administrator and all of it can be
// put back exactly as it was found.
enum class State {
    Off,
    On,

    // The setting is not where Windows keeps it on this machine — an edition without
    // the feature, or a version that moved it. Reported rather than forced.
    Unknown,
};

// Whether Windows is recording the last few minutes of every game in the background.
// It is on by default, it costs frames on every machine, and almost nobody uses it.
[[nodiscard]] State backgroundRecording();
bool setBackgroundRecording(bool on, std::string* error = nullptr);

// Which graphics card Windows hands this instance when the machine has two. The
// default is "let Windows decide", which on a laptop is regularly the wrong one.
[[nodiscard]] State highPerformanceGpu(const std::string& activationId);
bool setHighPerformanceGpu(const std::string& activationId, bool on, std::string* error = nullptr);

// The power plan, held only while a game is up. A machine left on high performance
// after the game closes is a machine with a flat battery, so the previous plan is
// remembered on the way in and put back on the way out — including if the launcher
// is closed while it still holds one.
[[nodiscard]] bool powerPlanAvailable();
[[nodiscard]] bool holdingPowerPlan();
void holdPowerPlan(bool hold);

}
