#include "Paths.hpp"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <optional>

namespace velyx {
namespace {

std::optional<std::filesystem::path> g_root;

std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    return std::filesystem::current_path();
}

}

std::filesystem::path Paths::defaultRoot() {
    return knownFolder(FOLDERID_RoamingAppData) / "Velyx";
}

void Paths::setRoot(const std::filesystem::path& root) { g_root = root; }

const std::filesystem::path& Paths::root() {
    if (!g_root) g_root = defaultRoot();
    return *g_root;
}

std::filesystem::path Paths::config() { return root() / "config"; }
std::filesystem::path Paths::profiles() { return root() / "profiles"; }
std::filesystem::path Paths::themes() { return root() / "themes"; }
std::filesystem::path Paths::scripts() { return root() / "scripts"; }
std::filesystem::path Paths::plugins() { return root() / "plugins"; }
std::filesystem::path Paths::screenshots() { return root() / "screenshots"; }
std::filesystem::path Paths::replays() { return root() / "replays"; }
std::filesystem::path Paths::stats() { return root() / "stats"; }
std::filesystem::path Paths::notes() { return root() / "notes"; }
std::filesystem::path Paths::waypoints() { return root() / "waypoints"; }
std::filesystem::path Paths::logs() { return root() / "logs"; }
std::filesystem::path Paths::crashes() { return logs() / "crashes"; }
std::filesystem::path Paths::cache() { return root() / "cache"; }
std::filesystem::path Paths::assets() { return root() / "assets"; }
std::filesystem::path Paths::instances() { return root() / "instances"; }
std::filesystem::path Paths::versions() { return root() / "versions"; }
std::filesystem::path Paths::accounts() { return root() / "accounts"; }

std::filesystem::path Paths::profile(const std::string& name) {
    return profiles() / sanitize(name);
}

std::filesystem::path Paths::profileVersions(const std::string& name) {
    return profile(name) / "versions";
}

void Paths::ensureLayout() {
    const std::array<std::filesystem::path, 15> directories{
        root(),      config(),  profiles(), themes(),    scripts(),
        plugins(),   screenshots(), replays(), stats(),  notes(),
        waypoints(), logs(),    crashes(),  cache(),     assets(),
    };

    std::error_code ec;
    for (const auto& directory : directories) {
        std::filesystem::create_directories(directory, ec);
    }
}

std::string Paths::sanitize(std::string_view name) {
    static constexpr std::string_view kIllegal = R"(<>:"/\|?*)";

    std::string result;
    result.reserve(name.size());

    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || kIllegal.find(c) != std::string_view::npos) {
            result.push_back('_');
        } else {
            result.push_back(c);
        }
    }

    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) result.pop_back();
    if (result.empty()) result = "unnamed";

    return result;
}

}
