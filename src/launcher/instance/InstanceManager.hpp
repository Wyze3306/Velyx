#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace velyx {

struct Instance {
    std::string id;
    std::string name;
    std::string accountId;
    std::string gameVersion;
    std::string profile = "Global";

    std::filesystem::path root;
    std::string packageFamilyName;
    std::string applicationId = "App";

    bool registered = false;
    bool injectVelyx = true;
    long long lastPlayedMs = 0;
    long long totalPlaySeconds = 0;

    uint32_t pid = 0;

    [[nodiscard]] bool running() const { return pid != 0; }

    [[nodiscard]] std::string activationId() const {
        return packageFamilyName + "!" + applicationId;
    }
};

class InstanceManager {
public:
    static InstanceManager& get();

    void load();
    void save() const;

    [[nodiscard]] const std::vector<Instance>& all() const { return instances_; }
    [[nodiscard]] Instance* find(const std::string& id);

    [[nodiscard]] static std::optional<std::filesystem::path> findInstalledGame(
        std::string* version = nullptr);

    enum class CloneMode { Link, Copy };
    using ProgressFn = std::function<void(size_t done, size_t total, const std::string& label)>;

    struct VersionSource {
        std::string version;
        std::filesystem::path root;
        bool installed = false;
    };

    [[nodiscard]] static std::vector<VersionSource> availableVersions();

    bool setVersion(Instance& instance, const VersionSource& source, const ProgressFn& onProgress,
                    std::string* error = nullptr);

    bool create(const std::string& name, CloneMode mode, const ProgressFn& onProgress,
                std::string* error = nullptr);

    bool registerInstance(Instance& instance, std::string* error = nullptr);
    bool unregisterInstance(Instance& instance, std::string* error = nullptr);

    bool launch(Instance& instance, std::string* error = nullptr);

    bool remove(const std::string& id, bool deleteFiles, std::string* error = nullptr);

    void refreshRunningState();

    [[nodiscard]] static bool developerModeEnabled();

private:
    InstanceManager() = default;

    // Copying out of C:\Program Files\WindowsApps is allowed to fail file by file, and a
    // half-copied instance registers and activates just as happily as a whole one, so the
    // clone has to say what it could not take rather than only how far it got.
    struct CloneResult {
        size_t copied = 0;
        size_t failed = 0;
        std::string firstError;

        [[nodiscard]] std::string failure() const;
    };

    static CloneResult cloneFiles(const std::filesystem::path& source,
                                  const std::filesystem::path& destination, CloneMode mode,
                                  const ProgressFn& onProgress);

    static bool patchManifest(const std::filesystem::path& manifest, const std::string& packageName,
                              const std::string& displayName, std::string* error);

    [[nodiscard]] static std::string readPublisher(const std::filesystem::path& manifest);

    std::vector<Instance> instances_;
};

}
