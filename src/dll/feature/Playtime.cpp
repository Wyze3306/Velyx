#include "Playtime.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Playtime";
constexpr int kMaxDays = 400;

std::filesystem::path playtimeFile() { return Paths::stats() / "playtime.json"; }
std::filesystem::path matchesFile() { return Paths::stats() / "matches.json"; }

std::string dateKey(std::chrono::system_clock::time_point point) {
    const auto time = std::chrono::system_clock::to_time_t(point);

    std::tm tm{};
    localtime_s(&tm, &time);

    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday);
    return buffer;
}

} // namespace

Playtime& Playtime::get() {
    static Playtime instance;
    return instance;
}

std::string Playtime::todayKey() { return dateKey(std::chrono::system_clock::now()); }

void Playtime::load() {
    days_.clear();

    std::ifstream stream(playtimeFile());
    if (!stream) return;

    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception& e) {
        Log::warn(kLog, "playtime.json unreadable: {}", e.what());
        return;
    }
    if (!document.is_object()) return;

    for (const auto& [date, seconds] : document.items()) {
        if (!seconds.is_number_integer()) continue;
        days_.push_back(PlaytimeDay{date, seconds.get<long long>()});
    }

    std::ranges::sort(days_, [](const PlaytimeDay& a, const PlaytimeDay& b) {
        return a.date < b.date;
    });

    if (days_.size() > kMaxDays) {
        days_.erase(days_.begin(),
                    days_.begin() + static_cast<std::ptrdiff_t>(days_.size() - kMaxDays));
    }
}

void Playtime::save() const {
    nlohmann::json document = nlohmann::json::object();
    for (const PlaytimeDay& day : days_) document[day.date] = day.seconds;

    std::error_code ec;
    std::filesystem::create_directories(Paths::stats(), ec);

    std::ofstream stream(playtimeFile());
    if (stream) stream << document.dump(1, '\t');
}

void Playtime::add(long long seconds) {
    if (seconds <= 0) return;

    const std::string key = todayKey();
    const auto it = std::ranges::find_if(days_, [&](const PlaytimeDay& d) { return d.date == key; });

    if (it == days_.end()) {
        days_.push_back(PlaytimeDay{key, seconds});
    } else {
        it->seconds += seconds;
    }

    save();
}

long long Playtime::today() const {
    const std::string key = todayKey();
    const auto it = std::ranges::find_if(days_, [&](const PlaytimeDay& d) { return d.date == key; });
    return it == days_.end() ? 0 : it->seconds;
}

long long Playtime::thisWeek() const {
    long long sum = 0;
    for (const PlaytimeDay& day : lastDays(7)) sum += day.seconds;
    return sum;
}

long long Playtime::total() const {
    long long sum = 0;
    for (const PlaytimeDay& day : days_) sum += day.seconds;
    return sum;
}

std::vector<PlaytimeDay> Playtime::lastDays(int count) const {
    std::vector<PlaytimeDay> result;
    if (count <= 0) return result;

    const auto now = std::chrono::system_clock::now();

    for (int offset = count - 1; offset >= 0; --offset) {
        const auto point = now - std::chrono::hours(24 * offset);
        const std::string key = dateKey(point);

        const auto it =
            std::ranges::find_if(days_, [&](const PlaytimeDay& d) { return d.date == key; });
        result.push_back(PlaytimeDay{key, it == days_.end() ? 0 : it->seconds});
    }

    return result;
}

long long Playtime::busiestDaySeconds() const {
    long long best = 0;
    for (const PlaytimeDay& day : days_) best = std::max(best, day.seconds);
    return best;
}

std::vector<MatchRecord> Playtime::matches(size_t limit) {
    std::vector<MatchRecord> records;

    std::ifstream stream(matchesFile());
    if (!stream) return records;

    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception&) {
        return records;
    }
    if (!document.is_array()) return records;

    for (const auto& entry : document) {
        MatchRecord record;
        record.server = entry.value("server", std::string{});
        record.endedAtMs = entry.value("endedAt", 0LL);
        record.seconds = entry.value("seconds", 0LL);
        record.kills = entry.value("kills", 0);
        record.deaths = entry.value("deaths", 0);
        record.blocks = entry.value("blocks", 0.f);
        record.averageFps = entry.value("averageFps", 0.f);
        record.onePercentLow = entry.value("onePercentLow", 0.f);
        records.push_back(std::move(record));
    }

    std::ranges::sort(records, [](const MatchRecord& a, const MatchRecord& b) {
        return a.endedAtMs > b.endedAtMs;
    });

    if (records.size() > limit) records.resize(limit);
    return records;
}

} // namespace velyx
