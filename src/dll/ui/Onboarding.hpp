#pragma once

#include <atomic>
#include <string>

#include "dll/module/Module.hpp"

namespace velyx {

class Onboarding final : public Module {
public:
    Onboarding();

    void onEnable() override;
    void onDisable() override;

private:
    void onRender(RenderTopEvent& event);
    void onMouse(MouseEvent& event);
    void onKey(KeyEvent& event);

    void drawWelcome(const Rect& body);
    void drawStyle(const Rect& body);
    void drawUsage(const Rect& body);
    void drawKeys(const Rect& body);
    void drawDone(const Rect& body);

    void applyPreset(const std::string& preset);
    void finish();

    int step_ = 0;
    std::string preset_;
    std::atomic<bool> finishRequested_{false};
    Animated appear_{0.f, 14.f};
};

} // namespace velyx
