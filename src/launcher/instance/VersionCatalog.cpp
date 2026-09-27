#include "VersionCatalog.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"

namespace velyx::catalog {
namespace {

constexpr const char* kLog = "Catalog";
constexpr const char* kSource =
    "https://raw.githubusercontent.com/MinecraftBedrockArchiver/GdkLinks/master/urls.json";
constexpr const char* kCacheFile = "versions-gdk.json";

std::vector<CatalogVersion> g_versions;
bool g_loaded = false;

std::filesystem::path cacheFile() { return Paths::cache() / kCacheFile; }

// "1.26.44.3" against "1.21.120.4": four numbers, compared as numbers. Read as text,
// 1.21.120 sorts above 1.26.44, which would bury every recent build under an old one.
std::vector<unsigned long long> parts(const std::string& version) {
    std::vector<unsigned long long> numbers;
    for (const std::string& part : strings::split(version, '.')) {
        unsigned long long value = 0;
        std::from_chars(part.data(), part.data() + part.size(), value);
        numbers.push_back(value);
    }
    return numbers;
}

bool newerFirst(const CatalogVersion& a, const CatalogVersion& b) {
    const auto left = parts(a.version);
    const auto right = parts(b.version);

    for (size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const unsigned long long l = i < left.size() ? left[i] : 0;
        const unsigned long long r = i < right.size() ? right[i] : 0;
        if (l != r) return l > r;
    }

    // Same build on both channels: the release is the one most people want on top.
    return !a.preview && b.preview;
}

void readChannel(const nlohmann::json& document, const char* key, bool preview,
                 std::vector<CatalogVersion>& into) {
    const auto channel = document.find(key);
    if (channel == document.end() || !channel->is_object()) return;

    for (const auto& [version, mirrors] : channel->items()) {
        if (!mirrors.is_array() || mirrors.empty()) continue;

        CatalogVersion entry;
        entry.version = version;
        entry.preview = preview;

        for (const auto& mirror : mirrors) {
            if (mirror.is_string()) entry.mirrors.push_back(mirror.get<std::string>());
        }

        if (!entry.mirrors.empty()) into.push_back(std::move(entry));
    }
}

bool parse(const std::string& body, std::vector<CatalogVersion>& into, std::string* error) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        if (error) *error = std::string("the version list is unreadable: ") + e.what();
        return false;
    }

    readChannel(document, "release", false, into);
    readChannel(document, "preview", true, into);

    if (into.empty()) {
        if (error) *error = "the version list came back empty";
        return false;
    }

    std::ranges::sort(into, newerFirst);
    return true;
}

} // namespace

const std::vector<CatalogVersion>& versions() {
    if (g_loaded) return g_versions;
    g_loaded = true;

    std::ifstream stream(cacheFile());
    if (!stream) return g_versions;

    const std::string body((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());

    std::vector<CatalogVersion> parsed;
    if (parse(body, parsed, nullptr)) {
        g_versions = std::move(parsed);
        Log::info(kLog, "{} build(s) known from the cache", g_versions.size());
    }

    return g_versions;
}

bool refresh(std::string* error) {
    std::string reason;
    const std::string body = http::get(kSource, &reason);

    if (body.empty()) {
        if (error) *error = "the version list could not be read: " + reason;
        return false;
    }

    std::vector<CatalogVersion> parsed;
    if (!parse(body, parsed, error)) return false;

    std::error_code ec;
    std::filesystem::create_directories(Paths::cache(), ec);

    if (std::ofstream out(cacheFile(), std::ios::binary | std::ios::trunc); out) {
        out << body;
    }

    g_versions = std::move(parsed);
    g_loaded = true;

    Log::info(kLog, "{} build(s) listed", g_versions.size());
    return true;
}

std::filesystem::path packageFile(const CatalogVersion& version) {
    return Paths::cache() / "packages" / version.fileName();
}

bool fetch(const CatalogVersion& version, const http::ProgressFn& onProgress, std::string* error) {
    const auto target = packageFile(version);

    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        Log::info(kLog, "{} is already here", target.filename().string());
        return true;
    }

    std::string last;

    // The archive lists the same package on assets1 and assets2. One of them being
    // down, or having forgotten this build, says nothing about the other.
    for (const std::string& mirror : version.mirrors) {
        std::string reason;
        if (http::download(mirror, target, onProgress, &reason)) return true;

        Log::warn(kLog, "{} refused {}: {}", mirror, version.version, reason);
        last = std::move(reason);

        if (last == "download cancelled") break;
    }

    if (error) {
        *error = version.mirrors.empty() ? "no mirror lists " + version.version
                                         : "no mirror would hand out " + version.version + ": " + last;
    }
    return false;
}

} // namespace velyx::catalog

namespace velyx {

std::string CatalogVersion::label() const {
    return preview ? version + " (preview)" : version;
}

std::string CatalogVersion::fileName() const {
    return (preview ? "MinecraftPreview-" : "Minecraft-") + version + ".msixvc";
}

} // namespace velyx
