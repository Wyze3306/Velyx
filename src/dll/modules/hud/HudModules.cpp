#include "HudModules.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/feature/Services.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/modules/hud/TextHud.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr const char* kUnknown = "--";

std::string formatCoordinate(float value, int decimals) {
    return strings::formatFloat(static_cast<double>(value), decimals);
}

class FpsHud final : public TextHud {
public:
    FpsHud()
        : TextHud("fps", "FPS", "Frames per second, with colour thresholds.",
                  {0.01f, 0.02f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Thresholds");
        settings.toggle("colorThresholds", "Colour by performance", true);
        settings.intSlider("warnBelow", "Amber threshold", 60, 10, 240, "", " FPS");
        settings.intSlider("badBelow", "Red threshold", 30, 5, 120, "", " FPS");
        settings.toggle("showLows", "Show 1% lows", false);

        addKeywords({"fps", "frames", "performance", "framerate"});
    }

    std::vector<Row> rows() override {
        const auto& active = theme();
        const float fps = Velyx::get().fps();

        Color color{};
        if (settings.value<bool>("colorThresholds", true)) {
            const int warn = settings.value<int>("warnBelow", 60);
            const int bad = settings.value<int>("badBelow", 30);
            if (fps < static_cast<float>(bad)) {
                color = active.danger;
            } else if (fps < static_cast<float>(warn)) {
                color = active.warning;
            }
        }

        std::vector<Row> result;
        result.push_back(Row{"FPS", std::to_string(static_cast<int>(std::round(fps))), color});

        if (settings.value<bool>("showLows", false)) {
            const float low = FrameStats::get().onePercentLow();
            result.push_back(Row{"1%", low > 0.f ? std::to_string(static_cast<int>(low)) : kUnknown,
                                 {}});
        }

        return result;
    }
};

class CpsHud final : public TextHud {
public:
    CpsHud()
        : TextHud("cps", "CPS", "Clicks per second, left and right.",
                  {0.01f, 0.08f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Buttons");
        settings.toggle("showLeft", "Left click", true);
        settings.toggle("showRight", "Right click", false);
        settings.toggle("combined", "Merge onto one line", false);
        settings.toggle("showPeak", "Show the session best", false);

        addKeywords({"cps", "clicks", "mouse"});
    }

    std::vector<Row> rows() override {
        const ClickTracker& tracker = ClickTracker::get();
        const bool left = settings.value<bool>("showLeft", true);
        const bool right = settings.value<bool>("showRight", false);
        const bool peak = settings.value<bool>("showPeak", false);

        std::vector<Row> result;

        if (settings.value<bool>("combined", false) && left && right) {
            result.push_back(Row{"CPS", std::format("{} | {}", tracker.left(), tracker.right()), {}});
        } else {
            if (left) {
                result.push_back(Row{"CPS", std::to_string(tracker.left()), {}});
            }
            if (right) {
                result.push_back(Row{"CPS R", std::to_string(tracker.right()), {}});
            }
        }

        if (peak) {
            result.push_back(Row{"Max", std::to_string(std::max(tracker.leftPeak(),
                                                                tracker.rightPeak())), {}});
        }

        return result;
    }
};

class ClockHud final : public TextHud {
public:
    ClockHud()
        : TextHud("clock", "Clock", "System time, in the format you choose.",
                  {0.99f, 0.02f}, HudAnchor::TopRight) {
        addTextSettings(false);

        settings.header("Format");
        settings.dropdown("format", "Display", "24 h", {"24 h", "12 h", "With seconds"});
        settings.toggle("showDate", "Show the date", false);

        addKeywords({"time", "clock"});
    }

    std::vector<Row> rows() override {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};
        localtime_s(&tm, &time);

        const std::string format = settings.value<std::string>("format", "24 h");

        char buffer[64]{};
        if (format == "12 h") {
            const int hour12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
            std::snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour12, tm.tm_min,
                          tm.tm_hour < 12 ? "AM" : "PM");
        } else if (format == "With seconds") {
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
                          tm.tm_sec);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d", tm.tm_hour, tm.tm_min);
        }

        std::vector<Row> result{Row{"", buffer, {}}};

        if (settings.value<bool>("showDate", false)) {
            char date[32]{};
            std::snprintf(date, sizeof(date), "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1,
                          tm.tm_year + 1900);
            result.push_back(Row{"", date, {}});
        }

        return result;
    }
};

class CoordinatesHud final : public TextHud {
public:
    CoordinatesHud()
        : TextHud("coordinates", "Coordinates", "Position, dimension and facing.",
                  {0.01f, 0.97f}, HudAnchor::BottomLeft) {
        addTextSettings(true);

        settings.header("Display");
        settings.intSlider("decimals", "Decimals", 0, 0, 3);
        settings.toggle("singleLine", "On a single line", true);
        settings.toggle("showNether", "Nether coordinates", false,
                        "Shows the value divided by 8.");
        settings.toggle("showDirection", "Direction", true);
        settings.toggle("showChunk", "Position in the chunk", false);

        addKeywords({"coords", "coordinates", "position", "xyz"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        const auto& player = sdk::game().player();
        const int decimals = settings.value<int>("decimals", 0);

        if (!player.valid) return {Row{"XYZ", kUnknown, {}}};

        if (Privacy::get().hideCoordinates) return {Row{"XYZ", "•••", {}}};

        std::vector<Row> result;

        const std::string x = formatCoordinate(player.position.x, decimals);
        const std::string y = formatCoordinate(player.position.y, decimals);
        const std::string z = formatCoordinate(player.position.z, decimals);

        if (settings.value<bool>("singleLine", true)) {
            result.push_back(Row{"XYZ", std::format("{} {} {}", x, y, z), {}});
        } else {
            result.push_back(Row{"X", x, {}});
            result.push_back(Row{"Y", y, {}});
            result.push_back(Row{"Z", z, {}});
        }

        if (settings.value<bool>("showNether", false)) {
            result.push_back(Row{"Nether",
                                 std::format("{} {}", formatCoordinate(player.position.x / 8.f, 0),
                                             formatCoordinate(player.position.z / 8.f, 0)),
                                 {}});
        }

        if (settings.value<bool>("showChunk", false)) {
            const int chunkX = static_cast<int>(std::floor(player.position.x)) & 15;
            const int chunkZ = static_cast<int>(std::floor(player.position.z)) & 15;
            result.push_back(Row{"Chunk", std::format("{} {}", chunkX, chunkZ), {}});
        }

        if (settings.value<bool>("showDirection", true)) {
            result.push_back(Row{"Facing", sdk::Game::compass(player.yaw), {}});
        }

        return result;
    }
};

class DirectionHud final : public TextHud {
public:
    DirectionHud()
        : TextHud("direction", "Direction", "A compass, in text.",
                  {0.5f, 0.03f}, HudAnchor::TopCenter) {
        addTextSettings(false);

        settings.header("Display");
        settings.toggle("longNames", "Full names", false);
        settings.toggle("showDegrees", "Show degrees", false);

        addKeywords({"compass", "direction", "north"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        const auto& player = sdk::game().player();
        if (!player.valid) return {Row{"", kUnknown, {}}};

        const char* name = settings.value<bool>("longNames", false)
                               ? sdk::Game::compassLong(player.yaw)
                               : sdk::Game::compass(player.yaw);

        if (settings.value<bool>("showDegrees", false)) {
            return {Row{"", std::format("{} ({}°)", name,
                                        static_cast<int>(std::round(wrapAngle(player.yaw)))), {}}};
        }
        return {Row{"", name, {}}};
    }
};

class SpeedHud final : public TextHud {
public:
    SpeedHud()
        : TextHud("speed", "Speed", "Horizontal speed in blocks per second.",
                  {0.01f, 0.14f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Unit");
        settings.dropdown("unit", "Unit", "b/s", {"b/s", "km/h", "b/tick"});
        settings.intSlider("decimals", "Decimals", 2, 0, 3);

        addKeywords({"speed", "bps"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        if (!sdk::game().player().valid) return {Row{"Speed", kUnknown, {}}};

        const float blocksPerSecond = sdk::game().horizontalSpeed();
        const std::string unit = settings.value<std::string>("unit", "b/s");
        const int decimals = settings.value<int>("decimals", 2);

        float value = blocksPerSecond;
        if (unit == "km/h") {
            value = blocksPerSecond * 3.6f;
        } else if (unit == "b/tick") {
            value = blocksPerSecond / 20.f;
        }

        return {Row{"Speed", std::format("{} {}", strings::formatFloat(value, decimals), unit), {}}};
    }
};

class PingHud final : public TextHud {
public:
    PingHud()
        : TextHud("ping", "Ping", "Latency to the server.",
                  {0.99f, 0.08f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Thresholds");
        settings.toggle("colorThresholds", "Colour by latency", true);
        settings.intSlider("warnAbove", "Amber threshold", 100, 20, 500, "", " ms");
        settings.intSlider("badAbove", "Red threshold", 200, 50, 1000, "", " ms");

        addKeywords({"ping", "latency", "ms", "network"});
    }

    bool relevantNow() const override { return sdk::game().world().multiplayer; }

    std::vector<Row> rows() override {
        const float ping = sdk::game().world().ping;
        if (ping < 0.f) return {Row{"Ping", kUnknown, {}}};

        Color color{};
        if (settings.value<bool>("colorThresholds", true)) {
            const auto& active = theme();
            if (ping > static_cast<float>(settings.value<int>("badAbove", 200))) {
                color = active.danger;
            } else if (ping > static_cast<float>(settings.value<int>("warnAbove", 100))) {
                color = active.warning;
            } else {
                color = active.success;
            }
        }

        return {Row{"Ping", std::format("{} ms", static_cast<int>(ping)), color}};
    }
};

class MemoryHud final : public TextHud {
public:
    MemoryHud()
        : TextHud("memory", "Memory", "Memory the game is using.",
                  {0.99f, 0.14f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Display");
        settings.dropdown("unit", "Unit", "MiB", {"MiB", "GiB"});

        addKeywords({"ram", "memory"});
    }

    std::vector<Row> rows() override {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters))) {
            return {Row{"RAM", kUnknown, {}}};
        }

        const double bytes = static_cast<double>(counters.WorkingSetSize);

        if (settings.value<std::string>("unit", "MiB") == "GiB") {
            return {Row{"RAM", strings::formatFloat(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GiB",
                        {}}};
        }
        return {Row{"RAM", strings::formatFloat(bytes / (1024.0 * 1024.0), 0) + " MiB", {}}};
    }
};

class IpDisplayHud final : public TextHud {
public:
    IpDisplayHud()
        : TextHud("ip_display", "Server address", "The server you are on, hideable for a stream.",
                  {0.5f, 0.97f}, HudAnchor::BottomCenter) {
        addTextSettings(false);
        addKeywords({"ip", "server", "address"});
    }

    bool relevantNow() const override { return sdk::game().world().multiplayer; }

    std::vector<Row> rows() override {
        const auto& world = sdk::game().world();
        if (world.serverAddress.empty()) return {Row{"", kUnknown, {}}};

        return {Row{"", Privacy::get().maskAddress(world.serverAddress), {}}};
    }
};

class AfkTimerHud final : public TextHud {
public:
    AfkTimerHud()
        : TextHud("afk_timer", "AFK timer", "Time since the last action.",
                  {0.5f, 0.1f}, HudAnchor::TopCenter) {
        addTextSettings(true);

        settings.header("Trigger");
        settings.intSlider("showAfter", "Show after", 30, 5, 600, "", " s");

        addKeywords({"afk", "idle"});
    }

    bool relevantNow() const override {
        return SessionStats::get().idleSeconds() >=
               static_cast<long long>(settings.value<int>("showAfter", 30));
    }

    std::vector<Row> rows() override {
        const long long idle = SessionStats::get().idleSeconds();
        return {Row{"AFK", strings::formatDuration(idle), theme().warning}};
    }
};

class SessionStatsHud final : public TextHud {
public:
    SessionStatsHud()
        : TextHud("session_stats", "Session stats",
                  "Duration, distance travelled, kills and average FPS.",
                  {0.99f, 0.4f}, HudAnchor::MiddleRight) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showTime", "Playtime", true);
        settings.toggle("showBlocks", "Blocks travelled", true);
        settings.toggle("showCombat", "Kills / deaths", true);
        settings.toggle("showFps", "Average FPS", true);
        settings.toggle("showClicks", "Total clicks", false);

        addKeywords({"stats", "session"});
    }

    std::vector<Row> rows() override {
        const SessionStats& stats = SessionStats::get();
        std::vector<Row> result;

        if (settings.value<bool>("showTime", true)) {
            result.push_back(Row{"Duration", strings::formatDuration(stats.secondsPlayed()), {}});
        }
        if (settings.value<bool>("showBlocks", true)) {
            result.push_back(Row{"Blocks",
                                 strings::formatThousands(
                                     static_cast<long long>(stats.blocksTravelled())), {}});
        }
        if (settings.value<bool>("showCombat", true)) {
            result.push_back(Row{"K/D", std::format("{} / {}", stats.kills(), stats.deaths()), {}});
        }
        if (settings.value<bool>("showFps", true)) {
            const float average = FrameStats::get().average();
            result.push_back(Row{"Avg FPS",
                                 average > 0.f ? std::to_string(static_cast<int>(average))
                                               : kUnknown, {}});
        }
        if (settings.value<bool>("showClicks", false)) {
            result.push_back(Row{"Clicks",
                                 strings::formatThousands(ClickTracker::get().totalClicks()), {}});
        }

        return result;
    }
};

class StopwatchHud final : public TextHud {
public:
    StopwatchHud()
        : TextHud("stopwatch", "Stopwatch", "A manual stopwatch for practice.",
                  {0.5f, 0.15f}, HudAnchor::TopCenter) {
        addTextSettings(false);

        settings.header("Controls");
        settings.keybind("toggleKey", "Start / stop", Keybind{VK_F7});
        settings.keybind("resetKey", "Reset", Keybind{VK_F7, false, true});
        settings.toggle("showMilliseconds", "Milliseconds", true);

        always(&StopwatchHud::onKey);
        addKeywords({"stopwatch", "timer", "practice"});
    }

    std::vector<Row> rows() override {
        const long long elapsed = running_ ? accumulatedMs_ + (nowMs() - startedAtMs_)
                                           : accumulatedMs_;

        const long long minutes = elapsed / 60000;
        const long long seconds = (elapsed / 1000) % 60;
        const long long millis = elapsed % 1000;

        const std::string text =
            settings.value<bool>("showMilliseconds", true)
                ? std::format("{:02}:{:02}.{:03}", minutes, seconds, millis)
                : std::format("{:02}:{:02}", minutes, seconds);

        return {Row{"", text, running_ ? theme().liveAccent() : Color{}}};
    }

private:
    static long long nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void onKey(KeyEvent& event) {
        if (!enabled() || !event.down || event.repeat) return;

        const Keybind toggleKey = settings.value<Keybind>("toggleKey", Keybind{});
        const Keybind resetKey = settings.value<Keybind>("resetKey", Keybind{});

        if (resetKey.bound() && event.key == resetKey.key && event.shift == resetKey.shift &&
            event.ctrl == resetKey.ctrl && event.alt == resetKey.alt) {
            running_ = false;
            accumulatedMs_ = 0;
            return;
        }

        if (toggleKey.bound() && event.key == toggleKey.key && event.shift == toggleKey.shift &&
            event.ctrl == toggleKey.ctrl && event.alt == toggleKey.alt) {
            if (running_) {
                accumulatedMs_ += nowMs() - startedAtMs_;
                running_ = false;
            } else {
                startedAtMs_ = nowMs();
                running_ = true;
            }
        }
    }

    bool running_ = false;
    long long startedAtMs_ = 0;
    long long accumulatedMs_ = 0;
};

class KeystrokesHud final : public HudModule {
public:
    KeystrokesHud()
        : HudModule("keystrokes", "Keystrokes", "Shows the movement keys and the clicks.",
                    {0.03f, 0.6f}, HudAnchor::MiddleLeft) {
        settings.header("Layout");
        settings.slider("keySize", "Key size", 30.f, 16.f, 60.f, "", "px");
        settings.slider("gap", "Spacing", 3.f, 0.f, 12.f, "", "px");
        settings.toggle("showMouse", "Mouse buttons", true);
        settings.toggle("showSpace", "Space bar", true);
        settings.toggle("showCps", "CPS on the buttons", true);

        settings.header("Colours");
        settings.color("idleColor", "Key released", Color::rgb8(11, 31, 23, 170));
        settings.color("pressedColor", "Key pressed", palette::kMint);
        settings.toggle("accentPressed", "Use the theme's accent", true);

        if (Setting* pressed = settings.find("pressedColor")) {
            pressed->visibleWhen = [this] { return !settings.value<bool>("accentPressed", true); };
        }
        if (Setting* cps = settings.find("showCps")) {
            cps->visibleWhen = [this] { return settings.value<bool>("showMouse", true); };
        }

        addKeywords({"keys", "keystrokes", "wasd", "clavier"});
    }

    Vec2 contentSize(Renderer& renderer) override {
        const float size = keySize();
        const float gap = settings.value<float>("gap", 3.f) * scale();

        float height = size * 2.f + gap;
        if (settings.value<bool>("showSpace", true)) height += size * 0.6f + gap;
        if (settings.value<bool>("showMouse", true)) height += size + gap;

        return {size * 3.f + gap * 2.f, height};
    }

    void drawContent(Renderer& renderer, const Rect& content) override {
        const float size = keySize();
        const float gap = settings.value<float>("gap", 3.f) * scale();
        const float radius = settings.value<float>("radius", 8.f) * 0.6f;

        const float left = content.left;
        float y = content.top;

        drawKey(renderer, Rect::fromSize(left + size + gap, y, size, size), "W", 'Z', radius);
        y += size + gap;

        drawKey(renderer, Rect::fromSize(left, y, size, size), "A", 'Q', radius);
        drawKey(renderer, Rect::fromSize(left + size + gap, y, size, size), "S", 'S', radius);
        drawKey(renderer, Rect::fromSize(left + (size + gap) * 2.f, y, size, size), "D", 'D',
                radius);
        y += size + gap;

        const float fullWidth = size * 3.f + gap * 2.f;

        if (settings.value<bool>("showSpace", true)) {
            const float height = size * 0.6f;
            drawKey(renderer, Rect::fromSize(left, y, fullWidth, height), "___", VK_SPACE, radius);
            y += height + gap;
        }

        if (settings.value<bool>("showMouse", true)) {
            const float half = (fullWidth - gap) * 0.5f;
            const bool showCps = settings.value<bool>("showCps", true);

            const std::string leftLabel =
                showCps ? std::to_string(ClickTracker::get().left()) : "CG";
            const std::string rightLabel =
                showCps ? std::to_string(ClickTracker::get().right()) : "CD";

            drawKey(renderer, Rect::fromSize(left, y, half, size), leftLabel, VK_LBUTTON, radius);
            drawKey(renderer, Rect::fromSize(left + half + gap, y, half, size), rightLabel,
                    VK_RBUTTON, radius);
        }
    }

private:
    [[nodiscard]] float keySize() const {
        return settings.value<float>("keySize", 30.f) * scale();
    }

    void drawKey(Renderer& renderer, const Rect& rect, const std::string& label, int virtualKey,
                 float radius) {
        const auto& active = theme();
        const bool down = WindowHook::isKeyDown(virtualKey);

        Animated& animation = animations_[virtualKey];
        animation.speed = active.motion(20.f);
        animation.to(down ? 1.f : 0.f);
        animation.update(renderer.delta());

        const Color idle = settings.value<Color>("idleColor", Color::rgb8(11, 31, 23, 170));
        const Color pressed = settings.value<bool>("accentPressed", true)
                                  ? active.liveAccent()
                                  : settings.value<Color>("pressedColor", palette::kMint);

        const Color background = lerp(idle, pressed, animation.value);
        renderer.fillRounded(rect, background, radius);

        FontSpec spec = fontFor(1.f, FontWeight::SemiBold);
        spec.size = rect.height() * 0.42f;
        spec.align = TextAlign::Center;
        spec.valign = TextVAlign::Middle;

        const Color foreground = lerp(textColor(), pressed.readableForeground(), animation.value);
        renderer.text(label, rect, foreground, spec);
    }

    std::unordered_map<int, Animated> animations_;
};

class FpsGraphHud final : public HudModule {
public:
    FpsGraphHud()
        : HudModule("fps_graph", "FPS graph",
                    "Frame times, freezes and 1% lows over the last few seconds.",
                    {0.99f, 0.97f}, HudAnchor::BottomRight) {
        settings.header("Graph");
        settings.slider("width", "Width", 220.f, 80.f, 600.f, "", "px");
        settings.slider("height", "Height", 70.f, 30.f, 240.f, "", "px");
        settings.intSlider("samples", "Samples", 180, 30, 512);
        settings.toggle("fill", "Fill under the curve", true);
        settings.toggle("showTargetLine", "Reference line", true);
        settings.intSlider("target", "Target FPS", 60, 30, 360, "", " FPS");
        settings.toggle("showLegend", "Numbered legend", true);

        addKeywords({"graphique", "graph", "frametime", "lows", "performance"});
    }

    Vec2 contentSize(Renderer& renderer) override {
        const float legend = settings.value<bool>("showLegend", true)
                                 ? fontFor().size * 1.3f
                                 : 0.f;
        return {settings.value<float>("width", 220.f) * scale(),
                settings.value<float>("height", 70.f) * scale() + legend};
    }

    void drawContent(Renderer& renderer, const Rect& content) override {
        const auto& active = theme();
        const FrameStats& stats = FrameStats::get();
        const auto& history = stats.history();

        const bool showLegend = settings.value<bool>("showLegend", true);
        const float legendHeight = showLegend ? fontFor().size * 1.3f : 0.f;
        const Rect plot{content.left, content.top, content.right, content.bottom - legendHeight};

        if (history.size() < 2) {
            FontSpec spec = fontFor();
            spec.align = TextAlign::Center;
            spec.valign = TextVAlign::Middle;
            renderer.text("Measuring…", plot, textColor().fade(0.6f), spec);
            return;
        }

        const auto sampleCount =
            std::min<size_t>(history.size(),
                             static_cast<size_t>(settings.value<int>("samples", 180)));
        const size_t first = history.size() - sampleCount;

        float worst = 8.f;
        for (size_t i = first; i < history.size(); ++i) worst = std::max(worst, history[i]);
        worst *= 1.15f;

        std::vector<Vec2> points;
        points.reserve(sampleCount);

        for (size_t i = first; i < history.size(); ++i) {
            const float t = static_cast<float>(i - first) / static_cast<float>(sampleCount - 1);
            const float x = plot.left + t * plot.width();
            const float y = plot.bottom - (history[i] / worst) * plot.height();
            points.push_back({x, clamp(y, plot.top, plot.bottom)});
        }

        if (settings.value<bool>("fill", true)) {
            std::vector<Vec2> polygon = points;
            polygon.push_back({plot.right, plot.bottom});
            polygon.push_back({plot.left, plot.bottom});
            renderer.fillPolygon(polygon, active.liveAccent().fade(0.18f));
        }

        if (settings.value<bool>("showTargetLine", true)) {
            const float targetMs = 1000.f / static_cast<float>(settings.value<int>("target", 60));
            const float y = plot.bottom - (targetMs / worst) * plot.height();
            if (y > plot.top && y < plot.bottom) {
                renderer.line({plot.left, y}, {plot.right, y}, active.textMuted.fade(0.45f), 1.f);
            }
        }

        renderer.polyline(points, active.liveAccent(), 1.6f);

        if (showLegend) {
            FontSpec spec = fontFor(0.85f, FontWeight::Medium);
            const Rect legend{content.left, plot.bottom, content.right, content.bottom};

            const std::string text = std::format(
                "moy {}  ·  1% {}  ·  0.1% {}  ·  freezes {}",
                static_cast<int>(stats.average()), static_cast<int>(stats.onePercentLow()),
                static_cast<int>(stats.pointOnePercentLow()), stats.freezes());

            renderer.text(text, legend, textColor().fade(0.75f), spec);
        }
    }
};

class ServerMonitorHud final : public TextHud {
public:
    ServerMonitorHud()
        : TextHud("server_monitor", "Server monitor",
                  "Ping, estimated TPS, packet loss and stability.",
                  {0.99f, 0.2f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showPing", "Ping", true);
        settings.toggle("showTps", "Estimated TPS", true);
        settings.toggle("showLoss", "Packet loss", true);
        settings.toggle("showStability", "Stability", true);

        addKeywords({"tps", "server", "network", "lag", "paquets"});
    }

    bool relevantNow() const override { return sdk::game().world().multiplayer; }

    std::vector<Row> rows() override {
        const auto& world = sdk::game().world();
        const auto& active = theme();

        std::vector<Row> result;

        if (settings.value<bool>("showPing", true)) {
            result.push_back(Row{"Ping",
                                 world.ping >= 0.f ? std::format("{} ms",
                                                                 static_cast<int>(world.ping))
                                                   : kUnknown,
                                 {}});
        }

        if (settings.value<bool>("showTps", true)) {
            if (world.tps < 0.f) {
                result.push_back(Row{"TPS", kUnknown, {}});
            } else {
                const Color color = world.tps >= 19.f   ? active.success
                                    : world.tps >= 15.f ? active.warning
                                                        : active.danger;
                result.push_back(Row{"TPS", strings::formatFloat(world.tps, 1), color});
            }
        }

        if (settings.value<bool>("showLoss", true)) {
            result.push_back(Row{"Loss",
                                 std::format("{} %", strings::formatFloat(world.packetLoss * 100.f, 1)),
                                 world.packetLoss > 0.02f ? active.danger : Color{}});
        }

        if (settings.value<bool>("showStability", true)) {
            const char* label = world.ping < 0.f            ? kUnknown
                                : world.packetLoss > 0.05f  ? "Unstable"
                                : world.ping > 200.f        ? "Average"
                                                            : "Good";
            result.push_back(Row{"Connection", label, {}});
        }

        return result;
    }
};

class ArmourHud final : public TextHud {
public:
    ArmourHud()
        : TextHud("armour", "Armour", "Armour points and health.",
                  {0.5f, 0.9f}, HudAnchor::BottomCenter) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showHealth", "Health", true);
        settings.toggle("showArmour", "Armour points", true);
        settings.toggle("showHunger", "Food", false);

        addKeywords({"armour", "armor", "health"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        const auto& player = sdk::game().player();
        const auto& active = theme();

        if (!player.valid) return {Row{"Health", kUnknown, {}}};

        std::vector<Row> result;

        if (settings.value<bool>("showHealth", true)) {
            const float ratio = player.maxHealth > 0.f ? player.health / player.maxHealth : 1.f;
            const Color color = ratio < 0.3f ? active.danger
                                : ratio < 0.6f ? active.warning
                                               : Color{};
            result.push_back(Row{"Health",
                                 std::format("{:.0f} / {:.0f}", player.health, player.maxHealth),
                                 color});
        }

        if (settings.value<bool>("showArmour", true)) {
            result.push_back(Row{"Armour", std::to_string(player.armourPoints), {}});
        }

        if (settings.value<bool>("showHunger", false)) {
            result.push_back(Row{"Hunger", std::format("{:.0f}", player.hunger), {}});
        }

        return result;
    }
};

}

void registerHudModules(ModuleManager& manager) {
    manager.add<FpsHud>();
    manager.add<CpsHud>();
    manager.add<ClockHud>();
    manager.add<CoordinatesHud>();
    manager.add<DirectionHud>();
    manager.add<SpeedHud>();
    manager.add<PingHud>();
    manager.add<MemoryHud>();
    manager.add<IpDisplayHud>();
    manager.add<AfkTimerHud>();
    manager.add<SessionStatsHud>();
    manager.add<StopwatchHud>();
    manager.add<KeystrokesHud>();
    manager.add<FpsGraphHud>();
    manager.add<ServerMonitorHud>();
    manager.add<ArmourHud>();
}

}
