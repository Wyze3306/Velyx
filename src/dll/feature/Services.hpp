#pragma once

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "dll/event/Events.hpp"

namespace velyx {

class ClickTracker {
public:
    static ClickTracker& get();

    void bind();

    [[nodiscard]] int left() const { return left_; }
    [[nodiscard]] int right() const { return right_; }

    [[nodiscard]] int leftPeak() const { return leftPeak_; }
    [[nodiscard]] int rightPeak() const { return rightPeak_; }

    [[nodiscard]] long long totalClicks() const { return total_; }

    void reset();

private:
    ClickTracker() = default;

    void onKey(KeyEvent& event);
    void onFrame(FrameEvent& event);

    std::deque<long long> leftTimes_;
    std::deque<long long> rightTimes_;
    int left_ = 0;
    int right_ = 0;
    int leftPeak_ = 0;
    int rightPeak_ = 0;
    long long total_ = 0;
};

class FrameStats {
public:
    static FrameStats& get();

    void bind();

    [[nodiscard]] float average() const { return average_; }
    [[nodiscard]] float onePercentLow() const { return onePercentLow_; }
    [[nodiscard]] float pointOnePercentLow() const { return pointOnePercentLow_; }
    [[nodiscard]] float worstFrameMs() const { return worstFrameMs_; }
    [[nodiscard]] int freezes() const { return freezes_; }

    [[nodiscard]] const std::deque<float>& history() const { return history_; }

    void reset();

private:
    FrameStats() = default;

    void onFrame(FrameEvent& event);
    void recompute();

    static constexpr size_t kHistory = 512;

    std::deque<float> history_;
    float average_ = 0.f;
    float onePercentLow_ = 0.f;
    float pointOnePercentLow_ = 0.f;
    float worstFrameMs_ = 0.f;
    int freezes_ = 0;
    float sinceRecompute_ = 0.f;
};

class SessionStats {
public:
    static SessionStats& get();

    void bind();

    [[nodiscard]] long long secondsPlayed() const;
    [[nodiscard]] long long secondsIdle() const;
    [[nodiscard]] float blocksTravelled() const { return blocksTravelled_; }
    [[nodiscard]] int kills() const { return kills_; }
    [[nodiscard]] int deaths() const { return deaths_; }
    [[nodiscard]] const std::string& server() const { return server_; }

    [[nodiscard]] long long idleSeconds() const;

    void addKill() { ++kills_; }
    void addDeath() { ++deaths_; }

    void flush();

private:
    SessionStats() = default;

    void onFrame(FrameEvent& event);
    void onKey(KeyEvent& event);
    void onMouse(MouseEvent& event);
    void onJoin(WorldJoinEvent& event);
    void onLeave(WorldLeaveEvent& event);
    void onDeath(DeathEvent& event);

    long long startedAtMs_ = 0;
    long long lastInputMs_ = 0;
    long long joinedAtMs_ = 0;
    float blocksTravelled_ = 0.f;
    int kills_ = 0;
    int deaths_ = 0;
    std::string server_;
};

class Privacy {
public:
    static Privacy& get();

    bool hideServerAddress = false;
    bool hidePlayerName = false;
    bool hideCoordinates = false;
    bool hideChat = false;

    [[nodiscard]] std::string maskAddress(const std::string& address) const;
    [[nodiscard]] std::string maskName(const std::string& name) const;

    [[nodiscard]] bool anythingHidden() const {
        return hideServerAddress || hidePlayerName || hideCoordinates || hideChat;
    }

private:
    Privacy() = default;
};

void bindServices();

}
