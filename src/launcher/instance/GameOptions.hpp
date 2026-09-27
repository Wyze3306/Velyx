#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace velyx::gameoptions {

// The graphics settings the game itself keeps, written from outside it. This is the
// one lever on the list that is worth more than everything the injected client can do
// put together: a render distance is chunks the machine does not have to build, and no
// amount of scheduling makes up for building them.
enum class Preset {
    Performance,
    Balanced,
};

const char* presetName(Preset preset);

// %LOCALAPPDATA%\Packages\<family>\LocalState\games\com.mojang\minecraftpe\options.txt
[[nodiscard]] std::filesystem::path optionsPath(const std::string& packageFamilyName);

struct Change {
    std::string label;
    std::string key;
    std::string from;
    std::string to;
};

struct Result {
    bool ok = false;

    // The game writes this file when it exits, with every key this build knows about.
    // Until it has run once there is nothing to read, and nothing here will guess at
    // what a version it has never seen calls its settings.
    bool noOptionsYet = false;

    std::vector<Change> changes;

    // Settings this preset has an opinion about that this build of the game does not
    // offer. Named rather than silently dropped, so a version that renames something
    // shows up as a short list instead of as a preset that quietly does less.
    std::vector<std::string> notOffered;

    std::string error;

    [[nodiscard]] std::string summary() const;
};

// Reads the file, changes only the keys already in it, and keeps a copy of the
// original the first time it touches one.
Result apply(const std::string& packageFamilyName, Preset preset);

// What apply() would do, without doing it.
Result preview(const std::string& packageFamilyName, Preset preset);

[[nodiscard]] bool hasBackup(const std::string& packageFamilyName);
bool restore(const std::string& packageFamilyName, std::string* error = nullptr);

}
