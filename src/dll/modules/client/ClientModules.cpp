#include "ClientModules.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/feature/Playtime.hpp"
#include "dll/feature/Screenshot.hpp"
#include "dll/feature/Services.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/modules/hud/TextHud.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

using Matrix = std::array<float, 20>;

constexpr Matrix kIdentity{1, 0, 0, 0,
                           0, 1, 0, 0,
                           0, 0, 1, 0,
                           0, 0, 0, 1,
                           0, 0, 0, 0};

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = sum;
        }
    }

    for (int column = 0; column < 4; ++column) {
        float sum = b[16 + column];
        for (int k = 0; k < 4; ++k) sum += a[16 + k] * b[k * 4 + column];
        out[16 + column] = sum;
    }

    return out;
}

Matrix saturationMatrix(float amount) {
    constexpr float lr = 0.2126f;
    constexpr float lg = 0.7152f;
    constexpr float lb = 0.0722f;

    const float inverse = 1.f - amount;

    return Matrix{lr * inverse + amount, lr * inverse,          lr * inverse,          0,
                  lg * inverse,          lg * inverse + amount, lg * inverse,          0,
                  lb * inverse,          lb * inverse,          lb * inverse + amount, 0,
                  0,                     0,                     0,                     1,
                  0,                     0,                     0,                     0};
}

Matrix contrastMatrix(float amount) {
    const float offset = 0.5f * (1.f - amount);

    return Matrix{amount, 0, 0, 0,
                  0, amount, 0, 0,
                  0, 0, amount, 0,
                  0, 0, 0, 1,
                  offset, offset, offset, 0};
}

Matrix warmthMatrix(float amount) {
    const float blue = lerp(1.f, 0.62f, amount);
    const float green = lerp(1.f, 0.88f, amount);

    return Matrix{1, 0, 0, 0,
                  0, green, 0, 0,
                  0, 0, blue, 0,
                  0, 0, 0, 1,
                  0, 0, 0, 0};
}

Matrix colourBlindMatrix(std::string_view kind) {
    if (kind == "Protanopie") {
        return Matrix{0.567f, 0.558f, 0.f,    0,
                      0.433f, 0.442f, 0.242f, 0,
                      0.f,    0.f,    0.758f, 0,
                      0, 0, 0, 1,
                      0, 0, 0, 0};
    }
    if (kind == "Deutéranopie") {
        return Matrix{0.625f, 0.70f, 0.f,   0,
                      0.375f, 0.30f, 0.30f, 0,
                      0.f,    0.f,   0.70f, 0,
                      0, 0, 0, 1,
                      0, 0, 0, 0};
    }
    if (kind == "Tritanopie") {
        return Matrix{0.95f, 0.f,    0.f,    0,
                      0.05f, 0.433f, 0.475f, 0,
                      0.f,   0.567f, 0.525f, 0,
                      0, 0, 0, 1,
                      0, 0, 0, 0};
    }
    return kIdentity;
}

class PrivacyMode final : public Module {
public:
    PrivacyMode()
        : Module("privacy_mode", "Mode confidentialité", ModuleCategory::Utility,
                 "Masque les informations qui vous identifient à l'écran.") {
        settings.toggle("hideServer", "Masquer l'adresse du serveur", true);
        settings.toggle("hideName", "Masquer le pseudo", true);
        settings.toggle("hideCoordinates", "Masquer les coordonnées", false);

        for (const char* id : {"hideServer", "hideName", "hideCoordinates"}) {
            settings.find(id)->onChange = [this] { apply(); };
        }

        addKeywords({"vie privée", "privacy", "masquer", "anonyme"});
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
        : Module("streamer_mode", "Mode streamer", ModuleCategory::Utility,
                 "Masque tout ce qui est personnel et filtre le chat pendant un live.") {
        settings.toggle("hideEverything", "Masquer serveur, pseudo et coordonnées", true);
        settings.toggle("filterChat", "Filtrer le chat", true);
        settings.toggle("censorInvites", "Censurer les adresses et invitations", true);
        settings.toggle("quietNotifications", "Notifications discrètes", true);
        settings.text("extraWords", "Mots à censurer",
                      "Séparés par des virgules.");

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

        Notifications::info("Mode streamer actif", "Les informations personnelles sont masquées.");
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
            event.message = "[adresse masquée]";
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
        : Module("performance_mode", "Mode performance", ModuleCategory::Utility,
                 "Coupe automatiquement les effets coûteux quand le framerate chute.") {
        settings.dropdown("trigger", "Déclenchement", "Automatique",
                          {"Automatique", "Toujours actif"});
        settings.intSlider("dropBelow", "Activer sous", 55, 20, 240, "", " FPS");
        settings.intSlider("restoreAbove", "Rétablir au-dessus de", 75, 25, 300, "", " FPS");
        settings.toggle("disableBlur", "Désactiver le flou", true);
        settings.toggle("disableShadows", "Désactiver les ombres", true);
        settings.toggle("reduceAnimations", "Réduire les animations", false);
        settings.toggle("notify", "Prévenir lors du basculement", true);

        const auto automatic = [this] {
            return settings.value<std::string>("trigger", "Automatique") == "Automatique";
        };
        settings.find("dropBelow")->visibleWhen = automatic;
        settings.find("restoreAbove")->visibleWhen = automatic;

        on(&PerformanceMode::onFrame);
        addKeywords({"performance", "fps", "optimisation", "effets"});
    }

    void onEnable() override { engaged_ = false; }

    void onDisable() override {
        if (engaged_) restore();
    }

private:
    void onFrame(FrameEvent& event) {
        const bool always =
            settings.value<std::string>("trigger", "Automatique") == "Toujours actif";
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
            Notifications::warning("Mode performance", "Effets réduits pour récupérer du framerate.");
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
            Notifications::success("Mode performance", "Effets rétablis.");
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
        : Module("battery_mode", "Mode batterie", ModuleCategory::Utility,
                 "Limite le framerate et les effets quand le portable est débranché.") {
        settings.toggle("autoDetect", "Détecter le secteur automatiquement", true);
        settings.intSlider("limit", "Limite de framerate", 60, 20, 144, "", " FPS");
        settings.toggle("cutEffects", "Couper flou et ombres", true);
        settings.toggle("notify", "Prévenir au changement d'alimentation", true);

        on(&BatteryMode::onFrame);
        addKeywords({"batterie", "portable", "économie", "autonomie"});
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
                Notifications::info(onBattery ? "Sur batterie" : "Sur secteur",
                                    onBattery ? "Framerate limité pour économiser l'autonomie."
                                              : "Limitation levée.");
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
        : Module("accessibility", "Mode accessibilité", ModuleCategory::Utility,
                 "Texte plus grand, contours marqués, animations réduites.") {
        settings.toggle("applyTheme", "Basculer sur le thème Contrast", true);
        settings.slider("textScale", "Échelle du texte", 1.2f, 1.f, 2.f, "", "x");
        settings.toggle("stopAnimations", "Supprimer les animations", true);
        settings.toggle("thickBorders", "Bordures épaisses", true);
        settings.toggle("noBlur", "Supprimer le flou", true);

        for (const char* id : {"textScale", "stopAnimations", "thickBorders", "noBlur"}) {
            settings.find(id)->onChange = [this] { apply(); };
        }

        addKeywords({"accessibilité", "lisible", "contraste", "daltonien"});
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

        Notifications::info("Mode accessibilité actif");
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
        : Module("screen_filters", "Filtres d'écran", ModuleCategory::Render,
                 "Filtre nuit, contraste, saturation et aides pour daltoniens.") {
        settings.slider("nightShift", "Filtre nuit", 0.f, 0.f, 1.f);
        settings.slider("saturation", "Saturation", 1.f, 0.f, 2.f, "", "x");
        settings.slider("contrast", "Contraste", 1.f, 0.5f, 1.8f, "", "x");
        settings.dropdown("colourBlind", "Aide daltonisme", "Aucune",
                          {"Aucune", "Protanopie", "Deutéranopie", "Tritanopie"});
        settings.toggle("skipMenus", "Ne pas filtrer les menus du client", true);

        on(&ScreenFilters::onRender, EventPriority::Low);
        addKeywords({"filtre", "nuit", "saturation", "contraste", "daltonien", "gamma"});
    }

private:
    void onRender(RenderEvent& event) {
        const float night = settings.value<float>("nightShift", 0.f);
        const float saturation = settings.value<float>("saturation", 1.f);
        const float contrast = settings.value<float>("contrast", 1.f);
        const std::string colourBlind = settings.value<std::string>("colourBlind", "Aucune");

        const bool neutral = night <= 0.001f && std::abs(saturation - 1.f) <= 0.001f &&
                             std::abs(contrast - 1.f) <= 0.001f && colourBlind == "Aucune";
        if (neutral) return;
        if (event.guiOpen && settings.value<bool>("skipMenus", true)) return;

        Matrix matrix = kIdentity;
        if (std::abs(saturation - 1.f) > 0.001f) matrix = multiply(matrix, saturationMatrix(saturation));
        if (std::abs(contrast - 1.f) > 0.001f) matrix = multiply(matrix, contrastMatrix(contrast));
        if (night > 0.001f) matrix = multiply(matrix, warmthMatrix(night));
        if (colourBlind != "Aucune") matrix = multiply(matrix, colourBlindMatrix(colourBlind));

        event.renderer->colorMatrix(
            Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y), matrix.data());
    }
};

class ScreenshotMode final : public Module {
public:
    ScreenshotMode()
        : Module("screenshot_mode", "Mode capture", ModuleCategory::Utility,
                 "Masque le HUD marqué, capture l'écran et range l'image par serveur et par date.") {
        markEssential();
        mutablePermissions().files = true;

        settings.keybind("captureKey", "Touche de capture", Keybind{VK_F2, false, false, false,
                                                                    Keybind::Mode::Once});
        settings.toggle("hideHud", "Masquer les éléments marqués", true);
        settings.toggle("hideClientUi", "Masquer le menu et les notifications", true);
        settings.toggle("notify", "Confirmer par une notification", true);
        settings.toggle("openFolder", "Ouvrir le dossier après la capture", false);

        always(&ScreenshotMode::onKey);
        always(&ScreenshotMode::onCapture, EventPriority::Last);

        addKeywords({"capture", "screenshot", "photo", "f2"});
    }

    void onEnable() override { request(); }

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
            Notifications::success("Capture enregistrée",
                                   result.path.filename().string());
            if (settings.value<bool>("openFolder", false)) {
                screenshot::revealInExplorer(result.path);
            }
        } else {
            Notifications::error("Capture impossible", result.error);
        }
    }

    int pending_ = 0;
};

class Benchmark final : public Module {
public:
    Benchmark()
        : Module("benchmark", "Benchmark", ModuleCategory::Utility,
                 "Mesure la performance quelques secondes et propose des réglages adaptés.") {
        settings.intSlider("duration", "Durée de la mesure", 20, 5, 120, "", " s");
        settings.toggle("applySuggestion", "Appliquer automatiquement la suggestion", false);

        on(&Benchmark::onFrame);
        on(&Benchmark::onRender);

        addKeywords({"benchmark", "test", "mesure", "performance"});
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
            verdict = "Machine confortable : tous les effets peuvent rester actifs.";
        } else if (average >= 120.f && low >= 60.f) {
            verdict = "Bon équilibre : gardez le flou, surveillez le nombre d'éléments de HUD.";
        } else if (average >= 60.f) {
            verdict = "Serré : coupez le flou et les ombres via le mode performance.";
            applyIfAsked(false, true);
        } else {
            verdict = "Limité : mode performance conseillé en permanence.";
            applyIfAsked(false, false);
        }

        Notifications::push(NotificationKind::Info,
                            std::format("Benchmark : {} FPS moyens, {} en 1 %",
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
        : TextHud("playtime", "Temps de jeu", "Aujourd'hui, cette semaine et au total.",
                  {0.99f, 0.6f}, HudAnchor::MiddleRight) {
        addTextSettings(true);

        settings.header("Lignes affichées");
        settings.toggle("showToday", "Aujourd'hui", true);
        settings.toggle("showWeek", "Cette semaine", true);
        settings.toggle("showTotal", "Total", false);
        settings.toggle("includeCurrent", "Inclure la session en cours", true);

        addKeywords({"temps", "playtime", "heures", "statistiques"});
    }

    std::vector<Row> rows() override {
        const Playtime& tracker = Playtime::get();
        const long long live =
            settings.value<bool>("includeCurrent", true) ? SessionStats::get().secondsPlayed() : 0;

        std::vector<Row> result;

        if (settings.value<bool>("showToday", true)) {
            result.push_back(Row{"Aujourd'hui", strings::formatDuration(tracker.today() + live), {}});
        }
        if (settings.value<bool>("showWeek", true)) {
            result.push_back(Row{"7 jours", strings::formatDuration(tracker.thisWeek() + live), {}});
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
        : Module("custom_hit_color", "Couleur de coup", ModuleCategory::Render,
                 "Change la couleur d'un joueur touché. Purement visuel, côté client.") {
        settings.color("color", "Couleur", Color::rgb8(61, 220, 132, 130));
        settings.toggle("useThemeAccent", "Utiliser l'accent du thème", false);
        settings.slider("intensity", "Intensité", 0.6f, 0.f, 1.f);
        settings.toggle("selfOnly", "Uniquement sur vos coups", false);

        settings.find("color")->visibleWhen = [this] {
            return !settings.value<bool>("useThemeAccent", false);
        };

        on(&CustomHitColor::onActorHurt);
        addKeywords({"couleur", "coup", "hit", "hurt"});
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
        : Module("damage_tint", "Teinte de dégâts", ModuleCategory::Render,
                 "Remplace le voile rouge quand vous prenez des dégâts.") {
        settings.color("color", "Couleur", Color::rgb8(232, 96, 82, 90));
        settings.slider("intensity", "Intensité", 0.5f, 0.f, 1.f);
        settings.toggle("scaleWithDamage", "Intensité selon les dégâts", true);
        settings.toggle("disable", "Supprimer complètement le voile", false);

        const auto visible = [this] { return !settings.value<bool>("disable", false); };
        settings.find("color")->visibleWhen = visible;
        settings.find("intensity")->visibleWhen = visible;
        settings.find("scaleWithDamage")->visibleWhen = visible;

        on(&CustomDamageTint::onHurt);
        addKeywords({"dégâts", "damage", "rouge", "teinte"});
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
}

} // namespace velyx
