#include "ClientModules.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

#include "core/Lang.hpp"
#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/feature/Clips.hpp"
#include "dll/feature/Playtime.hpp"
#include "dll/feature/Screenshot.hpp"
#include "dll/feature/Services.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/render/ColorMatrix.hpp"
#include "dll/modules/hud/TextHud.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

class PrivacyMode final : public Module {
public:
    PrivacyMode()
        : Module("privacy_mode", "Privacy mode", ModuleCategory::Utility,
                 "Hides what identifies you on screen.") {
        settings.toggle("hideServer", "Hide the server address", true);
        settings.toggle("hideName", "Hide the username", true);
        settings.toggle("hideCoordinates", "Hide the coordinates", false);

        for (const char* id : {"hideServer", "hideName", "hideCoordinates"}) {
            settings.find(id)->onChange = [this] { apply(); };
        }

        addKeywords({"privacy", "hide", "anonymous"});
    }

    void onEnable() override { apply(); }

    void onDisable() override {
        Privacy& privacy = Privacy::get();
        privacy.hideServerAddress = false;
        privacy.hidePlayerName = false;
        privacy.hideCoordinates = false;
    }

private:
    void apply() {
        if (!enabled()) return;

        Privacy& privacy = Privacy::get();
        privacy.hideServerAddress = settings.value<bool>("hideServer", true);
        privacy.hidePlayerName = settings.value<bool>("hideName", true);
        privacy.hideCoordinates = settings.value<bool>("hideCoordinates", false);
    }
};

class StreamerMode final : public Module {
public:
    StreamerMode()
        : Module("streamer_mode", "Streamer mode", ModuleCategory::Utility,
                 "Hides everything personal and filters the chat during a stream.") {
        settings.toggle("hideEverything", "Hide server, username and coordinates", true);
        settings.toggle("filterChat", "Filter the chat", true);
        settings.toggle("censorInvites", "Censor addresses and invites", true);
        settings.toggle("quietNotifications", "Quiet notifications", true);
        settings.text("extraWords", "Words to censor",
                      "Comma separated.");

        on(&StreamerMode::onChat, EventPriority::High);
        addKeywords({"stream", "streamer", "live", "twitch", "obs"});
    }

    void onEnable() override {
        Privacy& privacy = Privacy::get();
        const bool everything = settings.value<bool>("hideEverything", true);

        privacy.hideServerAddress = everything;
        privacy.hidePlayerName = everything;
        privacy.hideCoordinates = everything;
        privacy.hideChat = settings.value<bool>("filterChat", true);

        Notifications::info("Streamer mode on", "Personal details are hidden.");
    }

    void onDisable() override {
        Privacy& privacy = Privacy::get();
        privacy.hideServerAddress = false;
        privacy.hidePlayerName = false;
        privacy.hideCoordinates = false;
        privacy.hideChat = false;
    }

private:
    void onChat(ChatReceiveEvent& event) {
        if (!settings.value<bool>("filterChat", true)) return;

        if (settings.value<bool>("censorInvites", true) && looksLikeAddress(event.message)) {
            event.message = "[address hidden]";
            return;
        }

        for (const std::string& word :
             strings::split(settings.value<std::string>("extraWords", ""), ',')) {
            const auto trimmed = std::string(strings::trim(word));
            if (trimmed.empty()) continue;
            if (strings::containsInsensitive(event.message, trimmed)) {
                event.cancel();
                return;
            }
        }
    }

    static bool looksLikeAddress(const std::string& text) {
        int digits = 0;
        int dots = 0;

        for (const char c : text) {
            if (std::isdigit(static_cast<unsigned char>(c))) ++digits;
            if (c == '.') ++dots;
        }

        if (dots >= 3 && digits >= 4) return true;
        return strings::containsInsensitive(text, "discord.gg") ||
               strings::containsInsensitive(text, "http://") ||
               strings::containsInsensitive(text, "https://");
    }
};

class PerformanceMode final : public Module {
public:
    PerformanceMode()
        : Module("performance_mode", "Performance mode", ModuleCategory::Utility,
                 "Cuts the expensive effects on its own when the framerate drops.") {
        settings.dropdown("trigger", "Trigger", "Automatic",
                          {"Automatic", "Always on"});
        settings.intSlider("dropBelow", "Enable below", 55, 20, 240, "", " FPS");
        settings.intSlider("restoreAbove", "Restore above", 75, 25, 300, "", " FPS");
        settings.toggle("disableBlur", "Disable blur", true);
        settings.toggle("disableShadows", "Disable shadows", true);
        settings.toggle("reduceAnimations", "Reduce animation", false);
        settings.toggle("notify", "Warn when it switches", true);

        const auto automatic = [this] {
            return settings.value<std::string>("trigger", "Automatic") == "Automatic";
        };
        settings.find("dropBelow")->visibleWhen = automatic;
        settings.find("restoreAbove")->visibleWhen = automatic;

        on(&PerformanceMode::onFrame);
        addKeywords({"performance", "fps", "optimisation", "effects"});
    }

    void onEnable() override { engaged_ = false; }

    void onDisable() override {
        if (engaged_) restore();
    }

private:
    void onFrame(FrameEvent& event) {
        const bool always =
            settings.value<std::string>("trigger", "Automatic") == "Always on";
        const float fps = Velyx::get().fps();

        bool wanted = engaged_;
        if (always) {
            wanted = true;
        } else if (!engaged_ && fps > 0.f &&
                   fps < static_cast<float>(settings.value<int>("dropBelow", 55))) {
            wanted = true;
        } else if (engaged_ && fps > static_cast<float>(settings.value<int>("restoreAbove", 75))) {
            wanted = false;
        }

        if (wanted == engaged_) return;

        if (wanted) {
            engage();
        } else {
            restore();
        }
    }

    void engage() {
        Theme& active = ThemeManager::get().mutableCurrent();

        savedBlur_ = active.blur;
        savedShadows_ = active.shadows;
        savedAnimation_ = active.animationSpeed;

        if (settings.value<bool>("disableBlur", true)) active.blur = false;
        if (settings.value<bool>("disableShadows", true)) active.shadows = false;
        if (settings.value<bool>("reduceAnimations", false)) active.animationSpeed = 0.f;

        Velyx::get().renderer().setEffectsEnabled(active.blur);

        engaged_ = true;

        if (settings.value<bool>("notify", true)) {
            Notifications::warning("Performance mode", "Effects cut back to win framerate.");
        }
    }

    void restore() {
        Theme& active = ThemeManager::get().mutableCurrent();

        active.blur = savedBlur_;
        active.shadows = savedShadows_;
        active.animationSpeed = savedAnimation_;

        Velyx::get().renderer().setEffectsEnabled(active.blur);

        engaged_ = false;

        if (settings.value<bool>("notify", true)) {
            Notifications::success("Performance mode", "Effects restored.");
        }
    }

    bool engaged_ = false;
    bool savedBlur_ = true;
    bool savedShadows_ = true;
    float savedAnimation_ = 1.f;
};

class BatteryMode final : public Module {
public:
    BatteryMode()
        : Module("battery_mode", "Battery mode", ModuleCategory::Utility,
                 "Caps the framerate and the effects when the laptop is unplugged.") {
        settings.toggle("autoDetect", "Detect mains power automatically", true);
        settings.intSlider("limit", "Framerate cap", 60, 20, 144, "", " FPS");
        settings.toggle("cutEffects", "Cut blur and shadows", true);
        settings.toggle("notify", "Warn when the power source changes", true);

        on(&BatteryMode::onFrame);
        addKeywords({"battery", "laptop", "saving", "battery life"});
    }

    void onDisable() override { throttling_ = false; }

private:
    void onFrame(FrameEvent& event) {
        const bool onBattery = settings.value<bool>("autoDetect", true) ? runningOnBattery() : true;

        if (onBattery != throttling_) {
            throttling_ = onBattery;

            if (settings.value<bool>("cutEffects", true)) {
                Theme& active = ThemeManager::get().mutableCurrent();
                if (onBattery) {
                    savedBlur_ = active.blur;
                    savedShadows_ = active.shadows;
                    active.blur = false;
                    active.shadows = false;
                } else {
                    active.blur = savedBlur_;
                    active.shadows = savedShadows_;
                }
                Velyx::get().renderer().setEffectsEnabled(active.blur);
            }

            if (settings.value<bool>("notify", true)) {
                Notifications::info(onBattery ? "On battery" : "On mains",
                                    onBattery ? "Framerate capped to save battery."
                                              : "Cap lifted.");
            }
        }

        if (!throttling_) return;

        const auto limit = static_cast<double>(settings.value<int>("limit", 60));
        if (limit <= 0.0) return;

        const double targetMs = 1000.0 / limit;
        const double elapsedMs = static_cast<double>(event.deltaSeconds) * 1000.0;

        if (elapsedMs < targetMs) {
            const auto sleepMs = static_cast<DWORD>(targetMs - elapsedMs);
            if (sleepMs > 0) Sleep(sleepMs);
        }
    }

    static bool runningOnBattery() {
        SYSTEM_POWER_STATUS status{};
        if (!GetSystemPowerStatus(&status)) return false;
        return status.ACLineStatus == 0;
    }

    bool throttling_ = false;
    bool savedBlur_ = true;
    bool savedShadows_ = true;
};

class AccessibilityMode final : public Module {
public:
    AccessibilityMode()
        : Module("accessibility", "Accessibility mode", ModuleCategory::Utility,
                 "Larger text, stronger outlines, less animation.") {
        settings.toggle("applyTheme", "Switch to the Contrast theme", true);
        settings.slider("textScale", "Text scale", 1.2f, 1.f, 2.f, "", "x");
        settings.toggle("stopAnimations", "Remove the animation", true);
        settings.toggle("thickBorders", "Thick borders", true);
        settings.toggle("noBlur", "Remove the blur", true);

        for (const char* id : {"textScale", "stopAnimations", "thickBorders", "noBlur"}) {
            settings.find(id)->onChange = [this] { apply(); };
        }

        addKeywords({"accessibility", "readable", "contrast", "colour blind"});
    }

    void onEnable() override {
        const Theme& active = theme();
        savedTheme_ = active.name;
        savedScale_ = active.fontScale;
        savedAnimation_ = active.animationSpeed;
        savedBorder_ = active.borderWidth;
        savedBlur_ = active.blur;

        if (settings.value<bool>("applyTheme", true)) ThemeManager::get().apply("Contrast");
        apply();

        Notifications::info("Accessibility mode on");
    }

    void onDisable() override {
        if (settings.value<bool>("applyTheme", true) && !savedTheme_.empty()) {
            ThemeManager::get().apply(savedTheme_);
            return;
        }

        Theme& active = ThemeManager::get().mutableCurrent();
        active.fontScale = savedScale_;
        active.animationSpeed = savedAnimation_;
        active.borderWidth = savedBorder_;
        active.blur = savedBlur_;
    }

private:
    void apply() {
        if (!enabled()) return;

        Theme& active = ThemeManager::get().mutableCurrent();
        active.fontScale = settings.value<float>("textScale", 1.2f);
        if (settings.value<bool>("stopAnimations", true)) active.animationSpeed = 0.f;
        if (settings.value<bool>("thickBorders", true)) active.borderWidth = 2.f;
        if (settings.value<bool>("noBlur", true)) active.blur = false;

        Velyx::get().renderer().setEffectsEnabled(active.blur);
    }

    std::string savedTheme_;
    float savedScale_ = 1.f;
    float savedAnimation_ = 1.f;
    float savedBorder_ = 1.f;
    bool savedBlur_ = true;
};

class ScreenFilters final : public Module {
public:
    ScreenFilters()
        : Module("screen_filters", "Screen filters", ModuleCategory::Render,
                 "Night filter, contrast, saturation and colour blindness aids.") {
        settings.slider("nightShift", "Night filter", 0.f, 0.f, 1.f);
        settings.slider("saturation", "Saturation", 1.f, 0.f, 2.f, "", "x");
        settings.slider("contrast", "Contrast", 1.f, 0.5f, 1.8f, "", "x");
        settings.dropdown("colourBlind", "Colour blindness aid", "None",
                          {"None", "Protanopia", "Deuteranopia", "Tritanopia"});
        settings.toggle("skipMenus", "Leave the client's own menus unfiltered", true);

        on(&ScreenFilters::onRender, EventPriority::Low);
        addKeywords({"filter", "night", "saturation", "contrast", "colour blind", "gamma"});
    }

private:
    void onRender(RenderEvent& event) {
        const float night = settings.value<float>("nightShift", 0.f);
        const float saturation = settings.value<float>("saturation", 1.f);
        const float contrast = settings.value<float>("contrast", 1.f);
        const std::string colourBlind = settings.value<std::string>("colourBlind", "None");

        const bool neutral = night <= 0.001f && std::abs(saturation - 1.f) <= 0.001f &&
                             std::abs(contrast - 1.f) <= 0.001f && colourBlind == "None";
        if (neutral) return;
        if (event.guiOpen && settings.value<bool>("skipMenus", true)) return;

        ColorMatrix matrix = kIdentityMatrix;
        if (std::abs(saturation - 1.f) > 0.001f) matrix = multiply(matrix, saturationMatrix(saturation));
        if (std::abs(contrast - 1.f) > 0.001f) matrix = multiply(matrix, contrastMatrix(contrast));
        if (night > 0.001f) matrix = multiply(matrix, warmthMatrix(night));
        if (colourBlind != "None") matrix = multiply(matrix, colourBlindMatrix(colourBlind));

        event.renderer->colorMatrix(
            Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y), matrix.data());
    }
};

class ScreenshotMode final : public Module {
public:
    ScreenshotMode()
        : Module("screenshot_mode", "Capture mode", ModuleCategory::Utility,
                 "Hides the marked HUD, captures the screen and files the image by server and date.") {
        markEssential();
        mutablePermissions().files = true;

        settings.keybind("captureKey", "Capture key", Keybind{VK_F2, false, false, false,
                                                                    Keybind::Mode::Once});
        settings.toggle("hideHud", "Hide the marked elements", true);
        settings.toggle("hideClientUi", "Hide the menu and the notifications", true);
        settings.toggle("notify", "Confirm with a notification", true);
        settings.toggle("openFolder", "Open the folder after a capture", false);

        always(&ScreenshotMode::onKey);
        always(&ScreenshotMode::onCapture, EventPriority::Last);

        addKeywords({"capture", "screenshot", "photo", "f2"});
    }

private:
    void onKey(KeyEvent& event) {
        if (!event.down || event.repeat) return;

        const Keybind bind = settings.value<Keybind>("captureKey", Keybind{});
        if (!bind.bound() || event.key != bind.key) return;
        if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) return;

        request();
    }

    void request() {
        if (pending_ > 0) return;

        pending_ = settings.value<bool>("hideHud", true) ? 3 : 1;
        if (settings.value<bool>("hideHud", true)) Velyx::get().setScreenshotMode(true);
    }

    void onCapture(RenderTopEvent& event) {
        if (pending_ <= 0) return;

        --pending_;
        if (pending_ > 0) return;

        const auto& world = sdk::game().world();
        const std::string server =
            world.serverAddress.empty() ? world.worldName : world.serverAddress;

        const auto result = screenshot::capture(screenshot::suggestedPath(server));

        Velyx::get().setScreenshotMode(false);

        if (!settings.value<bool>("notify", true)) return;

        if (result.ok) {
            Notifications::success("Screenshot saved",
                                   result.path.filename().string());
            if (settings.value<bool>("openFolder", false)) {
                screenshot::revealInExplorer(result.path);
            }
        } else {
            Notifications::error("Capture failed", result.error);
        }
    }

    int pending_ = 0;
};

class Benchmark final : public Module {
public:
    Benchmark()
        : Module("benchmark", "Benchmark", ModuleCategory::Utility,
                 "Measures performance for a few seconds and suggests settings to match.") {
        settings.intSlider("duration", "Measurement length", 20, 5, 120, "", " s");
        settings.toggle("applySuggestion", "Apply the suggestion automatically", false);

        on(&Benchmark::onFrame);
        on(&Benchmark::onRender);

        addKeywords({"benchmark", "test", "measure", "performance"});
    }

    void onEnable() override {
        FrameStats::get().reset();
        elapsed_ = 0.f;
        finished_ = false;
    }

private:
    void onFrame(FrameEvent& event) {
        if (finished_) return;

        elapsed_ += event.deltaSeconds;
        if (elapsed_ < static_cast<float>(settings.value<int>("duration", 20))) return;

        finished_ = true;
        report();
        setEnabled(false);
    }

    void report() {
        const FrameStats& stats = FrameStats::get();

        const float average = stats.average();
        const float low = stats.onePercentLow();

        std::string verdict;
        if (average >= 200.f && low >= 120.f) {
            verdict = "A comfortable machine: every effect can stay on.";
        } else if (average >= 120.f && low >= 60.f) {
            verdict = "A good balance: keep the blur, watch how many HUD elements you run.";
        } else if (average >= 60.f) {
            verdict = "Tight: cut the blur and the shadows with performance mode.";
            applyIfAsked(false, true);
        } else {
            verdict = "Tight: performance mode is worth leaving on.";
            applyIfAsked(false, false);
        }

        Notifications::push(NotificationKind::Info,
                            std::format("Benchmark: {} FPS on average, {} at 1%",
                                        static_cast<int>(average), static_cast<int>(low)),
                            verdict, 12.f);
    }

    void applyIfAsked(bool blur, bool shadows) {
        if (!settings.value<bool>("applySuggestion", false)) return;

        Theme& active = ThemeManager::get().mutableCurrent();
        active.blur = blur;
        active.shadows = shadows;
        Velyx::get().renderer().setEffectsEnabled(blur);
    }

    void onRender(RenderTopEvent& event) {
        if (finished_) return;

        const auto& active = theme();
        Renderer& renderer = *event.renderer;

        const float duration = static_cast<float>(settings.value<int>("duration", 20));
        const float progress = duration > 0.f ? clamp(elapsed_ / duration, 0.f, 1.f) : 0.f;

        const Rect card = Rect::fromSize(event.screenSize.x * 0.5f - 150.f, 24.f, 300.f, 62.f);

        renderer.fillRounded(card, active.background.withAlpha(0.95f), active.panelRadius);
        renderer.strokeRounded(card, active.border, active.panelRadius, active.borderWidth);

        FontSpec spec;
        spec.family = active.fontFamily;
        spec.size = 13.f;
        spec.weight = FontWeight::SemiBold;
        spec.valign = TextVAlign::Middle;

        renderer.text(std::format("Benchmark  {} s", static_cast<int>(duration - elapsed_) + 1),
                      Rect{card.left + 16.f, card.top + 6.f, card.right - 16.f, card.top + 30.f},
                      active.text, spec);

        const Rect track{card.left + 16.f, card.bottom - 22.f, card.right - 16.f, card.bottom - 16.f};
        renderer.fillRounded(track, active.surface, 3.f);
        renderer.fillGradient(Rect{track.left, track.top,
                                   lerp(track.left, track.right, progress), track.bottom},
                              active.liveAccentDeep(), active.liveAccent(), 0.f, 3.f);
    }

    float elapsed_ = 0.f;
    bool finished_ = false;
};

class PlaytimeHud final : public TextHud {
public:
    PlaytimeHud()
        : TextHud("playtime", "Playtime", "Today, this week and all time.",
                  {0.99f, 0.6f}, HudAnchor::MiddleRight) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showToday", "Today", true);
        settings.toggle("showWeek", "This week", true);
        settings.toggle("showTotal", "Total", false);
        settings.toggle("includeCurrent", "Include the current session", true);

        addKeywords({"time", "playtime", "hours", "stats"});
    }

    std::vector<Row> rows() override {
        const Playtime& tracker = Playtime::get();
        const long long live =
            settings.value<bool>("includeCurrent", true) ? SessionStats::get().secondsPlayed() : 0;

        std::vector<Row> result;

        if (settings.value<bool>("showToday", true)) {
            result.push_back(Row{"Today", strings::formatDuration(tracker.today() + live), {}});
        }
        if (settings.value<bool>("showWeek", true)) {
            result.push_back(Row{"7 days", strings::formatDuration(tracker.thisWeek() + live), {}});
        }
        if (settings.value<bool>("showTotal", false)) {
            result.push_back(Row{"Total", strings::formatDuration(tracker.total() + live), {}});
        }

        return result;
    }
};

class CustomHitColor final : public Module {
public:
    CustomHitColor()
        : Module("custom_hit_color", "Hit colour", ModuleCategory::Render,
                 "Tints a player you hit. Purely visual, client side.") {
        settings.color("color", "Colour", Color::rgb8(61, 220, 132, 130));
        settings.toggle("useThemeAccent", "Use the theme's accent", false);
        settings.slider("intensity", "Strength", 0.6f, 0.f, 1.f);
        settings.toggle("selfOnly", "Only on your own hits", false);

        settings.find("color")->visibleWhen = [this] {
            return !settings.value<bool>("useThemeAccent", false);
        };

        on(&CustomHitColor::onActorHurt);
        addKeywords({"colour", "hit", "hurt"});
    }

private:
    void onActorHurt(ActorHurtEvent& event) {
        const Color base = settings.value<bool>("useThemeAccent", false)
                               ? theme().liveAccent()
                               : settings.value<Color>("color", palette::kMint);

        event.tint = base.withAlpha(base.a * settings.value<float>("intensity", 0.6f));
    }
};

class CustomDamageTint final : public Module {
public:
    CustomDamageTint()
        : Module("damage_tint", "Damage tint", ModuleCategory::Render,
                 "Replaces the red overlay when you take damage.") {
        settings.color("color", "Colour", Color::rgb8(232, 96, 82, 90));
        settings.slider("intensity", "Strength", 0.5f, 0.f, 1.f);
        settings.toggle("scaleWithDamage", "Scale with damage", true);
        settings.toggle("disable", "Remove the overlay entirely", false);

        const auto visible = [this] { return !settings.value<bool>("disable", false); };
        settings.find("color")->visibleWhen = visible;
        settings.find("intensity")->visibleWhen = visible;
        settings.find("scaleWithDamage")->visibleWhen = visible;

        on(&CustomDamageTint::onHurt);
        addKeywords({"damage", "red", "tint"});
    }

private:
    void onHurt(HurtEvent& event) {
        if (settings.value<bool>("disable", false)) {
            event.tint = Color{0.f, 0.f, 0.f, 0.f};
            return;
        }

        const Color base = settings.value<Color>("color", palette::kEmber);
        float intensity = settings.value<float>("intensity", 0.5f);

        if (settings.value<bool>("scaleWithDamage", true)) {
            intensity *= clamp(event.damage / 10.f, 0.25f, 1.5f);
        }

        event.tint = base.withAlpha(clamp(base.a * intensity, 0.f, 1.f));
    }
};


class ClipMarkers final : public Module {
public:
    ClipMarkers()
        : Module("clip_markers", "Markers", ModuleCategory::Utility,
                 "Drops a timestamped marker to find a moment in a recording.") {
        mutablePermissions().files = true;

        settings.keybind("markKey", "Drop a marker",
                         Keybind{VK_F8, false, false, false, Keybind::Mode::Once});
        settings.toggle("notify", "Confirm with a notification", true);
        settings.toggle("countInSession", "Show the session total", true);

        always(&ClipMarkers::onKey);
        addKeywords({"marker", "clip", "moment"});
    }

    void onEnable() override { mark(); }

private:
    void onKey(KeyEvent& event) {
        if (!event.down || event.repeat) return;

        const Keybind bind = settings.value<Keybind>("markKey", Keybind{});
        if (!bind.bound() || event.key != bind.key) return;
        if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) return;

        mark();
    }

    void mark() {
        const ClipMarker marker = Clips::get().mark();
        ++thisSession_;

        if (!settings.value<bool>("notify", true)) return;

        const std::string body =
            settings.value<bool>("countInSession", true)
                ? std::format("{} since the session started", thisSession_)
                : std::string{};

        Notifications::success(std::format("Marker at {}",
                                           strings::formatDuration(marker.sessionSeconds)),
                               body);
    }

    int thisSession_ = 0;
};

// The one thing a client cannot afford to leave unsaid: which key opens it. Drawn
// while nothing of Velyx is on screen, in the corner the game leaves empty, and it
// gets out of the way once the menu has been opened.
class MenuHint final : public Module {
public:
    MenuHint()
        : Module("menu_hint", "Menu reminder", ModuleCategory::Client,
                 "A corner badge naming the key that opens the client.") {
        settings.toggle("hideAfterFirstOpen", "Hide once the menu has been opened", false);
        settings.slider("hintOpacity", "Opacity", 0.75f, 0.2f, 1.f);

        on(&MenuHint::onRender);
        addKeywords({"hint", "reminder", "key", "menu", "help"});
    }

private:
    void onRender(RenderEvent& event) {
        if (modules().anyInterfaceOpen()) return;
        if (opened_ && settings.value<bool>("hideAfterFirstOpen", false)) return;

        Module* menu = modules().find("clickgui");
        if (menu && menu->enabled()) {
            opened_ = true;
            return;
        }

        Renderer& renderer = *event.renderer;
        const auto& active = theme();
        const float alpha = settings.value<float>("hintOpacity", 0.75f);

        const std::string keys =
            std::string(tr("Open the menu")) + "  " + describeKeybind(config().guiKey) + "   ·   " +
            std::string(tr("Search")) + "  " + describeKeybind(config().searchKey);

        FontSpec spec;
        spec.family = active.fontFamily;
        spec.size = 12.f * active.fontScale;
        spec.weight = FontWeight::Medium;
        spec.valign = TextVAlign::Middle;

        const Vec2 size = renderer.measure(keys, spec);
        const Rect badge = Rect::fromSize(14.f, 14.f, size.x + 58.f, 30.f);

        renderer.fillRounded(badge, active.backgroundDeep.withAlpha(0.55f * alpha), 9.f);
        renderer.strokeRounded(badge, active.border.withAlpha(0.6f * alpha), 9.f, 1.f);

        const Rect mark{badge.left + 6.f, badge.center().y - 10.f, badge.left + 26.f,
                        badge.center().y + 10.f};

        static const std::filesystem::path markFile = Velyx::get().asset("icon.png");
        if (ID2D1Bitmap1* icon = renderer.image(markFile)) {
            renderer.drawImage(icon, mark, alpha);
        }

        renderer.text("VELYX", Rect{mark.right + 8.f, badge.top, mark.right + 52.f, badge.bottom},
                      active.text.withAlpha(alpha), [&] {
                          FontSpec brand = spec;
                          brand.size = 11.f * active.fontScale;
                          brand.weight = FontWeight::Bold;
                          return brand;
                      }());

        renderer.text(keys, Rect{mark.right + 52.f, badge.top, badge.right - 8.f, badge.bottom},
                      active.textMuted.withAlpha(alpha), spec);
    }

    bool opened_ = false;
};

} // namespace

void registerClientModules(ModuleManager& manager) {
    manager.add<PrivacyMode>();
    manager.add<StreamerMode>();
    manager.add<PerformanceMode>();
    manager.add<BatteryMode>();
    manager.add<AccessibilityMode>();
    manager.add<ScreenFilters>();
    manager.add<ScreenshotMode>();
    manager.add<Benchmark>();
    manager.add<PlaytimeHud>();
    manager.add<CustomHitColor>();
    manager.add<CustomDamageTint>();
    manager.add<ClipMarkers>();
    manager.add<MenuHint>();
}

} // namespace velyx
