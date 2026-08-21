#include "Lang.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <unordered_map>

#include <json/json.hpp>

#include "core/Log.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Lang";
constexpr const char* kDefaultCode = "en";

struct TransparentHash {
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

using Table = std::unordered_map<std::string, std::string, TransparentHash, std::equal_to<>>;

// The table in use, plus every table loaded before it. Switching language while the
// interface is on screen would otherwise free the strings the current frame is
// drawing; a handful of retired tables costs a few kilobytes and no thinking.
std::shared_ptr<Table> g_active;
std::vector<std::shared_ptr<Table>> g_retired;
std::string g_code = kDefaultCode;

} // namespace

namespace lang {

void load(std::string_view code, const std::filesystem::path& directory) {
    g_code = code.empty() ? kDefaultCode : std::string(code);

    if (g_active) g_retired.push_back(g_active);
    g_active.reset();

    if (g_code == kDefaultCode) {
        Log::info(kLog, "language: english (no table needed)");
        return;
    }

    const auto file = directory / (g_code + ".json");

    std::ifstream stream(file);
    if (!stream) {
        Log::warn(kLog, "no table at {}, staying in english", file.string());
        g_code = kDefaultCode;
        return;
    }

    nlohmann::json json;
    try {
        stream >> json;
    } catch (const std::exception& e) {
        Log::warn(kLog, "{} is not valid JSON: {}", file.filename().string(), e.what());
        g_code = kDefaultCode;
        return;
    }

    if (!json.is_object()) {
        Log::warn(kLog, "{} is not an object", file.filename().string());
        g_code = kDefaultCode;
        return;
    }

    auto table = std::make_shared<Table>();
    for (const auto& [english, translated] : json.items()) {
        if (!translated.is_string()) continue;

        std::string value = translated.get<std::string>();
        if (value.empty()) continue;

        table->emplace(english, std::move(value));
    }

    g_active = std::move(table);
    Log::info(kLog, "language: {} ({} strings)", g_code, g_active->size());
}

const std::string& code() { return g_code; }

std::vector<std::string> available(const std::filesystem::path& directory) {
    std::vector<std::string> codes{kDefaultCode};

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;

        std::string stem = entry.path().stem().string();
        if (stem != kDefaultCode) codes.push_back(std::move(stem));
    }

    std::sort(codes.begin() + 1, codes.end());
    return codes;
}

} // namespace lang

std::string_view tr(std::string_view english) {
    if (!g_active || english.empty()) return english;

    const auto it = g_active->find(english);
    return it == g_active->end() ? english : std::string_view(it->second);
}

} // namespace velyx
