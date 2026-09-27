#include "ClientConfig.hpp"

#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Config";

std::filesystem::path clientFile() { return Paths::config() / "client.json"; }

nlohmann::json bindToJson(const Keybind& bind) { return toJson(SettingValue{bind}); }

Keybind bindFromJson(const nlohmann::json& json, const Keybind& fallback) {
    return std::get<Keybind>(fromJson(json, SettingValue{fallback}));
}

}

ClientConfig& ClientConfig::get() {
    static ClientConfig instance;
    return instance;
}

void ClientConfig::load() {
    std::ifstream stream(clientFile());
    if (!stream) {
        Log::info(kLog, "first run, using default configuration");
        return;
    }

    nlohmann::json json;
    try {
        stream >> json;
    } catch (const std::exception& e) {
        Log::warn(kLog, "client.json unreadable ({}), using defaults", e.what());
        return;
    }

    // "language" existed long before it did anything, and every configuration written
    // by an older build carries the "fr" it was born with. Only a file that knows
    // about the setting is allowed to speak for it.
    const int formatVersion = json.value("formatVersion", 0);

    activeProfile = json.value("activeProfile", activeProfile);
    if (formatVersion >= 1) language = json.value("language", language);
    updateChannel = json.value("updateChannel", updateChannel);
    theme = json.value("theme", theme);

    if (json.contains("interface") && json["interface"].is_object()) {
        interfaceState = json["interface"];
    }
    onboardingCompleted = json.value("onboardingCompleted", onboardingCompleted);
    telemetry = json.value("telemetry", telemetry);
    cleanShutdown = json.value("cleanShutdown", true);
    faulted = json.value("faulted", false);
    crashStreak = json.value("crashStreak", 0);
    lastCrashModule = json.value("lastCrashModule", std::string{});
    lastCrashReason = json.value("lastCrashReason", std::string{});

    if (json.contains("keys")) {
        const auto& keys = json["keys"];
        guiKey = bindFromJson(keys.value("gui", nlohmann::json{}), guiKey);
        // "palette" is what the command palette's key was called; the menu's search
        // inherited both the key and the setting.
        searchKey = bindFromJson(keys.value("palette", nlohmann::json{}), searchKey);
        searchKey = bindFromJson(keys.value("search", nlohmann::json{}), searchKey);
        hudEditorKey = bindFromJson(keys.value("hudEditor", nlohmann::json{}), hudEditorKey);
        instantReplayKey = bindFromJson(keys.value("instantReplay", nlohmann::json{}),
                                        instantReplayKey);
        screenshotKey = bindFromJson(keys.value("screenshot", nlohmann::json{}), screenshotKey);
        clipMarkerKey = bindFromJson(keys.value("clipMarker", nlohmann::json{}), clipMarkerKey);
        gameChatKey = bindFromJson(keys.value("gameChat", nlohmann::json{}), gameChatKey);
    }
}

void ClientConfig::save() const {
    nlohmann::json json;
    json["formatVersion"] = 1;
    json["activeProfile"] = activeProfile;
    json["language"] = language;
    json["updateChannel"] = updateChannel;
    json["theme"] = theme;
    json["interface"] = interfaceState;
    json["onboardingCompleted"] = onboardingCompleted;
    json["telemetry"] = telemetry;
    json["cleanShutdown"] = cleanShutdown;
    json["faulted"] = faulted;
    json["crashStreak"] = crashStreak;
    json["lastCrashModule"] = lastCrashModule;
    json["lastCrashReason"] = lastCrashReason;

    auto& keys = json["keys"];
    keys["gui"] = bindToJson(guiKey);
    keys["search"] = bindToJson(searchKey);
    keys["hudEditor"] = bindToJson(hudEditorKey);
    keys["instantReplay"] = bindToJson(instantReplayKey);
    keys["screenshot"] = bindToJson(screenshotKey);
    keys["clipMarker"] = bindToJson(clipMarkerKey);
    keys["gameChat"] = bindToJson(gameChatKey);

    std::error_code ec;
    std::filesystem::create_directories(Paths::config(), ec);

    std::ofstream stream(clientFile());
    if (!stream) {
        Log::error(kLog, "could not write client.json");
        return;
    }
    stream << json.dump(2);
}

void ClientConfig::markSessionStarted() {
    // An unclean exit is not a crash. A process that is killed leaves the flag exactly
    // as a fault would, and killing the game is how an ordinary session ends under a
    // launcher that does it that way, or under alt-F4 on any machine. Counting those
    // put the client in safe mode after two normal sessions and never let it out,
    // which from the menu looks like sixty broken modules rather than one guard.
    //
    // What a crash actually looks like is the handler having recorded a fault. Missing
    // one too violent to record costs a safe mode nobody asked for; counting every kill
    // as a fault costs the client itself.
    if (faulted) {
        ++crashStreak;
        Log::warn(kLog, "the previous session recorded a fault (streak: {})", crashStreak);
    } else {
        if (!cleanShutdown) {
            Log::info(kLog, "the previous session ended without a goodbye, but recorded "
                            "no fault; not counted");
        }
        crashStreak = 0;
    }

    faulted = false;
    cleanShutdown = false;
    save();
}

// Deliberately not touching the streak: whether the last session faulted is decided in
// one place, at the next start, so a fault survived mid-session still counts.
void ClientConfig::markSessionEnded() {
    cleanShutdown = true;
    save();
}

bool ClientConfig::shouldStartInSafeMode() const {

    return crashStreak >= 2;
}

}
