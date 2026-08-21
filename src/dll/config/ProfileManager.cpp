#include "ProfileManager.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/module/ModuleManager.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Profiles";
constexpr const char* kProfileFile = "profile.json";
constexpr int kFormatVersion = 1;
constexpr size_t kMaxVersions = 20;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string timestampId() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &time);

    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return {};

    try {
        nlohmann::json json;
        stream >> json;
        return json;
    } catch (const std::exception& e) {
        Log::warn(kLog, "{} unreadable: {}", path.filename().string(), e.what());
        return {};
    }
}

bool writeJson(const std::filesystem::path& path, const nlohmann::json& json) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream stream(path);
    if (!stream) {
        Log::error(kLog, "could not write {}", path.string());
        return false;
    }

    stream << json.dump(2);
    return true;
}

Profile profileFromJson(const nlohmann::json& json, const std::string& fallbackName) {
    Profile profile;
    profile.name = json.value("name", fallbackName);
    profile.description = json.value("description", std::string{});
    profile.autoSwitch = json.value("autoSwitch", true);
    profile.isDefault = json.value("isDefault", false);
    profile.modifiedAt = json.value("modifiedAt", 0LL);

    if (json.contains("serverMatches") && json["serverMatches"].is_array()) {
        for (const auto& entry : json["serverMatches"]) {
            if (entry.is_string()) profile.serverMatches.push_back(entry.get<std::string>());
        }
    }

    return profile;
}

nlohmann::json profileToJson(const Profile& profile) {
    nlohmann::json json;
    json["name"] = profile.name;
    json["description"] = profile.description;
    json["autoSwitch"] = profile.autoSwitch;
    json["isDefault"] = profile.isDefault;
    json["modifiedAt"] = profile.modifiedAt;
    json["serverMatches"] = profile.serverMatches;
    return json;
}

}

ProfileManager& ProfileManager::get() {
    static ProfileManager instance;
    return instance;
}

std::filesystem::path ProfileManager::fileFor(const std::string& name) const {
    return Paths::profile(name) / kProfileFile;
}

Profile* ProfileManager::findMutable(const std::string& name) {
    const auto it = std::ranges::find_if(profiles_, [&](const Profile& p) { return p.name == name; });
    return it == profiles_.end() ? nullptr : &*it;
}

bool ProfileManager::exists(const std::string& name) const {
    return std::ranges::any_of(profiles_, [&](const Profile& p) { return p.name == name; });
}

std::string ProfileManager::uniqueName(const std::string& wanted) const {
    if (!exists(wanted)) return wanted;

    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = std::format("{} ({})", wanted, suffix);
        if (!exists(candidate)) return candidate;
    }
    return wanted + " " + strings::hashId(wanted).substr(0, 4);
}

void ProfileManager::createStarterProfiles() {

    // Contexts, not servers: a profile that names one is out of date the day that
    // server changes, and Velyx has no business shipping a list of them.
    const std::vector<Profile> starters{
        Profile{"Global", "The default profile, used when no other one is picked.",
                {}, false, true, nowMs()},
        Profile{"PvP", "Minimal latency, effects cut back, a readable HUD.",
                {}, false, false, nowMs()},
        Profile{"Performance", "The bare minimum on screen, everything else off.",
                {}, false, false, nowMs()},
        Profile{"Survival", "The full HUD: coordinates, compass, waypoints.",
                {}, false, false, nowMs()},
    };

    for (const Profile& profile : starters) {
        nlohmann::json document;
        document["formatVersion"] = kFormatVersion;
        document["profile"] = profileToJson(profile);
        document["modules"] = nlohmann::json::object();

        writeJson(fileFor(profile.name), document);
        profiles_.push_back(profile);
    }

    Log::info(kLog, "created the starter profiles ({})", starters.size());
}

// Earlier builds shipped two profiles named after servers and a set of matching rules
// on a third. They go, once — but only while they still hold exactly what was shipped,
// so a profile the player has since made their own is left alone.
void ProfileManager::dropShippedServerProfiles() {
    const std::pair<std::string, std::vector<std::string>> shipped[]{
        {"Hive", {"hive", "hivebedrock"}},
        {"CubeCraft", {"cubecraft", "cubecraft.net"}},
    };

    for (const auto& [name, rules] : shipped) {
        const Profile* profile = findMutable(name);
        if (!profile || profile->serverMatches != rules) continue;

        std::error_code ec;
        std::filesystem::remove_all(Paths::profile(name), ec);
        std::erase_if(profiles_, [&](const Profile& p) { return p.name == name; });

        Log::info(kLog, "dropped the '{}' starter profile", name);
    }

    // The third one is still a profile worth having; only the rules it was shipped
    // with have to go.
    Profile* pvp = findMutable("PvP");
    if (!pvp || pvp->serverMatches != std::vector<std::string>{"zeqa", "pvp", "duels"}) return;

    pvp->serverMatches.clear();

    nlohmann::json document = readJson(fileFor(pvp->name));
    document["profile"] = profileToJson(*pvp);
    writeJson(fileFor(pvp->name), document);

    Log::info(kLog, "cleared the server rules shipped with the 'PvP' profile");
}

void ProfileManager::load() {
    if (loaded_) return;
    loaded_ = true;

    profiles_.clear();

    std::error_code ec;
    std::filesystem::create_directories(Paths::profiles(), ec);

    for (const auto& entry : std::filesystem::directory_iterator(Paths::profiles(), ec)) {
        if (!entry.is_directory(ec)) continue;

        const auto file = entry.path() / kProfileFile;
        if (!std::filesystem::exists(file, ec)) continue;

        const nlohmann::json document = readJson(file);
        if (document.is_null()) continue;

        const nlohmann::json& node = document.contains("profile") ? document["profile"] : document;
        profiles_.push_back(profileFromJson(node, entry.path().filename().string()));
    }

    if (profiles_.empty()) createStarterProfiles();

    dropShippedServerProfiles();

    if (profiles_.empty()) createStarterProfiles();

    if (!std::ranges::any_of(profiles_, [](const Profile& p) { return p.isDefault; })) {
        profiles_.front().isDefault = true;
    }

    events().on<WorldJoinEvent>(this, &ProfileManager::onWorldJoin, EventPriority::High);

    Log::info(kLog, "{} profile(s) available", profiles_.size());
}

nlohmann::json ProfileManager::serializeCurrent() const {
    nlohmann::json document;
    document["formatVersion"] = kFormatVersion;
    document["profile"] = profileToJson(current_);
    document["modules"] = modules().save();
    return document;
}

// The interfaces travel with the client, not with the profile. They are written here
// because this is what every path that saves state already calls.
void ProfileManager::saveInterfaceState() {
    config().interfaceState = modules().save(ModuleManager::Interfaces::Only);
    config().save();
}

void ProfileManager::saveCurrent() {
    if (current_.name.empty()) return;

    // A switch disables every module before loading the new set, and each toggle runs
    // onDisable — where the interface saves the current profile. current_ is already
    // the profile being switched to by then, so that save wrote the half-applied
    // state over it and emptied it. Nothing is persisted mid-switch.
    if (switching_) return;

    current_.modifiedAt = nowMs();

    if (Profile* stored = findMutable(current_.name)) *stored = current_;

    if (writeJson(fileFor(current_.name), serializeCurrent())) {
        Log::debug(kLog, "saved profile '{}'", current_.name);
    }
}

void ProfileManager::applyDocument(const nlohmann::json& document) {
    if (document.contains("modules")) modules().load(document["modules"]);
}

bool ProfileManager::switchTo(const std::string& name, bool automatic) {
    const Profile* found = nullptr;
    for (const Profile& profile : profiles_) {
        if (profile.name == name) found = &profile;
    }

    // The name saved in the configuration outlives the profile it names: it survives a
    // folder deleted by hand and the server profiles earlier builds shipped. Landing on
    // the default one beats starting with no profile at all.
    if (!found) {
        Log::warn(kLog, "unknown profile '{}', falling back to the default one", name);

        for (const Profile& profile : profiles_) {
            if (profile.isDefault) found = &profile;
        }
        if (!found && !profiles_.empty()) found = &profiles_.front();
        if (!found) return false;
    }

    // Taken by value: everything below reaches back into this manager through
    // onDisable and onEnable, and anything that touches profiles_ there would leave
    // a pointer into it dangling.
    const Profile target = *found;
    if (current_.name == target.name) return true;

    if (!current_.name.empty()) saveCurrent();

    const std::string previous = current_.name;

    switching_ = true;

    modules().disableAll();

    current_ = target;
    applyDocument(readJson(fileFor(target.name)));

    switching_ = false;

    ProfileChangeEvent event;
    event.previous = previous;
    event.current = target.name;
    event.automatic = automatic;
    events().emit(event);

    Log::info(kLog, "profile -> {}{}", target.name, automatic ? " (automatique)" : "");
    return true;
}

bool ProfileManager::create(const std::string& name, const std::string& copyFrom) {
    const std::string finalName = uniqueName(name.empty() ? "New profile" : name);

    Profile profile;
    profile.name = finalName;
    profile.modifiedAt = nowMs();

    nlohmann::json document;
    document["formatVersion"] = kFormatVersion;
    document["profile"] = profileToJson(profile);

    if (!copyFrom.empty() && exists(copyFrom)) {
        const nlohmann::json source = readJson(fileFor(copyFrom));
        document["modules"] = source.value("modules", nlohmann::json::object());
    } else {
        document["modules"] = nlohmann::json::object();
    }

    if (!writeJson(fileFor(finalName), document)) return false;

    profiles_.push_back(profile);
    Log::info(kLog, "created profile '{}'", finalName);
    return true;
}

bool ProfileManager::duplicate(const std::string& name, const std::string& newName) {
    return create(newName.empty() ? name + " (copy)" : newName, name);
}

bool ProfileManager::remove(const std::string& name) {
    Profile* profile = findMutable(name);
    if (!profile) return false;

    if (profile->isDefault) {
        Log::warn(kLog, "the default profile cannot be deleted");
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(Paths::profile(name), ec);
    std::erase_if(profiles_, [&](const Profile& p) { return p.name == name; });

    if (current_.name == name) {
        const auto it = std::ranges::find_if(profiles_,
                                             [](const Profile& p) { return p.isDefault; });
        if (it != profiles_.end()) {
            current_.name.clear();
            switchTo(it->name);
        }
    }

    return true;
}

bool ProfileManager::rename(const std::string& from, const std::string& to) {
    Profile* profile = findMutable(from);
    if (!profile || exists(to)) return false;

    std::error_code ec;
    std::filesystem::rename(Paths::profile(from), Paths::profile(to), ec);
    if (ec) {
        Log::error(kLog, "rename failed: {}", ec.message());
        return false;
    }

    profile->name = to;
    if (current_.name == from) current_.name = to;

    writeJson(fileFor(to), [&] {
        nlohmann::json document = readJson(fileFor(to));
        document["profile"] = profileToJson(*profile);
        return document;
    }());

    return true;
}

std::vector<std::string> ProfileManager::names() const {
    std::vector<std::string> result;
    result.reserve(profiles_.size());
    for (const Profile& profile : profiles_) result.push_back(profile.name);
    return result;
}

std::optional<std::string> ProfileManager::profileForServer(std::string_view address,
                                                            std::string_view name) const {
    const std::string haystack = strings::toLower(std::string(address) + " " + std::string(name));

    const Profile* best = nullptr;
    size_t bestLength = 0;

    for (const Profile& profile : profiles_) {
        if (!profile.autoSwitch) continue;

        for (const std::string& match : profile.serverMatches) {
            if (match.empty()) continue;
            if (haystack.find(strings::toLower(match)) == std::string::npos) continue;

            if (match.size() > bestLength) {
                best = &profile;
                bestLength = match.size();
            }
        }
    }

    if (best) return best->name;

    // No rule matched. Falling back to the default profile here would undo the one the
    // player picked by hand every time they joined a world.
    return std::nullopt;
}

void ProfileManager::onWorldJoin(WorldJoinEvent& event) {
    if (!autoSwitchEnabled_) return;

    const auto target = profileForServer(event.serverAddress, event.worldName);
    if (!target || *target == current_.name) return;

    switchTo(*target, true);
}

bool ProfileManager::snapshot(const std::string& label) {
    if (current_.name.empty()) return false;

    nlohmann::json document = serializeCurrent();
    document["versionLabel"] = label;
    document["createdAt"] = nowMs();

    const auto directory = Paths::profileVersions(current_.name);
    const auto path = directory / (timestampId() + ".json");

    if (!writeJson(path, document)) return false;

    std::vector<std::filesystem::path> existing;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().extension() == ".json") existing.push_back(entry.path());
    }

    if (existing.size() > kMaxVersions) {
        std::ranges::sort(existing);
        for (size_t i = 0; i + kMaxVersions < existing.size(); ++i) {
            std::filesystem::remove(existing[i], ec);
        }
    }

    Log::debug(kLog, "created restore point '{}'", label);
    return true;
}

std::vector<ProfileVersion> ProfileManager::versions(const std::string& profile) const {
    std::vector<ProfileVersion> result;

    std::error_code ec;
    const auto directory = Paths::profileVersions(profile);
    if (!std::filesystem::exists(directory, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().extension() != ".json") continue;

        const nlohmann::json document = readJson(entry.path());

        ProfileVersion version;
        version.id = entry.path().stem().string();
        version.label = document.value("versionLabel", std::string("auto"));
        version.createdAt = document.value("createdAt", 0LL);
        result.push_back(std::move(version));
    }

    std::ranges::sort(result, [](const ProfileVersion& a, const ProfileVersion& b) {
        return a.createdAt > b.createdAt;
    });
    return result;
}

bool ProfileManager::restore(const std::string& profile, const std::string& versionId) {
    const auto path = Paths::profileVersions(profile) / (versionId + ".json");

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    snapshot("before restore");

    const nlohmann::json document = readJson(path);
    if (document.is_null()) return false;

    modules().disableAll();
    applyDocument(document);
    saveCurrent();

    Log::info(kLog, "restored profile '{}' from {}", profile, versionId);
    return true;
}

std::string ProfileManager::exportCode() const {
    const std::string payload = serializeCurrent().dump();
    return "VELYX1:" + strings::base64Encode(payload);
}

bool ProfileManager::importCode(std::string_view code, std::string* importedName) {
    constexpr std::string_view kPrefix = "VELYX1:";

    std::string_view body = strings::trim(code);
    if (!strings::startsWith(body, kPrefix)) {
        Log::warn(kLog, "unrecognised share code");
        return false;
    }
    body.remove_prefix(kPrefix.size());

    const auto decoded = strings::base64Decode(body);
    if (!decoded) {
        Log::warn(kLog, "share code is corrupt");
        return false;
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*decoded);
    } catch (const std::exception& e) {
        Log::warn(kLog, "share code could not be parsed: {}", e.what());
        return false;
    }

    Profile profile = profileFromJson(document.value("profile", nlohmann::json::object()),
                                      "Imported profile");
    profile.name = uniqueName(profile.name);
    profile.isDefault = false;
    profile.modifiedAt = nowMs();

    document["profile"] = profileToJson(profile);
    if (!writeJson(fileFor(profile.name), document)) return false;

    profiles_.push_back(profile);
    if (importedName) *importedName = profile.name;

    Log::info(kLog, "imported profile '{}'", profile.name);
    return true;
}

bool ProfileManager::exportFile(const std::string& name,
                                const std::filesystem::path& destination) const {
    const nlohmann::json document = readJson(fileFor(name));
    if (document.is_null()) return false;
    return writeJson(destination, document);
}

bool ProfileManager::importFile(const std::filesystem::path& source, std::string* importedName) {
    const nlohmann::json document = readJson(source);
    if (document.is_null()) return false;

    return importCode("VELYX1:" + strings::base64Encode(document.dump()), importedName);
}

}
