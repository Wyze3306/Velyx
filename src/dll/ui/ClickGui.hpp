#pragma once

#include <string>

#include "dll/module/Module.hpp"

namespace velyx {

class ModuleManager;

class ClickGui final : public Module {
public:
    ClickGui();

    void onEnable() override;
    void onDisable() override;

private:
    enum class Page {
        Favourites,
        Movement,
        Hud,
        Render,
        Utility,
        Misc,
        Scripts,
        Themes,
        Profiles,
        Keybinds,
        Diagnostics,
        History,
    };

    void onRender(RenderTopEvent& event);
    void onMouse(MouseEvent& event);
    void onKey(KeyEvent& event);
    void onChar(CharEvent& event);

    void drawHeader(const Rect& rect);
    void drawSidebar(const Rect& rect);
    void drawModuleList(const Rect& rect);
    void drawSettings(const Rect& rect);
    void drawFooter(const Rect& rect);

    void drawThemesPage(const Rect& rect);
    void drawProfilesPage(const Rect& rect);
    void drawKeybindsPage(const Rect& rect);
    void drawDiagnosticsPage(const Rect& rect);
    void drawHistoryPage(const Rect& rect);

    float drawSetting(const Rect& rect, Module& owner, Setting& setting, int index);
    [[nodiscard]] static float settingHeight(const Setting& setting);

    [[nodiscard]] std::vector<Module*> visibleModules() const;

    Rect window_;
    Page page_ = Page::Favourites;
    Module* selected_ = nullptr;

    std::string search_;
    std::string profileCode_;
    std::string newProfileName_;

    Animated open_{0.f, 14.f};
    Vec2 dragOffset_;
    bool dragging_ = false;
    bool showAdvanced_ = false;
};

}
