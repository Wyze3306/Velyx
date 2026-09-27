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

    // Counts the session itself, on the frame, rather than waiting for a world to be
    // left: leaving one needs a signature pack, and a session that ends any other way —
    // the game closed from a world, or from the menu — was never counted at all.
    void bind();

    void add(long long seconds);

    // Seconds since the last commit. Every total below already includes them, so a
    // readout is live without having to add the session on top of it.
    [[nodiscard]] long long uncommitted() const;

    [[nodiscard]] long long today() const;
    [[nodiscard]] long long thisWeek() const;
    [[nodiscard]] long long total() const;

    [[nodiscard]] std::vector<PlaytimeDay> lastDays(int count) const;
    [[nodiscard]] long long busiestDaySeconds() const;

    [[nodiscard]] static std::vector<MatchRecord> matches(size_t limit = 100);

private:
    Playtime() = default;

    [[nodiscard]] static std::string todayKey();

    void onFrame(struct FrameEvent& event);
    void onGameClosing(struct GameClosingEvent& event);
    void commit();
    void addOn(const std::string& day, long long seconds);

    std::vector<PlaytimeDay> days_;

    float pendingSeconds_ = 0.f;
    float sinceCommit_ = 0.f;
    std::string pendingDay_;
};

} // namespace velyx
