#pragma once

#include <optional>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "dll/event/Events.hpp"

namespace velyx {

struct Profile {
    std::string name;
    std::string description;

    std::vector<std::string> serverMatches;

    bool autoSwitch = true;

    bool isDefault = false;

    long long modifiedAt = 0;
};

struct ProfileVersion {
    std::string id;
    std::string label;
    long long createdAt = 0;
};

class ProfileManager {
public:
    static ProfileManager& get();

    void load();

    void saveCurrent();

    // The interfaces' own settings, saved beside the configuration rather than in a
    // profile. Called when a profile is written and when the client shuts down.
    static void saveInterfaceState();

    bool switchTo(const std::string& name, bool automatic = false);

    bool create(const std::string& name, const std::string& copyFrom = {});
    bool remove(const std::string& name);
    bool rename(const std::string& from, const std::string& to);
    bool duplicate(const std::string& name, const std::string& newName);

    [[nodiscard]] const Profile& current() const { return current_; }
    [[nodiscard]] Profile& mutableCurrent() { return current_; }
    [[nodiscard]] const std::vector<Profile>& all() const { return profiles_; }
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] bool exists(const std::string& name) const;

    void setAutoSwitchEnabled(bool enabled) { autoSwitchEnabled_ = enabled; }
    [[nodiscard]] bool autoSwitchEnabled() const { return autoSwitchEnabled_; }

    [[nodiscard]] std::optional<std::string> profileForServer(std::string_view address,
                                                              std::string_view name) const;

    bool snapshot(const std::string& label = "auto");

    [[nodiscard]] std::vector<ProfileVersion> versions(const std::string& profile) const;
    bool restore(const std::string& profile, const std::string& versionId);

    [[nodiscard]] std::string exportCode() const;

    bool importCode(std::string_view code, std::string* importedName = nullptr);

    bool exportFile(const std::string& name, const std::filesystem::path& destination) const;
    bool importFile(const std::filesystem::path& source, std::string* importedName = nullptr);

private:
    ProfileManager() = default;

    void createStarterProfiles();
    void fillEmptyStarterProfiles();
    void dropShippedServerProfiles();
    [[nodiscard]] nlohmann::json serializeCurrent() const;
    void applyDocument(const nlohmann::json& document);
    [[nodiscard]] std::filesystem::path fileFor(const std::string& name) const;
    [[nodiscard]] Profile* findMutable(const std::string& name);
    [[nodiscard]] std::string uniqueName(const std::string& wanted) const;

    void onWorldJoin(WorldJoinEvent& event);

    std::vector<Profile> profiles_;
    // True while switchTo() is applying a profile: see saveCurrent().
    bool switching_ = false;

    Profile current_;
    bool autoSwitchEnabled_ = true;
    bool loaded_ = false;
};

inline ProfileManager& profiles() { return ProfileManager::get(); }

}
