#include "Onboarding.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <format>

#include "dll/Velyx.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/config/ProfileManager.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/ui/Theme.hpp"
#include "dll/ui/Ui.hpp"

namespace velyx {
namespace {

constexpr int kStepCount = 5;
constexpr float kCardWidth = 620.f;
constexpr float kCardHeight = 420.f;

struct Preset {
    const char* id;
    const char* title;
    const char* description;
    const char* profile;
    const char* theme;
};

constexpr std::array<Preset, 3> kPresets{{
    {"pvp", "PvP", "A compact HUD: crosshair, latency and framerate up front.", "PvP",
     "Night"},
    {"survival", "Survival", "Coordinates, compass, armour and session stats.", "Survival",
     "Velyx"},
    {"stream", "Streaming", "Personal details hidden, screenshots filed away.", "Global",
     "Velyx"},
}};

const char* const kPvpModules[] = {"fps", "cps", "ping", "keystrokes", "crosshair",
                                   "performance_mode"};
const char* const kSurvivalModules[] = {"coordinates", "direction", "clock", "armour",
                                        "session_stats"};
const char* const kStreamModules[] = {"streamer_mode", "clock", "fps", "screenshot_mode"};

} // namespace

Onboarding::Onboarding()
    : Module("onboarding", "Setup assistant", ModuleCategory::Client,
             "A guided set-up on the first run.") {
    markInterfaceModule();

    on(&Onboarding::onRender);
    on(&Onboarding::onMouse, EventPriority::First);
    on(&Onboarding::onKey, EventPriority::First);

    addKeywords({"assistant", "onboarding", "startup", "first run"});
}

void Onboarding::onEnable() {
    step_ = 0;
    preset_.clear();
    finishRequested_.store(false, std::memory_order_release);
    appear_.set(0.f);
    appear_.to(1.f);
    WindowHook::setCaptureInput(true);
}

void Onboarding::onDisable() {
    if (!modules().anyInterfaceOpen()) WindowHook::setCaptureInput(false);
}

void Onboarding::onMouse(MouseEvent& event) {
    ui().feedMouse(event);
    event.cancel();
}

void Onboarding::onKey(KeyEvent& event) {
    // finish() disables the module, which unsubscribes these very handlers and
    // writes the configuration to disk. Both belong on the render thread, where
    // the rest of the assistant already runs.
    if (event.down && event.key == VK_ESCAPE) finishRequested_.store(true, std::memory_order_release);
    ui().feedKey(event);
    event.cancel();
}

void Onboarding::applyPreset(const std::string& preset) {
    preset_ = preset;

    const auto enable = [](const char* const* ids, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (Module* module = modules().find(ids[i])) module->setEnabled(true, false);
        }
    };

    if (preset == "pvp") {
        enable(kPvpModules, std::size(kPvpModules));
    } else if (preset == "survival") {
        enable(kSurvivalModules, std::size(kSurvivalModules));
    } else {
        enable(kStreamModules, std::size(kStreamModules));
    }

    for (const Preset& entry : kPresets) {
        if (preset != entry.id) continue;

        ThemeManager::get().apply(entry.theme);
        config().theme = entry.theme;

        if (profiles().exists(entry.profile)) {
            profiles().switchTo(entry.profile);
            config().activeProfile = entry.profile;
        }
    }
}

void Onboarding::finish() {
    config().onboardingCompleted = true;
    config().save();

    profiles().saveCurrent();
    setEnabled(false);
}

void Onboarding::drawWelcome(const Rect& body) {
    Ui& gui = ui();
    const auto& active = theme();

    static const std::filesystem::path markFile = Velyx::get().asset("icon.png");

    const Rect mark{body.left, body.top + 2.f, body.left + 34.f, body.top + 36.f};
    float titleLeft = body.left;

    if (ID2D1Bitmap1* icon = gui.renderer().image(markFile)) {
        gui.renderer().drawImageRounded(icon, mark, 9.f);
        titleLeft = mark.right + 14.f;
    }

    gui.text("Welcome to Velyx",
             Rect{titleLeft, body.top, body.right, body.top + 40.f}, active.text, 26.f,
             FontWeight::Bold);

    gui.text("A few questions to land on something usable straight away. "
             "Everything stays changeable from the menu afterwards.",
             Rect{body.left, body.top + 44.f, body.right, body.top + 100.f}, active.textMuted,
             13.5f);

    const std::array<std::pair<const char*, const char*>, 3> points{{
        {"One menu", "Modules, settings, themes and profiles in one place."},
        {"Profiles", "Modules, HUD and theme change together, in one click."},
        {"Nothing automated", "No module plays for you."},
    }};

    float y = body.top + 118.f;
    for (const auto& [title, detail] : points) {
        gui.renderer().fillCircle({body.left + 5.f, y + 12.f}, 3.f, active.liveAccent());
        gui.text(title, Rect{body.left + 20.f, y, body.right, y + 22.f}, active.text, 14.f,
                 FontWeight::SemiBold);
        gui.text(detail, Rect{body.left + 20.f, y + 20.f, body.right, y + 40.f}, active.textMuted,
                 12.f);
        y += 52.f;
    }
}

void Onboarding::drawStyle(const Rect& body) {
    Ui& gui = ui();
    const auto& active = theme();

    gui.text("Pick a theme", Rect{body.left, body.top, body.right, body.top + 34.f},
             active.text, 22.f, FontWeight::Bold);
    gui.text("Colours, corners and animation are yours to tune later on.",
             Rect{body.left, body.top + 34.f, body.right, body.top + 60.f}, active.textMuted, 13.f);

    const auto& all = ThemeManager::get().all();
    const float width = (body.width() - 3.f * 12.f) / 4.f;

    float x = body.left;
    for (size_t i = 0; i < all.size() && i < 4; ++i) {
        const Theme& entry = all[i];
        const Rect card{x, body.top + 76.f, x + width, body.top + 200.f};
        x += width + 12.f;

        const UiId id("onboard_theme", static_cast<int>(i));
        const bool current = entry.name == active.name;
        const bool pressed = gui.hoverAndClick(id, card);
        const float hover = gui.animate(id, gui.hovered(id));

        gui.renderer().fillRounded(card, entry.background, entry.panelRadius);
        gui.renderer().strokeRounded(card,
                                     current ? active.liveAccent()
                                             : entry.border.fade(0.6f + hover * 0.4f),
                                     entry.panelRadius, current ? 2.f : 1.f);

        gui.renderer().fillRounded(Rect{card.left + 12.f, card.top + 16.f, card.right - 12.f,
                                        card.top + 34.f},
                                   entry.surface, entry.radius);
        gui.renderer().fillRounded(Rect{card.left + 12.f, card.top + 42.f, card.left + 52.f,
                                        card.top + 58.f},
                                   entry.accent, entry.radius * 0.7f);

        gui.text(entry.name, Rect{card.left + 12.f, card.bottom - 32.f, card.right - 12.f,
                                  card.bottom - 12.f},
                 entry.text, 12.5f, FontWeight::SemiBold);

        if (pressed) {
            ThemeManager::get().apply(entry.name);
            config().theme = entry.name;
        }
    }

    bool rgb = active.rgbAccent;
    if (gui.toggleRow(UiId("onboard_rgb"),
                      Rect{body.left, body.top + 216.f, body.right, body.top + 260.f},
                      "Animated accent", "Cycles the accent colour's hue.", rgb)) {
        ThemeManager::get().mutableCurrent().rgbAccent = rgb;
    }
}

void Onboarding::drawUsage(const Rect& body) {
    Ui& gui = ui();
    const auto& active = theme();

    gui.text("How do you play?", Rect{body.left, body.top, body.right, body.top + 34.f},
             active.text, 22.f, FontWeight::Bold);
    gui.text("Velyx switches on a first set of modules and the profile that goes with it.",
             Rect{body.left, body.top + 34.f, body.right, body.top + 60.f}, active.textMuted, 13.f);

    float y = body.top + 78.f;
    for (size_t i = 0; i < kPresets.size(); ++i) {
        const Preset& preset = kPresets[i];
        const Rect card{body.left, y, body.right, y + 62.f};
        y += 72.f;

        const UiId id("onboard_preset", static_cast<int>(i));
        const bool selected = preset_ == preset.id;
        const bool pressed = gui.hoverAndClick(id, card);
        const float hover = gui.animate(id, gui.hovered(id));

        gui.renderer().fillRounded(card,
                                   selected ? active.liveAccent().fade(0.16f)
                                            : active.surface.fade(0.5f + hover * 0.4f),
                                   active.radius);
        if (selected) {
            gui.renderer().strokeRounded(card, active.liveAccent(), active.radius, 1.5f);
        }

        gui.text(preset.title, Rect{card.left + 16.f, card.top + 10.f, card.right - 16.f,
                                    card.top + 32.f},
                 active.text, 15.f, FontWeight::SemiBold);
        gui.text(preset.description, Rect{card.left + 16.f, card.top + 30.f, card.right - 16.f,
                                          card.bottom - 8.f},
                 active.textMuted, 12.f);

        if (pressed) applyPreset(preset.id);
    }
}

void Onboarding::drawKeys(const Rect& body) {
    Ui& gui = ui();
    const auto& active = theme();
    ClientConfig& settings = config();

    gui.text("Your keybinds", Rect{body.left, body.top, body.right, body.top + 34.f}, active.text,
             22.f, FontWeight::Bold);
    gui.text("Changeable at any time from the Keybinds page.",
             Rect{body.left, body.top + 34.f, body.right, body.top + 60.f}, active.textMuted, 13.f);

    const std::array<std::pair<const char*, Keybind*>, 4> binds{{
        {"Open the menu", &settings.guiKey},
        {"Open the menu on the search", &settings.searchKey},
        {"HUD editor", &settings.hudEditorKey},
        {"Screenshot", &settings.screenshotKey},
    }};

    float y = body.top + 80.f;
    for (size_t i = 0; i < binds.size(); ++i) {
        const auto& [label, bind] = binds[i];
        const Rect row{body.left, y, body.right, y + 40.f};
        y += 46.f;

        gui.text(label, Rect{row.left + 4.f, row.top, row.left + 240.f, row.bottom}, active.text,
                 13.5f);

        Keybind value = *bind;
        if (gui.keybindField(UiId("onboard_key", static_cast<int>(i)),
                             Rect{row.right - 200.f, row.top + 5.f, row.right, row.bottom - 5.f},
                             value)) {
            *bind = value;

            if (bind == &settings.guiKey) {
                if (Module* menu = modules().find("clickgui")) menu->keybind() = value;
            } else if (bind == &settings.hudEditorKey) {
                if (Module* editor = modules().find("hud_editor")) editor->keybind() = value;
            } else if (bind == &settings.screenshotKey) {
                // The capture mode carries its own key rather than a module keybind,
                // so this one has to be written where the module actually reads it.
                if (Module* capture = modules().find("screenshot_mode")) {
                    capture->settings.set("captureKey", SettingValue{value});
                }
            }
            settings.save();
        }
    }
}

void Onboarding::drawDone(const Rect& body) {
    Ui& gui = ui();
    const auto& active = theme();
    const Signatures& signatures = Signatures::get();

    gui.text("All set", Rect{body.left, body.top, body.right, body.top + 34.f}, active.text,
             22.f, FontWeight::Bold);

    const auto missing = signatures.missing();
    const bool healthy = missing.empty();

    const Rect banner{body.left, body.top + 54.f, body.right, body.top + 118.f};
    gui.renderer().fillRounded(banner, (healthy ? active.success : active.warning).fade(0.12f),
                               active.radius);
    gui.renderer().strokeRounded(banner, (healthy ? active.success : active.warning).fade(0.5f),
                                 active.radius, 1.f);

    gui.text(healthy ? "The signature pack matches your build of the game."
                     : "No signature pack for this build of the game.",
             Rect{banner.left + 14.f, banner.top + 10.f, banner.right - 14.f, banner.top + 34.f},
             healthy ? active.success : active.warning, 13.5f, FontWeight::SemiBold);

    gui.text(healthy
                 ? std::format("Minecraft {} detected, every module is available.",
                               signatures.gameVersion())
                 : "Modules that read the game will show -- until a pack is "
                   "installed. Everything else works.",
             Rect{banner.left + 14.f, banner.top + 32.f, banner.right - 14.f, banner.bottom - 8.f},
             active.textMuted, 12.f);

    gui.text(std::format("Press {} to open the menu at any time.",
                         describeKeybind(config().guiKey)),
             Rect{body.left, body.top + 140.f, body.right, body.top + 170.f}, active.text, 13.5f);
}

void Onboarding::onRender(RenderTopEvent& event) {
    if (finishRequested_.exchange(false, std::memory_order_acq_rel)) {
        finish();
        return;
    }

    Renderer& renderer = *event.renderer;
    const auto& active = theme();

    appear_.speed = active.motion(14.f);
    appear_.update(event.deltaSeconds);

    const float show = ease(active.easing, appear_.value);
    if (show <= 0.001f) return;

    Ui& gui = ui();
    gui.beginFrame(renderer, event.deltaSeconds);

    renderer.fillRect(Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y),
                      active.backgroundDeep.withAlpha(0.72f * show));

    const float width = std::min(kCardWidth, event.screenSize.x - 60.f);
    const float height = std::min(kCardHeight, event.screenSize.y - 60.f);
    const Rect card = Rect::fromSize((event.screenSize.x - width) * 0.5f,
                                     (event.screenSize.y - height) * 0.5f, width, height);

    renderer.pushScale({lerp(0.97f, 1.f, show), lerp(0.97f, 1.f, show)}, card.center());
    renderer.pushOpacity(show);

    gui.panel(card, active.panelRadius, true);

    const Rect body{card.left + 32.f, card.top + 28.f, card.right - 32.f, card.bottom - 76.f};

    switch (step_) {
        case 0: drawWelcome(body); break;
        case 1: drawStyle(body); break;
        case 2: drawUsage(body); break;
        case 3: drawKeys(body); break;
        default: drawDone(body); break;
    }

    const float dotY = card.bottom - 44.f;
    for (int i = 0; i < kStepCount; ++i) {
        const float x = card.left + 32.f + static_cast<float>(i) * 16.f;
        const float fill = gui.animate(UiId("onboard_dot", i), i <= step_, 18.f);
        renderer.fillCircle({x, dotY}, 4.f, lerp(active.border, active.liveAccent(), fill));
    }

    if (step_ > 0) {
        if (gui.button(UiId("onboard_back"),
                       Rect{card.right - 280.f, card.bottom - 58.f, card.right - 180.f,
                            card.bottom - 26.f},
                       "Back")) {
            --step_;
        }
    } else if (gui.button(UiId("onboard_skip"),
                          Rect{card.right - 280.f, card.bottom - 58.f, card.right - 180.f,
                               card.bottom - 26.f},
                          "Skip")) {
        finish();
    }

    const bool last = step_ >= kStepCount - 1;
    const bool blocked = step_ == 2 && preset_.empty();

    if (gui.button(UiId("onboard_next"),
                   Rect{card.right - 170.f, card.bottom - 58.f, card.right - 32.f,
                        card.bottom - 26.f},
                   last ? "Open the menu" : "Continue", true, !blocked)) {
        if (last) {
            finish();
            if (Module* menu = modules().find("clickgui")) menu->setEnabled(true);
        } else {
            ++step_;
        }
    }

    renderer.popOpacity();
    renderer.popTransform();
    gui.endFrame();
}

} // namespace velyx
