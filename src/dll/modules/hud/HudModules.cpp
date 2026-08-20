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
        : TextHud("fps", "FPS", "Images par seconde, avec seuils de couleur.",
                  {0.01f, 0.02f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Seuils");
        settings.toggle("colorThresholds", "Colorer selon la performance", true);
        settings.intSlider("warnBelow", "Seuil orange", 60, 10, 240, "", " FPS");
        settings.intSlider("badBelow", "Seuil rouge", 30, 5, 120, "", " FPS");
        settings.toggle("showLows", "Afficher les 1% lows", false);

        addKeywords({"fps", "images", "performance", "framerate"});
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
        : TextHud("cps", "CPS", "Clics par seconde, gauche et droite.",
                  {0.01f, 0.08f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Boutons");
        settings.toggle("showLeft", "Clic gauche", true);
        settings.toggle("showRight", "Clic droit", false);
        settings.toggle("combined", "Fusionner sur une ligne", false);
        settings.toggle("showPeak", "Afficher le record de la session", false);

        addKeywords({"cps", "clics", "clicks", "souris"});
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
                result.push_back(Row{"CPS D", std::to_string(tracker.right()), {}});
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
        : TextHud("clock", "Horloge", "Heure système, format configurable.",
                  {0.99f, 0.02f}, HudAnchor::TopRight) {
        addTextSettings(false);

        settings.header("Format");
        settings.dropdown("format", "Affichage", "24 h", {"24 h", "12 h", "Avec secondes"});
        settings.toggle("showDate", "Afficher la date", false);

        addKeywords({"heure", "horloge", "clock", "temps"});
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
        } else if (format == "Avec secondes") {
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
        : TextHud("coordinates", "Coordonnées", "Position, dimension et direction.",
                  {0.01f, 0.97f}, HudAnchor::BottomLeft) {
        addTextSettings(true);

        settings.header("Affichage");
        settings.intSlider("decimals", "Décimales", 0, 0, 3);
        settings.toggle("singleLine", "Sur une seule ligne", true);
        settings.toggle("showNether", "Coordonnées du Nether", false,
                        "Affiche l'équivalent divisé par 8.");
        settings.toggle("showDirection", "Direction", true);
        settings.toggle("showChunk", "Position dans le chunk", false);

        addKeywords({"coords", "coordonnées", "position", "xyz"});
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
            result.push_back(Row{"Face", sdk::Game::compass(player.yaw), {}});
        }

        return result;
    }
};

class DirectionHud final : public TextHud {
public:
    DirectionHud()
        : TextHud("direction", "Direction", "Boussole textuelle.",
                  {0.5f, 0.03f}, HudAnchor::TopCenter) {
        addTextSettings(false);

        settings.header("Affichage");
        settings.toggle("longNames", "Noms complets", false);
        settings.toggle("showDegrees", "Afficher les degrés", false);

        addKeywords({"boussole", "direction", "compass", "nord"});
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
        : TextHud("speed", "Vitesse", "Vitesse horizontale en blocs par seconde.",
                  {0.01f, 0.14f}, HudAnchor::TopLeft) {
        addTextSettings(true);

        settings.header("Unité");
        settings.dropdown("unit", "Unité", "b/s", {"b/s", "km/h", "b/tick"});
        settings.intSlider("decimals", "Décimales", 2, 0, 3);

        addKeywords({"vitesse", "speed", "bps"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        if (!sdk::game().player().valid) return {Row{"Vitesse", kUnknown, {}}};

        const float blocksPerSecond = sdk::game().horizontalSpeed();
        const std::string unit = settings.value<std::string>("unit", "b/s");
        const int decimals = settings.value<int>("decimals", 2);

        float value = blocksPerSecond;
        if (unit == "km/h") {
            value = blocksPerSecond * 3.6f;
        } else if (unit == "b/tick") {
            value = blocksPerSecond / 20.f;
        }

        return {Row{"Vitesse", std::format("{} {}", strings::formatFloat(value, decimals), unit), {}}};
    }
};

class PingHud final : public TextHud {
public:
    PingHud()
        : TextHud("ping", "Ping", "Latence vers le serveur.",
                  {0.99f, 0.08f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Seuils");
        settings.toggle("colorThresholds", "Colorer selon la latence", true);
        settings.intSlider("warnAbove", "Seuil orange", 100, 20, 500, "", " ms");
        settings.intSlider("badAbove", "Seuil rouge", 200, 50, 1000, "", " ms");

        addKeywords({"ping", "latence", "ms", "réseau"});
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
        : TextHud("memory", "Mémoire", "Mémoire utilisée par le jeu.",
                  {0.99f, 0.14f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Affichage");
        settings.dropdown("unit", "Unité", "MiB", {"MiB", "GiB"});

        addKeywords({"ram", "mémoire", "memory"});
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
        : TextHud("ip_display", "Adresse du serveur", "Serveur en cours, masquable pour le stream.",
                  {0.5f, 0.97f}, HudAnchor::BottomCenter) {
        addTextSettings(false);
        addKeywords({"ip", "serveur", "adresse", "server"});
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
        : TextHud("afk_timer", "Minuteur AFK", "Temps écoulé depuis la dernière action.",
                  {0.5f, 0.1f}, HudAnchor::TopCenter) {
        addTextSettings(true);

        settings.header("Déclenchement");
        settings.intSlider("showAfter", "Afficher après", 30, 5, 600, "", " s");

        addKeywords({"afk", "inactif", "idle"});
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
        : TextHud("session_stats", "Stats de session",
                  "Durée, distance parcourue, éliminations et FPS moyen.",
                  {0.99f, 0.4f}, HudAnchor::MiddleRight) {
        addTextSettings(true);

        settings.header("Lignes affichées");
        settings.toggle("showTime", "Durée de jeu", true);
        settings.toggle("showBlocks", "Blocs parcourus", true);
        settings.toggle("showCombat", "Éliminations / morts", true);
        settings.toggle("showFps", "FPS moyen", true);
        settings.toggle("showClicks", "Clics totaux", false);

        addKeywords({"stats", "session", "statistiques"});
    }

    std::vector<Row> rows() override {
        const SessionStats& stats = SessionStats::get();
        std::vector<Row> result;

        if (settings.value<bool>("showTime", true)) {
            result.push_back(Row{"Durée", strings::formatDuration(stats.secondsPlayed()), {}});
        }
        if (settings.value<bool>("showBlocks", true)) {
            result.push_back(Row{"Blocs",
                                 strings::formatThousands(
                                     static_cast<long long>(stats.blocksTravelled())), {}});
        }
        if (settings.value<bool>("showCombat", true)) {
            result.push_back(Row{"K/D", std::format("{} / {}", stats.kills(), stats.deaths()), {}});
        }
        if (settings.value<bool>("showFps", true)) {
            const float average = FrameStats::get().average();
            result.push_back(Row{"FPS moy.",
                                 average > 0.f ? std::to_string(static_cast<int>(average))
                                               : kUnknown, {}});
        }
        if (settings.value<bool>("showClicks", false)) {
            result.push_back(Row{"Clics",
                                 strings::formatThousands(ClickTracker::get().totalClicks()), {}});
        }

        return result;
    }
};

class StopwatchHud final : public TextHud {
public:
    StopwatchHud()
        : TextHud("stopwatch", "Chronomètre", "Chronomètre manuel pour l'entraînement.",
                  {0.5f, 0.15f}, HudAnchor::TopCenter) {
        addTextSettings(false);

        settings.header("Contrôles");
        settings.keybind("toggleKey", "Démarrer / arrêter", Keybind{VK_F7});
        settings.keybind("resetKey", "Réinitialiser", Keybind{VK_F7, false, true});
        settings.toggle("showMilliseconds", "Millisecondes", true);

        always(&StopwatchHud::onKey);
        addKeywords({"chrono", "stopwatch", "timer", "entraînement"});
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
        : HudModule("keystrokes", "Keystrokes", "Affiche les touches de déplacement et les clics.",
                    {0.03f, 0.6f}, HudAnchor::MiddleLeft) {
        settings.header("Disposition");
        settings.slider("keySize", "Taille des touches", 30.f, 16.f, 60.f, "", "px");
        settings.slider("gap", "Espacement", 3.f, 0.f, 12.f, "", "px");
        settings.toggle("showMouse", "Boutons de souris", true);
        settings.toggle("showSpace", "Barre d'espace", true);
        settings.toggle("showCps", "CPS sur les boutons", true);

        settings.header("Couleurs");
        settings.color("idleColor", "Touche relâchée", Color::rgb8(11, 31, 23, 170));
        settings.color("pressedColor", "Touche pressée", palette::kMint);
        settings.toggle("accentPressed", "Utiliser l'accent du thème", true);

        if (Setting* pressed = settings.find("pressedColor")) {
            pressed->visibleWhen = [this] { return !settings.value<bool>("accentPressed", true); };
        }
        if (Setting* cps = settings.find("showCps")) {
            cps->visibleWhen = [this] { return settings.value<bool>("showMouse", true); };
        }

        addKeywords({"touches", "keystrokes", "wasd", "clavier"});
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
        : HudModule("fps_graph", "Graphique FPS",
                    "Frametimes, freezes et 1% lows sur les dernières secondes.",
                    {0.99f, 0.97f}, HudAnchor::BottomRight) {
        settings.header("Graphique");
        settings.slider("width", "Largeur", 220.f, 80.f, 600.f, "", "px");
        settings.slider("height", "Hauteur", 70.f, 30.f, 240.f, "", "px");
        settings.intSlider("samples", "Échantillons", 180, 30, 512);
        settings.toggle("fill", "Remplir sous la courbe", true);
        settings.toggle("showTargetLine", "Ligne de référence", true);
        settings.intSlider("target", "FPS cible", 60, 30, 360, "", " FPS");
        settings.toggle("showLegend", "Légende chiffrée", true);

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
            renderer.text("Mesure en cours…", plot, textColor().fade(0.6f), spec);
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
        : TextHud("server_monitor", "Moniteur serveur",
                  "Ping, TPS estimé, pertes de paquets et stabilité.",
                  {0.99f, 0.2f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Lignes affichées");
        settings.toggle("showPing", "Ping", true);
        settings.toggle("showTps", "TPS estimé", true);
        settings.toggle("showLoss", "Pertes de paquets", true);
        settings.toggle("showStability", "Stabilité", true);

        addKeywords({"tps", "serveur", "réseau", "lag", "paquets"});
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
            result.push_back(Row{"Pertes",
                                 std::format("{} %", strings::formatFloat(world.packetLoss * 100.f, 1)),
                                 world.packetLoss > 0.02f ? active.danger : Color{}});
        }

        if (settings.value<bool>("showStability", true)) {
            const char* label = world.ping < 0.f            ? kUnknown
                                : world.packetLoss > 0.05f  ? "Instable"
                                : world.ping > 200.f        ? "Moyenne"
                                                            : "Bonne";
            result.push_back(Row{"Connexion", label, {}});
        }

        return result;
    }
};

class ArmourHud final : public TextHud {
public:
    ArmourHud()
        : TextHud("armour", "Armure", "Points d'armure et points de vie.",
                  {0.5f, 0.9f}, HudAnchor::BottomCenter) {
        addTextSettings(true);

        settings.header("Lignes affichées");
        settings.toggle("showHealth", "Points de vie", true);
        settings.toggle("showArmour", "Points d'armure", true);
        settings.toggle("showHunger", "Nourriture", false);

        addKeywords({"armure", "armor", "vie", "santé"});
    }

    bool relevantNow() const override { return sdk::game().player().valid; }

    std::vector<Row> rows() override {
        const auto& player = sdk::game().player();
        const auto& active = theme();

        if (!player.valid) return {Row{"Vie", kUnknown, {}}};

        std::vector<Row> result;

        if (settings.value<bool>("showHealth", true)) {
            const float ratio = player.maxHealth > 0.f ? player.health / player.maxHealth : 1.f;
            const Color color = ratio < 0.3f ? active.danger
                                : ratio < 0.6f ? active.warning
                                               : Color{};
            result.push_back(Row{"Vie",
                                 std::format("{:.0f} / {:.0f}", player.health, player.maxHealth),
                                 color});
        }

        if (settings.value<bool>("showArmour", true)) {
            result.push_back(Row{"Armure", std::to_string(player.armourPoints), {}});
        }

        if (settings.value<bool>("showHunger", false)) {
            result.push_back(Row{"Faim", std::format("{:.0f}", player.hunger), {}});
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
