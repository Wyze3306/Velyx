#include "Services.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "dll/feature/Playtime.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Services";
constexpr const char* kMaskGlyph = "•";
constexpr const char* kMaskedAddress = "•••";

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

long long wallClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void trimToLastSecond(std::deque<long long>& times, long long now) {
    while (!times.empty() && now - times.front() > 1000) times.pop_front();
}

}

ClickTracker& ClickTracker::get() {
    static ClickTracker instance;
    return instance;
}

void ClickTracker::bind() {
    events().on<KeyEvent>(this, &ClickTracker::onKey, EventPriority::Last);
    events().on<FrameEvent>(this, &ClickTracker::onFrame, EventPriority::First);
}

void ClickTracker::onKey(KeyEvent& event) {
    if (!event.down || event.repeat) return;

    if (event.key == VK_LBUTTON) {
        leftTimes_.push_back(nowMs());
        ++total_;
    } else if (event.key == VK_RBUTTON) {
        rightTimes_.push_back(nowMs());
        ++total_;
    }
}

void ClickTracker::onFrame(FrameEvent& event) {
    const long long now = nowMs();

    trimToLastSecond(leftTimes_, now);
    trimToLastSecond(rightTimes_, now);

    left_ = static_cast<int>(leftTimes_.size());
    right_ = static_cast<int>(rightTimes_.size());

    leftPeak_ = std::max(leftPeak_, left_);
    rightPeak_ = std::max(rightPeak_, right_);
}

void ClickTracker::reset() {
    leftTimes_.clear();
    rightTimes_.clear();
    left_ = right_ = leftPeak_ = rightPeak_ = 0;
    total_ = 0;
}

FrameStats& FrameStats::get() {
    static FrameStats instance;
    return instance;
}

void FrameStats::bind() {
    events().on<FrameEvent>(this, &FrameStats::onFrame, EventPriority::First);
}

void FrameStats::onFrame(FrameEvent& event) {
    if (event.deltaSeconds <= 0.f) return;

    const float frameMs = event.deltaSeconds * 1000.f;

    history_.push_back(frameMs);
    if (history_.size() > kHistory) history_.pop_front();

    if (frameMs > 100.f) ++freezes_;
    worstFrameMs_ = std::max(worstFrameMs_, frameMs);

    sinceRecompute_ += event.deltaSeconds;
    if (sinceRecompute_ >= 0.25f) {
        sinceRecompute_ = 0.f;
        recompute();
    }
}

void FrameStats::recompute() {
    if (history_.empty()) return;

    std::vector<float> sorted(history_.begin(), history_.end());
    std::ranges::sort(sorted);

    double total = 0.0;
    for (const float value : sorted) total += value;
    average_ = static_cast<float>(1000.0 / (total / static_cast<double>(sorted.size())));

    const auto worstCount = std::max<size_t>(1, sorted.size() / 100);
    const auto worstTenth = std::max<size_t>(1, sorted.size() / 1000);

    const auto averageOfWorst = [&](size_t count) {
        double sum = 0.0;
        for (size_t i = sorted.size() - count; i < sorted.size(); ++i) sum += sorted[i];
        return static_cast<float>(1000.0 / (sum / static_cast<double>(count)));
    };

    onePercentLow_ = averageOfWorst(worstCount);
    pointOnePercentLow_ = averageOfWorst(worstTenth);
}

void FrameStats::reset() {
    history_.clear();
    average_ = onePercentLow_ = pointOnePercentLow_ = worstFrameMs_ = 0.f;
    freezes_ = 0;
}

SessionStats& SessionStats::get() {
    static SessionStats instance;
    return instance;
}

void SessionStats::bind() {
    startedAtMs_ = nowMs();
    lastInputMs_ = startedAtMs_;

    events().on<FrameEvent>(this, &SessionStats::onFrame, EventPriority::Last);
    events().on<KeyEvent>(this, &SessionStats::onKey, EventPriority::Last);
    events().on<MouseEvent>(this, &SessionStats::onMouse, EventPriority::Last);
    events().on<WorldJoinEvent>(this, &SessionStats::onJoin);
    events().on<WorldLeaveEvent>(this, &SessionStats::onLeave);
    events().on<DeathEvent>(this, &SessionStats::onDeath);
}

long long SessionStats::secondsPlayed() const { return (nowMs() - startedAtMs_) / 1000; }

long long SessionStats::idleSeconds() const { return (nowMs() - lastInputMs_) / 1000; }

long long SessionStats::secondsIdle() const { return idleSeconds(); }

void SessionStats::onFrame(FrameEvent& event) {
    static Vec3 previous;

    const auto& player = sdk::game().player();
    if (!player.valid) {
        previous = {};
        return;
    }

    const float step = player.position.flatDistanceTo(previous);

    if (step > 0.f && step < 8.f) blocksTravelled_ += step;
    previous = player.position;
}

void SessionStats::onKey(KeyEvent& event) {
    if (event.down) lastInputMs_ = nowMs();
}

void SessionStats::onMouse(MouseEvent& event) {
    if (event.action != MouseAction::Move) lastInputMs_ = nowMs();
}

void SessionStats::onJoin(WorldJoinEvent& event) {
    joinedAtMs_ = nowMs();
    server_ = event.serverAddress.empty() ? event.worldName : event.serverAddress;
}

void SessionStats::onLeave(WorldLeaveEvent& event) { flush(); }

void SessionStats::onDeath(DeathEvent& event) { ++deaths_; }

void SessionStats::flush() {
    if (joinedAtMs_ == 0) return;

    const long long durationSeconds = (nowMs() - joinedAtMs_) / 1000;
    joinedAtMs_ = 0;

    if (durationSeconds < 15) return;

    const auto path = Paths::stats() / "matches.json";

    nlohmann::json history = nlohmann::json::array();
    if (std::ifstream input(path); input) {
        try {
            input >> history;
        } catch (const std::exception&) {
            history = nlohmann::json::array();
        }
        if (!history.is_array()) history = nlohmann::json::array();
    }

    nlohmann::json entry;
    entry["server"] = server_;
    entry["endedAt"] = wallClockMs();
    entry["seconds"] = durationSeconds;
    entry["kills"] = kills_;
    entry["deaths"] = deaths_;
    entry["blocks"] = blocksTravelled_;
    entry["averageFps"] = FrameStats::get().average();
    entry["onePercentLow"] = FrameStats::get().onePercentLow();
    history.push_back(entry);

    while (history.size() > 500) history.erase(history.begin());

    std::error_code ec;
    std::filesystem::create_directories(Paths::stats(), ec);

    if (std::ofstream output(path); output) {
        output << history.dump(1, '\t');
        Log::debug(kLog, "recorded match on {} ({}s)", server_, durationSeconds);
    }

    Playtime::get().add(durationSeconds);

    kills_ = 0;
    deaths_ = 0;
}

Privacy& Privacy::get() {
    static Privacy instance;
    return instance;
}

std::string Privacy::maskAddress(const std::string& address) const {
    if (!hideServerAddress || address.empty()) return address;

    const size_t lastDot = address.rfind('.');
    if (lastDot == std::string::npos) return kMaskedAddress;
    return kMaskedAddress + address.substr(lastDot);
}

std::string Privacy::maskName(const std::string& name) const {
    if (!hidePlayerName || name.empty()) return name;

    std::string masked = name.substr(0, 1);
    for (size_t i = 1; i < name.size(); ++i) masked += kMaskGlyph;
    return masked;
}

void bindServices() {
    ClickTracker::get().bind();
    FrameStats::get().bind();
    SessionStats::get().bind();

    Log::debug(kLog, "services bound");
}

}
