#pragma once

#include <atomic>
#include <string>

#include "dll/module/Module.hpp"

namespace velyx {

class ModuleManager;

class ClickGui final : public Module {
public:
    ClickGui();

    void onEnable() override;
    void onDisable() override;

    // Opens the menu on the module list with the caret already in the search field,
    // which is what the search key does from anywhere in the game.
    void openOnSearch();

private:
    enum class Page { Modules, Themes, Profiles, Keybinds, Diagnostics, History, Captures };

    enum class Filter { All, Favourites, Movement, Hud, Render, Utility, Misc, Scripts,
                        Client, Combat };

    void onRender(RenderTopEvent& event);
    void onMouse(MouseEvent& event);
    void onKey(KeyEvent& event);
    void onChar(CharEvent& event);

    void drawHeader(const Rect& rect);
    void drawSidebar(const Rect& rect);
    void drawCategoryBar(const Rect& rect);
    void drawModuleGrid(const Rect& rect);
    void drawSettings(const Rect& rect);
    void drawFooter(const Rect& rect);

    void drawThemesPage(const Rect& rect);
    void drawProfilesPage(const Rect& rect);
    void drawKeybindsPage(const Rect& rect);
    void drawDiagnosticsPage(const Rect& rect);
    void drawHistoryPage(const Rect& rect);
    void drawCapturesPage(const Rect& rect);

    float drawSetting(const Rect& rect, Module& owner, Setting& setting, int index);
    [[nodiscard]] static float settingHeight(const Setting& setting);

    [[nodiscard]] std::vector<Module*> visibleModules() const;

    Rect window_;
    Page page_ = Page::Modules;
    Filter filter_ = Filter::All;
    bool showSettings_ = false;
    Module* selected_ = nullptr;

    std::string search_;

    // Set from the message thread by the search key, read and cleared by the frame:
    // page_ and the rest of the interface state belong to the render thread.
    std::atomic<bool> searchRequested_{false};
    bool focusSearch_ = false;
    std::string profileCode_;
    std::string newProfileName_;

    Animated open_{0.f, 14.f};
    Vec2 dragOffset_;
    bool dragging_ = false;
    bool showAdvanced_ = false;
};

}
