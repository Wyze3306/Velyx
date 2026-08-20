#pragma once

#include <string>
#include <vector>

namespace velyx {

struct PlaytimeDay {
    std::string date;
    long long seconds = 0;
};

struct MatchRecord {
    std::string server;
    long long endedAtMs = 0;
    long long seconds = 0;
    int kills = 0;
    int deaths = 0;
    float blocks = 0.f;
    float averageFps = 0.f;
    float onePercentLow = 0.f;
};

class Playtime {
public:
    static Playtime& get();

    void load();
    void save() const;

    void add(long long seconds);

    [[nodiscard]] long long today() const;
    [[nodiscard]] long long thisWeek() const;
    [[nodiscard]] long long total() const;

    [[nodiscard]] std::vector<PlaytimeDay> lastDays(int count) const;
    [[nodiscard]] long long busiestDaySeconds() const;

    [[nodiscard]] static std::vector<MatchRecord> matches(size_t limit = 100);

private:
    Playtime() = default;

    [[nodiscard]] static std::string todayKey();

    std::vector<PlaytimeDay> days_;
};

} // namespace velyx
