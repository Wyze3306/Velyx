#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "dll/module/HudModule.hpp"
#include "dll/module/Module.hpp"

namespace velyx {

class HudEditor final : public Module {
public:
    HudEditor();

    void onEnable() override;
    void onDisable() override;

private:
    void onRender(RenderTopEvent& event);
    void onMouse(MouseEvent& event);
    void onKey(KeyEvent& event);

    void drawGrid(Renderer& renderer, Vec2 screenSize);
    void drawBottomBar(Renderer& renderer, Vec2 screenSize);
    void drawSelection(Renderer& renderer, HudModule& element);
    void drawGuides(Renderer& renderer, const Rect& moving, const std::vector<Rect>& others);

    [[nodiscard]] Vec2 snap(Vec2 position, const Rect& bounds, const std::vector<Rect>& others,
                            Vec2 screenSize) const;

    [[nodiscard]] std::vector<HudModule*> movingGroup() const;

    HudModule* selected_ = nullptr;
    HudModule* dragged_ = nullptr;
    HudModule* hovered_ = nullptr;
    Vec2 dragOffset_;
    std::vector<Vec2> guides_;

    std::unordered_set<std::string> locked_;
    Animated fade_{0.f, 14.f};
};

}
