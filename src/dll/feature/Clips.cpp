#include "Clips.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "dll/feature/Services.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Clips";
constexpr size_t kMaxMarkers = 500;

std::filesystem::path clipsFile() { return Paths::stats() / "clips.json"; }

long long wallClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

Clips& Clips::get() {
    static Clips instance;
    return instance;
}

void Clips::load() {
    if (loaded_) return;
    loaded_ = true;

    std::ifstream stream(clipsFile());
    if (!stream) return;

    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception& e) {
        Log::warn(kLog, "clips.json unreadable: {}", e.what());
        return;
    }
    if (!document.is_array()) return;

    for (const auto& entry : document) {
        ClipMarker marker;
        marker.atMs = entry.value("at", 0LL);
        marker.sessionSeconds = entry.value("session", 0LL);
        marker.server = entry.value("server", std::string{});
        marker.note = entry.value("note", std::string{});
        markers_.push_back(std::move(marker));
    }
}

void Clips::save() const {
    nlohmann::json document = nlohmann::json::array();

    for (const ClipMarker& marker : markers_) {
        nlohmann::json entry;
        entry["at"] = marker.atMs;
        entry["session"] = marker.sessionSeconds;
        entry["server"] = marker.server;
        entry["note"] = marker.note;
        document.push_back(entry);
    }

    std::error_code ec;
    std::filesystem::create_directories(Paths::stats(), ec);

    std::ofstream stream(clipsFile());
    if (stream) stream << document.dump(1, '\t');
}

ClipMarker Clips::mark(std::string note) {
    load();

    const auto& world = sdk::game().world();

    ClipMarker marker;
    marker.atMs = wallClockMs();
    marker.sessionSeconds = SessionStats::get().secondsPlayed();
    marker.server = world.serverAddress.empty() ? world.worldName : world.serverAddress;
    marker.note = std::move(note);

    markers_.push_back(marker);
    while (markers_.size() > kMaxMarkers) markers_.erase(markers_.begin());

    save();
    Log::info(kLog, "clip marker at {}s on {}", marker.sessionSeconds,
              marker.server.empty() ? "solo" : marker.server);

    return marker;
}

std::vector<ClipMarker> Clips::recent(size_t limit) const {
    std::vector<ClipMarker> result(markers_.rbegin(), markers_.rend());
    if (result.size() > limit) result.resize(limit);
    return result;
}

void Clips::clear() {
    markers_.clear();
    save();
}

} // namespace velyx
