#include "ClickGui.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <format>

#include <velyx/Version.hpp>

#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/config/ProfileManager.hpp"
#include "dll/hook/HookManager.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/feature/Playtime.hpp"
#include "dll/feature/Screenshot.hpp"
#include "dll/feature/Services.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/ui/Theme.hpp"
#include "dll/ui/Ui.hpp"

namespace velyx {
namespace {

constexpr float kWindowWidth = 940.f;
constexpr float kWindowHeight = 580.f;
constexpr float kHeaderHeight = 56.f;
constexpr float kFooterHeight = 30.f;
constexpr float kSidebarWidth = 190.f;
constexpr float kListWidth = 300.f;
constexpr float kRowHeight = 46.f;

struct SidebarItem {
    const char* label;
    const char* glyph;
    int page;
    bool separatorBefore;
};

const SidebarItem kSidebar[] = {
    {"Favoris", "★", 0, false},
    {"Déplacement", "→", 1, true},
    {"HUD", "▤", 2, false},
    {"Rendu", "◈", 3, false},
    {"Utilitaires", "⚙", 4, false},
    {"Divers", "◆", 5, false},
    {"Scripts", "{}", 6, false},
    {"Thèmes", "◐", 7, true},
    {"Profils", "▣", 8, false},
    {"Historique", "◷", 11, false},
    {"Raccourcis", "⌨", 9, false},
    {"Diagnostic", "!", 10, false},
};

ModuleCategory categoryForPage(int page) {
    switch (page) {
        case 1: return ModuleCategory::Movement;
        case 2: return ModuleCategory::Hud;
        case 3: return ModuleCategory::Render;
        case 4: return ModuleCategory::Utility;
        case 5: return ModuleCategory::Misc;
        case 6: return ModuleCategory::Script;
        default: return ModuleCategory::Client;
    }
}

}

ClickGui::ClickGui()
    : Module("clickgui", "Menu Velyx", ModuleCategory::Client,
             "Le menu principal du client.") {
    markEssential();

    keybind() = config().guiKey;

    settings.header("Fenêtre");
    settings.toggle("rememberPosition", "Mémoriser la position", true);
    settings.position("windowPosition", "Position", {0.5f, 0.5f});
    settings.toggle("dimBackground", "Assombrir le jeu", true);
    settings.slider("dimAmount", "Intensité", 0.45f, 0.f, 0.9f);
    settings.toggle("blurBackground", "Flouter le fond", true);

    settings.find("dimAmount")->visibleWhen = [this] {
        return settings.value<bool>("dimBackground", true);
    };

    on(&ClickGui::onRender);
    on(&ClickGui::onMouse, EventPriority::First);
    on(&ClickGui::onKey, EventPriority::First);
    on(&ClickGui::onChar, EventPriority::First);

    addKeywords({"menu", "gui", "clickgui", "interface"});
}

void ClickGui::onEnable() {
    WindowHook::setCaptureInput(true);
    open_.set(0.f);
    open_.to(1.f);
}

void ClickGui::onDisable() {
    WindowHook::setCaptureInput(false);
    profiles().saveCurrent();
}

void ClickGui::onMouse(MouseEvent& event) {
    ui().feedMouse(event);
    event.cancel();
}

void ClickGui::onKey(KeyEvent& event) {

    if (event.key == keybind().key) return;

    if (event.down && event.key == VK_ESCAPE && !ui().capturingText()) {
        setEnabled(false);
        event.cancel();
        return;
    }

    ui().feedKey(event);
    event.cancel();
}

void ClickGui::onChar(CharEvent& event) {
    ui().feedChar(event);
    event.cancel();
}

std::vector<Module*> ClickGui::visibleModules() const {
    if (!search_.empty()) {
        std::vector<Module*> result;
        for (const auto& hit : modules().search(search_, 40)) {
            if (hit.setting) continue;
            if (std::ranges::find(result, hit.module) == result.end()) result.push_back(hit.module);
        }
        return result;
    }

    if (page_ == Page::Favourites) {
        auto result = modules().favourites();
        if (result.empty()) {

            result = modules().enabled();
        }
        return result;
    }

    return modules().byCategory(categoryForPage(static_cast<int>(page_)));
}

void ClickGui::onRender(RenderTopEvent& event) {
    Renderer& renderer = *event.renderer;
    const auto& active = theme();

    open_.speed = active.motion(14.f);
    open_.update(event.deltaSeconds);

    const float appear = ease(active.easing, open_.value);
    if (appear <= 0.001f) return;

    Ui& gui = ui();
    gui.beginFrame(renderer, event.deltaSeconds);

    if (settings.value<bool>("dimBackground", true)) {
        const float amount = settings.value<float>("dimAmount", 0.45f) * appear;
        renderer.fillRect(Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y),
                          active.backgroundDeep.withAlpha(amount));
    }

    const Vec2 stored = settings.value<Vec2>("windowPosition", {0.5f, 0.5f});
    const Vec2 centre{stored.x * event.screenSize.x, stored.y * event.screenSize.y};

    const float width = std::min(kWindowWidth, event.screenSize.x - 40.f);
    const float height = std::min(kWindowHeight, event.screenSize.y - 40.f);

    window_ = Rect::fromSize(centre.x - width * 0.5f, centre.y - height * 0.5f, width, height);

    renderer.pushScale({lerp(0.96f, 1.f, appear), lerp(0.96f, 1.f, appear)}, window_.center());
    renderer.pushOpacity(appear);

    if (settings.value<bool>("blurBackground", true) && active.blur) {
        renderer.blurBehind(window_, active.blurSigma * 1.6f, active.panelRadius);
    }
    gui.panel(window_, active.panelRadius, false);

    const Rect header{window_.left, window_.top, window_.right, window_.top + kHeaderHeight};
    const Rect footer{window_.left, window_.bottom - kFooterHeight, window_.right, window_.bottom};
    const Rect body{window_.left, header.bottom, window_.right, footer.top};

    drawHeader(header);

    const Rect sidebar{body.left, body.top, body.left + kSidebarWidth, body.bottom};
    drawSidebar(sidebar);

    const Rect content{sidebar.right, body.top, body.right, body.bottom};

    switch (page_) {
        case Page::Themes:
            drawThemesPage(content);
            break;
        case Page::Profiles:
            drawProfilesPage(content);
            break;
        case Page::Keybinds:
            drawKeybindsPage(content);
            break;
        case Page::Diagnostics:
            drawDiagnosticsPage(content);
            break;
        case Page::History:
            drawHistoryPage(content);
            break;
        default: {
            const Rect list{content.left, content.top, content.left + kListWidth, content.bottom};
            drawModuleList(list);
            drawSettings(Rect{list.right, content.top, content.right, content.bottom});
            break;
        }
    }

    drawFooter(footer);

    renderer.popOpacity();
    renderer.popTransform();

    gui.endFrame();
}

void ClickGui::drawHeader(const Rect& rect) {
    Ui& gui = ui();
    Renderer& renderer = gui.renderer();
    const auto& active = theme();

    renderer.fillRoundedCorners(rect, active.surface.fade(0.5f), active.panelRadius,
                                active.panelRadius, 0.f, 0.f);
    renderer.line({rect.left, rect.bottom}, {rect.right, rect.bottom}, active.border.fade(0.7f),
                  1.f);

    const Rect mark{rect.left + 20.f, rect.top, rect.left + 130.f, rect.bottom};

    const Vec2 markCentre{mark.left + 11.f, mark.center().y};
    renderer.polyline({{markCentre.x - 9.f, markCentre.y - 9.f},
                       {markCentre.x, markCentre.y + 9.f},
                       {markCentre.x + 9.f, markCentre.y - 9.f}},
                      active.liveAccent(), 2.6f);

    gui.text("VELYX", Rect{mark.left + 30.f, mark.top, mark.right, mark.bottom}, active.text, 18.f,
             FontWeight::Bold);

    const Rect search{rect.left + 160.f, rect.center().y - 15.f, rect.left + 480.f,
                      rect.center().y + 15.f};
    gui.textField(UiId("search"), search, search_, "Rechercher un module ou un réglage…", 48);

    const Rect profileRect{rect.right - 250.f, rect.center().y - 15.f, rect.right - 60.f,
                           rect.center().y + 15.f};

    std::string currentProfile = profiles().current().name;
    if (currentProfile.empty()) currentProfile = "Global";

    auto names = profiles().names();
    if (gui.dropdown(UiId("profile_switch"), profileRect, currentProfile, names)) {
        profiles().switchTo(currentProfile);
        config().activeProfile = currentProfile;
        config().save();
    }

    const Rect close{rect.right - 46.f, rect.center().y - 15.f, rect.right - 16.f,
                     rect.center().y + 15.f};
    if (gui.iconButton(UiId("close"), close, "✕", active.textMuted)) setEnabled(false);

    const Rect dragArea{rect.left, rect.top, search.left - 8.f, rect.bottom};
    if (gui.clicked() && dragArea.contains(gui.mouse())) {
        dragging_ = true;
        dragOffset_ = gui.mouse() - window_.center();
    }
    if (!gui.mouseDown()) dragging_ = false;

    if (dragging_ && settings.value<bool>("rememberPosition", true)) {
        const Vec2 screen = Velyx::get().screenSize();
        if (screen.x > 0.f && screen.y > 0.f) {
            const Vec2 centre = gui.mouse() - dragOffset_;
            settings.set("windowPosition",
                         SettingValue{Vec2{clamp(centre.x / screen.x, 0.05f, 0.95f),
                                           clamp(centre.y / screen.y, 0.05f, 0.95f)}});
        }
    }
}

void ClickGui::drawSidebar(const Rect& rect) {
    Ui& gui = ui();
    Renderer& renderer = gui.renderer();
    const auto& active = theme();

    renderer.fillRect(rect, active.backgroundDeep.fade(0.35f));
    renderer.line({rect.right, rect.top}, {rect.right, rect.bottom}, active.border.fade(0.7f), 1.f);

    float y = rect.top + 12.f;

    for (const SidebarItem& item : kSidebar) {
        if (item.separatorBefore) {
            renderer.line({rect.left + 16.f, y + 4.f}, {rect.right - 16.f, y + 4.f},
                          active.border.fade(0.5f), 1.f);
            y += 12.f;
        }

        const Rect row{rect.left + 8.f, y, rect.right - 8.f, y + 34.f};
        y += 36.f;

        const bool selected = static_cast<int>(page_) == item.page;
        const UiId id("sidebar", item.page);
        const float hover = gui.animate(id, gui.hovered(id));
        const float selection = gui.animate(UiId("sidebar_sel", item.page), selected, 20.f);

        if (hover > 0.01f || selection > 0.01f) {
            renderer.fillRounded(row,
                                 lerp(active.surfaceHover.fade(hover * 0.6f),
                                      active.liveAccent().fade(0.16f), selection),
                                 active.radius);
        }

        if (selection > 0.01f) {
            const float barHeight = row.height() * 0.55f * selection;
            renderer.fillRounded(Rect{row.left - 5.f, row.center().y - barHeight * 0.5f,
                                      row.left - 2.f, row.center().y + barHeight * 0.5f},
                                 active.liveAccent(), 1.5f);
        }

        const Color foreground = lerp(active.textMuted, active.text, std::max(hover, selection));

        gui.text(item.glyph, Rect{row.left + 10.f, row.top, row.left + 34.f, row.bottom},
                 lerp(foreground, active.liveAccent(), selection), 14.f, FontWeight::Medium,
                 TextAlign::Center);
        gui.text(item.label, Rect{row.left + 40.f, row.top, row.right - 30.f, row.bottom},
                 foreground, 13.5f, selected ? FontWeight::SemiBold : FontWeight::Medium);

        if (item.page >= 1 && item.page <= 6) {
            const auto list = modules().byCategory(categoryForPage(item.page));
            const auto activeCount = std::ranges::count_if(
                list, [](const Module* module) { return module->enabled(); });

            if (activeCount > 0) {
                gui.text(std::to_string(activeCount),
                         Rect{row.right - 30.f, row.top, row.right - 8.f, row.bottom},
                         active.liveAccent(), 11.f, FontWeight::Bold, TextAlign::Right);
            }
        }

        if (gui.hovered(id) && gui.clicked()) {
            page_ = static_cast<Page>(item.page);
            selected_ = nullptr;
            search_.clear();
        }
    }
}

void ClickGui::drawModuleList(const Rect& rect) {
    Ui& gui = ui();
    Renderer& renderer = gui.renderer();
    const auto& active = theme();

    renderer.line({rect.right, rect.top}, {rect.right, rect.bottom}, active.border.fade(0.7f), 1.f);

    const auto list = visibleModules();

    if (list.empty()) {
        gui.text(search_.empty() ? "Aucun module dans cette catégorie."
                                 : "Aucun résultat.",
                 rect, active.textMuted, 13.f, FontWeight::Regular, TextAlign::Center);
        return;
    }

    const Rect view = rect.inflated(-10.f);
    const float contentHeight = static_cast<float>(list.size()) * (kRowHeight + 6.f);
    const float offset = gui.beginScroll(UiId("module_list"), view, contentHeight);

    float y = view.top + offset;

    for (size_t i = 0; i < list.size(); ++i) {
        Module* module = list[i];
        const Rect row{view.left, y, view.right - 6.f, y + kRowHeight};
        y += kRowHeight + 6.f;

        if (row.bottom < view.top - 20.f || row.top > view.bottom + 20.f) continue;

        const UiId id("module_row", static_cast<int>(strings::hash32(module->id())));
        const bool isSelected = selected_ == module;
        const float hover = gui.animate(id, gui.hovered(id));
        const float selection = gui.animate(UiId("module_sel", static_cast<int>(i)), isSelected, 20.f);

        renderer.fillRounded(row,
                             lerp(active.surface.fade(0.55f + hover * 0.35f),
                                  active.liveAccent().fade(0.14f), selection),
                             active.radius);

        if (module->enabled()) {
            renderer.fillRounded(Rect{row.left, row.top + 8.f, row.left + 3.f, row.bottom - 8.f},
                                 active.liveAccent(), 1.5f);
        }
        if (selection > 0.01f) {
            renderer.strokeRounded(row, active.liveAccent().fade(selection), active.radius,
                                   active.borderWidth);
        }

        gui.text(module->name(), Rect{row.left + 14.f, row.top + 6.f, row.right - 90.f,
                                      row.top + kRowHeight * 0.55f},
                 module->enabled() ? active.text : active.text.fade(0.75f), 13.5f,
                 FontWeight::SemiBold);

        gui.text(module->description(),
                 Rect{row.left + 14.f, row.top + kRowHeight * 0.5f, row.right - 90.f,
                      row.bottom - 4.f},
                 active.textMuted, 11.f, FontWeight::Regular);

        const Rect star{row.right - 78.f, row.center().y - 12.f, row.right - 54.f,
                        row.center().y + 12.f};
        if (gui.iconButton(UiId("fav", static_cast<int>(i)), star, module->favourite() ? "★" : "☆",
                           module->favourite() ? active.liveAccent() : active.textMuted)) {
            module->setFavourite(!module->favourite());
        }

        bool enabled = module->enabled();
        if (gui.toggle(UiId("module_toggle", static_cast<int>(i)),
                       Rect{row.right - 50.f, row.top, row.right - 10.f, row.bottom}, enabled)) {
            module->setEnabled(enabled);
        }

        if (gui.hovered(id) && gui.clicked() && !star.contains(gui.mouse())) selected_ = module;
    }

    gui.endScroll();
}

float ClickGui::settingHeight(const Setting& setting) {
    switch (setting.type) {
        case SettingType::Header:   return 34.f;
        case SettingType::Slider:
        case SettingType::IntSlider: return 54.f;
        case SettingType::Toggle:    return setting.description.empty() ? 40.f : 52.f;
        case SettingType::Position:  return 44.f;
        default:                     return 46.f;
    }
}

float ClickGui::drawSetting(const Rect& rect, Module& owner, Setting& setting, int index) {
    Ui& gui = ui();
    const auto& active = theme();

    const float height = settingHeight(setting);
    const Rect row{rect.left, rect.top, rect.right, rect.top + height};
    const UiId id("setting", index * 977 + static_cast<int>(strings::hash32(setting.id)));

    switch (setting.type) {
        case SettingType::Header:
            gui.sectionHeader(setting.label, Rect{row.left + 6.f, row.top + 12.f, row.right, row.bottom});
            break;

        case SettingType::Toggle: {
            bool value = std::get<bool>(setting.value);
            if (gui.toggleRow(id, row, setting.label, setting.description, value)) {
                owner.settings.set(setting.id, SettingValue{value});
                SettingChangeEvent change;
                change.module = &owner;
                change.setting = setting.id;
                events().emit(change);
            }
            break;
        }

        case SettingType::Slider: {
            float value = std::get<float>(setting.value);
            if (gui.sliderRow(id, row, setting.label, setting.description, value, setting.min,
                              setting.max, setting.unit, setting.step)) {
                owner.settings.set(setting.id, SettingValue{value});
            }
            break;
        }

        case SettingType::IntSlider: {
            float value = static_cast<float>(std::get<int>(setting.value));
            if (gui.sliderRow(id, row, setting.label, setting.description, value, setting.min,
                              setting.max, setting.unit, 1.f)) {
                owner.settings.set(setting.id, SettingValue{static_cast<int>(std::round(value))});
            }
            break;
        }

        case SettingType::Dropdown: {
            gui.text(setting.label, Rect{row.left + 10.f, row.top, row.left + 150.f, row.bottom},
                     active.text, 14.f);
            std::string value = std::get<std::string>(setting.value);
            if (gui.dropdown(id, Rect{row.left + 160.f, row.center().y - 15.f, row.right - 10.f,
                                      row.center().y + 15.f},
                             value, setting.options)) {
                owner.settings.set(setting.id, SettingValue{value});
            }
            break;
        }

        case SettingType::Text: {
            gui.text(setting.label, Rect{row.left + 10.f, row.top, row.left + 150.f, row.bottom},
                     active.text, 14.f);
            std::string value = std::get<std::string>(setting.value);
            if (gui.textField(id, Rect{row.left + 160.f, row.center().y - 15.f, row.right - 10.f,
                                       row.center().y + 15.f},
                              value, setting.description)) {
                owner.settings.set(setting.id, SettingValue{value});
            }
            break;
        }

        case SettingType::Color: {
            Color value = std::get<Color>(setting.value);
            if (gui.colorRow(id, row, setting.label, value)) {
                owner.settings.set(setting.id, SettingValue{value});
            }
            break;
        }

        case SettingType::Keybind: {
            gui.text(setting.label, Rect{row.left + 10.f, row.top, row.left + 150.f, row.bottom},
                     active.text, 14.f);
            Keybind value = std::get<Keybind>(setting.value);
            if (gui.keybindField(id, Rect{row.right - 190.f, row.center().y - 15.f, row.right - 10.f,
                                          row.center().y + 15.f},
                                 value)) {
                owner.settings.set(setting.id, SettingValue{value});
            }
            break;
        }

        case SettingType::Position: {
            gui.text(setting.label, Rect{row.left + 10.f, row.top, row.left + 200.f, row.bottom},
                     active.text, 14.f);
            gui.text("Déplacez l'élément dans l'éditeur de HUD",
                     Rect{row.left + 200.f, row.top, row.right - 10.f, row.bottom},
                     active.textMuted, 11.5f, FontWeight::Regular, TextAlign::Right);
            break;
        }

        case SettingType::Button: {
            gui.text(setting.description,
                     Rect{row.left + 10.f, row.top, row.right - 130.f, row.bottom},
                     active.textMuted, 12.f, FontWeight::Regular);
            if (gui.button(id, Rect{row.right - 120.f, row.center().y - 15.f, row.right - 10.f,
                                    row.center().y + 15.f},
                           setting.label) &&
                setting.action) {
                setting.action();
            }
            break;
        }
    }

    return height;
}

void ClickGui::drawSettings(const Rect& rect) {
    Ui& gui = ui();
    const auto& active = theme();

    if (!selected_) {
        gui.text("Sélectionnez un module pour voir ses réglages.", rect, active.textMuted, 13.f,
                 FontWeight::Regular, TextAlign::Center);
        return;
    }

    Module& module = *selected_;
    const Rect header{rect.left + 18.f, rect.top + 12.f, rect.right - 18.f, rect.top + 74.f};

    gui.text(module.name(), Rect{header.left, header.top, header.right - 120.f, header.top + 26.f},
             active.text, 17.f, FontWeight::Bold);
    gui.text(module.description(),
             Rect{header.left, header.top + 26.f, header.right - 120.f, header.bottom},
             active.textMuted, 12.f, FontWeight::Regular);

    bool enabled = module.enabled();
    if (gui.toggle(UiId("selected_toggle"),
                   Rect{header.right - 44.f, header.top, header.right, header.top + 26.f},
                   enabled)) {
        module.setEnabled(enabled);
    }

    if (module.permissions().any()) {
        const auto labels = module.permissions().describe();
        gui.text("Accès : " + strings::join(labels, " · "),
                 Rect{header.left, header.bottom - 2.f, header.right, header.bottom + 14.f},
                 active.warning.fade(0.9f), 11.f, FontWeight::Medium);
    }

    const Rect keybindRect{header.right - 190.f, header.top + 30.f, header.right,
                           header.top + 58.f};
    Keybind bind = module.keybind();
    if (gui.keybindField(UiId("module_keybind"), keybindRect, bind)) module.keybind() = bind;

    const Rect view{rect.left + 8.f, rect.top + 92.f, rect.right - 8.f, rect.bottom - 8.f};

    float contentHeight = 8.f;
    for (const Setting& setting : module.settings.list()) {
        if (!setting.visible()) continue;
        if (setting.advanced && !showAdvanced_) continue;
        contentHeight += settingHeight(setting);
    }
    contentHeight += 46.f;

    const float offset = gui.beginScroll(UiId("settings_scroll"), view, contentHeight);

    float y = view.top + offset;
    int index = 0;

    for (Setting& setting : module.settings.list()) {
        if (!setting.visible()) {
            ++index;
            continue;
        }
        if (setting.advanced && !showAdvanced_) {
            ++index;
            continue;
        }

        const Rect row{view.left, y, view.right - 8.f, view.bottom};
        y += drawSetting(row, module, setting, index++);
    }

    const Rect actions{view.left + 8.f, y + 8.f, view.right - 16.f, y + 38.f};

    const bool hasAdvanced = std::ranges::any_of(
        module.settings.list(), [](const Setting& setting) { return setting.advanced; });

    if (hasAdvanced) {
        if (gui.button(UiId("advanced"),
                       Rect{actions.left, actions.top, actions.left + 190.f, actions.bottom},
                       showAdvanced_ ? "Masquer les options avancées"
                                     : "Options avancées")) {
            showAdvanced_ = !showAdvanced_;
        }
    }

    if (gui.button(UiId("reset"),
                   Rect{actions.right - 150.f, actions.top, actions.right, actions.bottom},
                   "Réinitialiser")) {
        module.settings.resetAll();
    }

    gui.endScroll();
}

void ClickGui::drawThemesPage(const Rect& rect) {
    Ui& gui = ui();
    ThemeManager& manager = ThemeManager::get();
    const auto& active = theme();

    const Rect header{rect.left + 20.f, rect.top + 14.f, rect.right - 20.f, rect.top + 46.f};
    gui.text("Thèmes", header, active.text, 17.f, FontWeight::Bold);
    gui.text("Modifiez les couleurs, les arrondis, les polices et les animations.",
             Rect{header.left, header.bottom - 10.f, header.right, header.bottom + 8.f},
             active.textMuted, 12.f, FontWeight::Regular);

    const Rect gallery{rect.left + 20.f, rect.top + 66.f, rect.right - 20.f, rect.top + 168.f};
    const auto& all = manager.all();

    const float cardWidth = 150.f;
    float x = gallery.left;

    for (size_t i = 0; i < all.size(); ++i) {
        const Theme& entry = all[i];
        const Rect card{x, gallery.top, x + cardWidth, gallery.bottom};
        x += cardWidth + 12.f;
        if (card.right > gallery.right) break;

        const UiId id("theme_card", static_cast<int>(i));
        const bool isCurrent = entry.name == active.name;
        const float hover = gui.animate(id, gui.hovered(id));

        gui.renderer().fillRounded(card, entry.background, entry.panelRadius);
        gui.renderer().strokeRounded(card,
                                     isCurrent ? active.liveAccent()
                                               : entry.border.fade(0.6f + hover * 0.4f),
                                     entry.panelRadius, isCurrent ? 2.f : 1.f);

        gui.renderer().fillRounded(Rect{card.left + 12.f, card.top + 14.f, card.right - 12.f,
                                        card.top + 34.f},
                                   entry.surface, entry.radius);
        gui.renderer().fillRounded(Rect{card.left + 12.f, card.top + 42.f, card.left + 60.f,
                                        card.top + 58.f},
                                   entry.accent, entry.radius * 0.7f);
        gui.renderer().fillRounded(Rect{card.left + 66.f, card.top + 42.f, card.left + 100.f,
                                        card.top + 58.f},
                                   entry.accentDeep, entry.radius * 0.7f);

        gui.text(entry.name, Rect{card.left + 12.f, card.bottom - 34.f, card.right - 12.f,
                                  card.bottom - 12.f},
                 entry.text, 13.f, FontWeight::SemiBold);

        if (gui.hovered(id) && gui.clicked()) {
            manager.apply(entry.name);
            profiles().mutableCurrent().theme = entry.name;
        }
    }

    Theme draft = manager.current();
    const Rect editor{rect.left + 20.f, gallery.bottom + 18.f, rect.right - 20.f, rect.bottom - 60.f};

    const float contentHeight = 9.f * 44.f + 40.f;
    const float offset = gui.beginScroll(UiId("theme_editor"), editor, contentHeight);

    float y = editor.top + offset;
    const auto row = [&](float height) {
        const Rect result{editor.left, y, editor.right - 8.f, y + height};
        y += height;
        return result;
    };

    bool changed = false;
    changed |= gui.colorRow(UiId("t_accent"), row(40.f), "Accent principal", draft.accent);
    changed |= gui.colorRow(UiId("t_accent2"), row(40.f), "Accent secondaire", draft.accentDeep);
    changed |= gui.colorRow(UiId("t_bg"), row(40.f), "Fond", draft.background);
    changed |= gui.colorRow(UiId("t_surface"), row(40.f), "Surface", draft.surface);
    changed |= gui.colorRow(UiId("t_text"), row(40.f), "Texte", draft.text);
    changed |= gui.sliderRow(UiId("t_radius"), row(52.f), "Arrondi des panneaux", "", draft.panelRadius,
                             0.f, 32.f, " px");
    changed |= gui.sliderRow(UiId("t_scale"), row(52.f), "Échelle du texte", "", draft.fontScale,
                             0.7f, 1.6f, "x");
    changed |= gui.sliderRow(UiId("t_motion"), row(52.f), "Vitesse des animations",
                             "0 désactive complètement les animations.", draft.animationSpeed, 0.f,
                             2.f, "x");
    changed |= gui.toggleRow(UiId("t_blur"), row(44.f), "Flou d'arrière-plan", "", draft.blur);
    changed |= gui.toggleRow(UiId("t_rgb"), row(44.f), "Accent RGB",
                             "Fait défiler la teinte de l'accent.", draft.rgbAccent);

    gui.endScroll();

    if (changed) manager.mutableCurrent() = draft;

    const Rect actions{rect.left + 20.f, rect.bottom - 48.f, rect.right - 20.f, rect.bottom - 14.f};
    if (gui.button(UiId("theme_save"),
                   Rect{actions.left, actions.top, actions.left + 190.f, actions.bottom},
                   "Enregistrer comme nouveau thème", true)) {
        Theme copy = manager.current();
        copy.name += " personnalisé";
        manager.save(copy);
    }
    if (gui.button(UiId("theme_reload"),
                   Rect{actions.left + 200.f, actions.top, actions.left + 340.f, actions.bottom},
                   "Recharger")) {
        manager.load();
    }
}

void ClickGui::drawProfilesPage(const Rect& rect) {
    Ui& gui = ui();
    ProfileManager& manager = profiles();
    const auto& active = theme();

    gui.text("Profils", Rect{rect.left + 20.f, rect.top + 14.f, rect.right - 20.f, rect.top + 42.f},
             active.text, 17.f, FontWeight::Bold);
    gui.text("Un jeu de réglages par contexte, avec bascule automatique selon le serveur.",
             Rect{rect.left + 20.f, rect.top + 38.f, rect.right - 20.f, rect.top + 58.f},
             active.textMuted, 12.f, FontWeight::Regular);

    const Rect list{rect.left + 20.f, rect.top + 70.f, rect.left + 380.f, rect.bottom - 120.f};
    const auto& all = manager.all();

    const float contentHeight = static_cast<float>(all.size()) * 58.f;
    const float offset = gui.beginScroll(UiId("profile_list"), list, contentHeight);

    float y = list.top + offset;
    for (size_t i = 0; i < all.size(); ++i) {
        const Profile& profile = all[i];
        const Rect row{list.left, y, list.right - 8.f, y + 52.f};
        y += 58.f;

        const UiId id("profile_row", static_cast<int>(i));
        const bool isCurrent = profile.name == manager.current().name;
        const float hover = gui.animate(id, gui.hovered(id));

        gui.renderer().fillRounded(row,
                                   isCurrent ? active.liveAccent().fade(0.14f)
                                             : active.surface.fade(0.5f + hover * 0.4f),
                                   active.radius);
        if (isCurrent) {
            gui.renderer().strokeRounded(row, active.liveAccent().fade(0.6f), active.radius, 1.f);
        }

        gui.text(profile.name, Rect{row.left + 14.f, row.top + 6.f, row.right - 20.f, row.top + 28.f},
                 active.text, 13.5f, FontWeight::SemiBold);

        const std::string subtitle =
            profile.serverMatches.empty()
                ? (profile.isDefault ? "Profil par défaut" : "Aucune règle automatique")
                : "Auto : " + strings::join(profile.serverMatches, ", ");
        gui.text(subtitle, Rect{row.left + 14.f, row.top + 26.f, row.right - 20.f, row.bottom - 4.f},
                 active.textMuted, 11.f, FontWeight::Regular);

        if (gui.hovered(id) && gui.clicked() && !isCurrent) {
            manager.switchTo(profile.name);
            config().activeProfile = profile.name;
            config().save();
        }
    }
    gui.endScroll();

    const Rect side{rect.left + 400.f, rect.top + 70.f, rect.right - 20.f, rect.bottom - 20.f};

    gui.sectionHeader("Nouveau profil", Rect{side.left, side.top, side.right, side.top + 20.f});
    gui.textField(UiId("new_profile"),
                  Rect{side.left, side.top + 28.f, side.right - 110.f, side.top + 58.f},
                  newProfileName_, "Nom du profil", 32);
    if (gui.button(UiId("create_profile"),
                   Rect{side.right - 100.f, side.top + 28.f, side.right, side.top + 58.f}, "Créer",
                   true)) {
        if (!newProfileName_.empty()) {
            manager.create(newProfileName_);
            newProfileName_.clear();
        }
    }

    gui.sectionHeader("Partage", Rect{side.left, side.top + 78.f, side.right, side.top + 98.f});
    gui.textField(UiId("share_code"),
                  Rect{side.left, side.top + 106.f, side.right, side.top + 136.f}, profileCode_,
                  "Collez un code VELYX1:… ou exportez le profil actuel", 4096);

    if (gui.button(UiId("export_code"),
                   Rect{side.left, side.top + 146.f, side.left + 150.f, side.top + 176.f},
                   "Exporter")) {
        profileCode_ = manager.exportCode();
    }
    if (gui.button(UiId("import_code"),
                   Rect{side.left + 160.f, side.top + 146.f, side.left + 310.f, side.top + 176.f},
                   "Importer")) {
        std::string imported;
        if (manager.importCode(profileCode_, &imported)) profileCode_ = "Importé : " + imported;
    }

    gui.sectionHeader("Historique", Rect{side.left, side.top + 196.f, side.right, side.top + 216.f});

    const auto versions = manager.versions(manager.current().name);
    float versionY = side.top + 224.f;

    for (size_t i = 0; i < versions.size() && i < 5; ++i) {
        const Rect row{side.left, versionY, side.right, versionY + 30.f};
        versionY += 34.f;

        gui.text(versions[i].id + "  ·  " + versions[i].label,
                 Rect{row.left, row.top, row.right - 110.f, row.bottom}, active.textMuted, 11.5f,
                 FontWeight::Regular);

        if (gui.button(UiId("restore", static_cast<int>(i)),
                       Rect{row.right - 100.f, row.top, row.right, row.bottom}, "Restaurer")) {
            manager.restore(manager.current().name, versions[i].id);
        }
    }

    if (gui.button(UiId("snapshot"),
                   Rect{side.left, versionY + 6.f, side.left + 190.f, versionY + 36.f},
                   "Créer un point de restauration")) {
        manager.snapshot("manuel");
    }
}

void ClickGui::drawKeybindsPage(const Rect& rect) {
    Ui& gui = ui();
    const auto& active = theme();

    gui.text("Raccourcis", Rect{rect.left + 20.f, rect.top + 14.f, rect.right - 20.f, rect.top + 42.f},
             active.text, 17.f, FontWeight::Bold);
    gui.text("Tous les raccourcis du client, au même endroit.",
             Rect{rect.left + 20.f, rect.top + 38.f, rect.right - 20.f, rect.top + 58.f},
             active.textMuted, 12.f, FontWeight::Regular);

    const Rect view{rect.left + 20.f, rect.top + 70.f, rect.right - 20.f, rect.bottom - 16.f};

    std::vector<Module*> bindable;
    for (const auto& module : modules().all()) bindable.push_back(module.get());
    std::ranges::sort(bindable, [](const Module* a, const Module* b) {

        if (a->keybind().bound() != b->keybind().bound()) return a->keybind().bound();
        return a->name() < b->name();
    });

    const float offset =
        gui.beginScroll(UiId("keybinds"), view, static_cast<float>(bindable.size()) * 40.f);

    float y = view.top + offset;
    for (size_t i = 0; i < bindable.size(); ++i) {
        Module* module = bindable[i];
        const Rect row{view.left, y, view.right - 8.f, y + 34.f};
        y += 40.f;

        if (row.bottom < view.top - 20.f || row.top > view.bottom + 20.f) continue;

        gui.text(module->name(), Rect{row.left + 6.f, row.top, row.left + 240.f, row.bottom},
                 module->keybind().bound() ? active.text : active.textMuted, 13.f);
        gui.text(categoryLabel(module->category()),
                 Rect{row.left + 250.f, row.top, row.left + 380.f, row.bottom}, active.textMuted,
                 11.5f, FontWeight::Regular);

        Keybind bind = module->keybind();
        if (gui.keybindField(UiId("kb", static_cast<int>(i)),
                             Rect{row.right - 320.f, row.top, row.right - 140.f, row.bottom},
                             bind)) {
            module->keybind() = bind;
        }

        std::string mode = bind.mode == Keybind::Mode::Hold     ? "Maintien"
                           : bind.mode == Keybind::Mode::Once   ? "Impulsion"
                                                                : "Bascule";
        if (gui.dropdown(UiId("kbmode", static_cast<int>(i)),
                         Rect{row.right - 130.f, row.top, row.right, row.bottom}, mode,
                         {"Bascule", "Maintien", "Impulsion"})) {
            module->keybind().mode = mode == "Maintien"    ? Keybind::Mode::Hold
                                     : mode == "Impulsion" ? Keybind::Mode::Once
                                                           : Keybind::Mode::Toggle;
        }
    }

    gui.endScroll();
}

void ClickGui::drawDiagnosticsPage(const Rect& rect) {
    Ui& gui = ui();
    const auto& active = theme();
    const Signatures& signatures = Signatures::get();

    gui.text("Diagnostic", Rect{rect.left + 20.f, rect.top + 14.f, rect.right - 20.f, rect.top + 42.f},
             active.text, 17.f, FontWeight::Bold);

    const auto missing = signatures.missing();
    const bool healthy = missing.empty();

    const Rect banner{rect.left + 20.f, rect.top + 54.f, rect.right - 20.f, rect.top + 100.f};
    gui.renderer().fillRounded(banner,
                               (healthy ? active.success : active.warning).fade(0.14f),
                               active.radius);
    gui.renderer().strokeRounded(banner, (healthy ? active.success : active.warning).fade(0.5f),
                                 active.radius, 1.f);

    gui.text(healthy ? "Toutes les signatures requises sont résolues."
                     : std::format("{} signature(s) requises manquent pour cette version du jeu.",
                                   missing.size()),
             Rect{banner.left + 14.f, banner.top, banner.right - 14.f, banner.center().y + 2.f},
             healthy ? active.success : active.warning, 13.f, FontWeight::SemiBold);

    gui.text(std::format("Minecraft {}  ·  Velyx {}  ·  {} hooks",
                         signatures.gameVersion(), version::kFull,
                         HookManager::get().count()),
             Rect{banner.left + 14.f, banner.center().y, banner.right - 14.f, banner.bottom - 6.f},
             active.textMuted, 11.5f, FontWeight::Regular);

    float top = rect.top + 112.f;

    if (const auto report = crash::lastReport()) {
        const Rect crashBanner{rect.left + 20.f, top, rect.right - 20.f, top + 74.f};
        top = crashBanner.bottom + 12.f;

        gui.renderer().fillRounded(crashBanner, active.danger.fade(0.12f), active.radius);
        gui.renderer().strokeRounded(crashBanner, active.danger.fade(0.5f), active.radius, 1.f);

        gui.text(std::format("Dernier plantage : {} ({})", report->exception, report->when),
                 Rect{crashBanner.left + 14.f, crashBanner.top + 8.f, crashBanner.right - 130.f,
                      crashBanner.top + 30.f},
                 active.danger, 13.f, FontWeight::SemiBold);

        gui.text(report->insideVelyx
                     ? std::format("Origine probable : module « {} ». Le mode sans échec le "
                                   "désactivera au prochain plantage.", report->suspect)
                     : "Origine hors de Velyx : jeu, pilote graphique ou autre logiciel injecté.",
                 Rect{crashBanner.left + 14.f, crashBanner.top + 30.f, crashBanner.right - 130.f,
                      crashBanner.bottom - 8.f},
                 active.textMuted, 11.5f, FontWeight::Regular);

        if (gui.button(UiId("crash_open"),
                       Rect{crashBanner.right - 118.f, crashBanner.top + 10.f,
                            crashBanner.right - 14.f, crashBanner.top + 40.f},
                       "Ouvrir")) {
            screenshot::revealInExplorer(report->file);
        }
        if (gui.button(UiId("crash_clear"),
                       Rect{crashBanner.right - 118.f, crashBanner.top + 44.f,
                            crashBanner.right - 14.f, crashBanner.bottom - 8.f},
                       "Effacer")) {
            crash::clearReports();
        }
    }

    const Rect view{rect.left + 20.f, top, rect.right - 20.f, rect.bottom - 16.f};
    const auto all = signatures.all();

    const float offset =
        gui.beginScroll(UiId("sig_list"), view, static_cast<float>(all.size()) * 26.f);

    float y = view.top + offset;
    for (const SignatureResult& result : all) {
        const Rect row{view.left, y, view.right - 8.f, y + 22.f};
        y += 26.f;

        if (row.bottom < view.top - 20.f || row.top > view.bottom + 20.f) continue;

        const Color colour = result.resolved      ? active.success
                             : result.spec.required ? active.danger
                                                    : active.textMuted;

        gui.renderer().fillCircle({row.left + 6.f, row.center().y}, 3.f, colour);
        gui.text(result.spec.name, Rect{row.left + 18.f, row.top, row.right - 220.f, row.bottom},
                 active.text, 12.f, FontWeight::Regular);
        gui.text(result.spec.owner.empty() ? "—" : result.spec.owner,
                 Rect{row.right - 210.f, row.top, row.right - 90.f, row.bottom}, active.textMuted,
                 11.f, FontWeight::Regular);
        gui.text(result.resolved ? std::format("{:#x}", result.address) : "non résolue",
                 Rect{row.right - 80.f, row.top, row.right, row.bottom}, colour, 11.f,
                 FontWeight::Medium, TextAlign::Right);
    }

    gui.endScroll();
}

void ClickGui::drawHistoryPage(const Rect& rect) {
    Ui& gui = ui();
    Renderer& renderer = gui.renderer();
    const auto& active = theme();
    const Playtime& tracker = Playtime::get();

    gui.text("Historique", Rect{rect.left + 20.f, rect.top + 14.f, rect.right - 20.f, rect.top + 42.f},
             active.text, 17.f, FontWeight::Bold);

    const long long live = SessionStats::get().secondsPlayed();

    const std::array<std::pair<const char*, long long>, 3> totals{{
        {"Aujourd'hui", tracker.today() + live},
        {"7 derniers jours", tracker.thisWeek() + live},
        {"Total", tracker.total() + live},
    }};

    float cardX = rect.left + 20.f;
    for (const auto& [label, seconds] : totals) {
        const Rect card{cardX, rect.top + 50.f, cardX + 168.f, rect.top + 112.f};
        cardX += 180.f;

        renderer.fillRounded(card, active.surface.fade(0.6f), active.radius);
        gui.text(label, Rect{card.left + 14.f, card.top + 8.f, card.right - 14.f, card.top + 28.f},
                 active.textMuted, 11.f, FontWeight::Medium);
        gui.text(strings::formatDuration(seconds, false),
                 Rect{card.left + 14.f, card.top + 26.f, card.right - 14.f, card.bottom - 8.f},
                 active.liveAccent(), 18.f, FontWeight::Bold);
    }

    const Rect chart{rect.left + 20.f, rect.top + 124.f, rect.right - 20.f, rect.top + 216.f};
    renderer.fillRounded(chart, active.surface.fade(0.35f), active.radius);

    const auto days = tracker.lastDays(14);
    const long long peak = std::max<long long>(1, tracker.busiestDaySeconds());

    const float barWidth = (chart.width() - 24.f) / static_cast<float>(days.size());
    for (size_t i = 0; i < days.size(); ++i) {
        const float ratio = static_cast<float>(days[i].seconds) / static_cast<float>(peak);
        const float height = std::max(2.f, (chart.height() - 30.f) * ratio);

        const Rect bar{chart.left + 12.f + static_cast<float>(i) * barWidth + 3.f,
                       chart.bottom - 20.f - height,
                       chart.left + 12.f + static_cast<float>(i + 1) * barWidth - 3.f,
                       chart.bottom - 20.f};

        renderer.fillGradient(bar, active.liveAccentDeep(), active.liveAccent(), 90.f, 3.f);

        if (i % 2 == 0) {
            gui.text(days[i].date.substr(8), Rect{bar.left - 6.f, chart.bottom - 20.f,
                                                  bar.right + 6.f, chart.bottom - 2.f},
                     active.textMuted, 10.f, FontWeight::Regular, TextAlign::Center);
        }
    }

    gui.sectionHeader("Parties", Rect{rect.left + 20.f, rect.top + 228.f, rect.right - 20.f,
                                      rect.top + 248.f});

    const auto matches = Playtime::matches(120);
    const Rect view{rect.left + 20.f, rect.top + 254.f, rect.right - 20.f, rect.bottom - 16.f};

    if (matches.empty()) {
        gui.text("Aucune partie enregistrée pour l'instant.", view, active.textMuted, 12.5f,
                 FontWeight::Regular, TextAlign::Center);
        return;
    }

    const float offset =
        gui.beginScroll(UiId("match_list"), view, static_cast<float>(matches.size()) * 44.f);

    float y = view.top + offset;
    for (size_t i = 0; i < matches.size(); ++i) {
        const MatchRecord& match = matches[i];
        const Rect row{view.left, y, view.right - 8.f, y + 38.f};
        y += 44.f;

        if (row.bottom < view.top - 20.f || row.top > view.bottom + 20.f) continue;

        const UiId id("match", static_cast<int>(i));
        const float hover = gui.animate(id, row.contains(gui.mouse()));
        if (hover > 0.01f) {
            renderer.fillRounded(row, active.surfaceHover.fade(hover * 0.5f), active.radius);
        }

        const std::string server =
            match.server.empty() ? "Solo" : Privacy::get().maskAddress(match.server);

        gui.text(server, Rect{row.left + 12.f, row.top, row.left + 260.f, row.bottom}, active.text,
                 13.f, FontWeight::Medium);
        gui.text(strings::formatDuration(match.seconds),
                 Rect{row.left + 270.f, row.top, row.left + 360.f, row.bottom}, active.textMuted,
                 12.f, FontWeight::Regular);
        gui.text(std::format("{} / {}", match.kills, match.deaths),
                 Rect{row.left + 370.f, row.top, row.left + 440.f, row.bottom}, active.textMuted,
                 12.f, FontWeight::Regular);
        gui.text(std::format("{} FPS", static_cast<int>(match.averageFps)),
                 Rect{row.right - 160.f, row.top, row.right - 80.f, row.bottom},
                 active.liveAccent().fade(0.85f), 12.f, FontWeight::Medium, TextAlign::Right);
        gui.text(std::format("{} blocs", strings::formatThousands(
                                             static_cast<long long>(match.blocks))),
                 Rect{row.right - 70.f, row.top, row.right, row.bottom}, active.textMuted, 11.f,
                 FontWeight::Regular, TextAlign::Right);
    }

    gui.endScroll();
}

void ClickGui::drawFooter(const Rect& rect) {
    Ui& gui = ui();
    Renderer& renderer = gui.renderer();
    const auto& active = theme();

    renderer.fillRoundedCorners(rect, active.backgroundDeep.fade(0.4f), 0.f, 0.f,
                                active.panelRadius, active.panelRadius);
    renderer.line({rect.left, rect.top}, {rect.right, rect.top}, active.border.fade(0.7f), 1.f);

    const int activeCount = static_cast<int>(modules().enabled().size());

    gui.text(std::format("Velyx {}  ·  {} module(s) actif(s)  ·  profil « {} »", version::kString,
                         activeCount, profiles().current().name),
             Rect{rect.left + 20.f, rect.top, rect.right - 200.f, rect.bottom}, active.textMuted,
             11.5f, FontWeight::Regular);

    gui.text(std::format("{} FPS", static_cast<int>(Velyx::get().fps())),
             Rect{rect.right - 180.f, rect.top, rect.right - 20.f, rect.bottom},
             active.liveAccent(), 11.5f, FontWeight::SemiBold, TextAlign::Right);
}

}
