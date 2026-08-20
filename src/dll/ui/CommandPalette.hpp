#pragma once

#include <functional>
#include <string>
#include <vector>

#include "dll/module/Module.hpp"

namespace velyx {

class CommandPalette final : public Module {
public:
    CommandPalette();

    static void registerCommand(std::string title, std::string subtitle,
                                std::function<void()> action, std::string category = "Action");

    void onEnable() override;
    void onDisable() override;

private:
    struct Entry {
        std::string title;
        std::string subtitle;
        std::string category;
        std::function<void()> action;
        int score = 0;
    };

    void onRender(RenderTopEvent& event);
    void onKey(KeyEvent& event);
    void onChar(CharEvent& event);
    void onMouse(MouseEvent& event);

    [[nodiscard]] std::vector<Entry> matches() const;
    void run(const Entry& entry);

    std::string query_;
    int highlighted_ = 0;
    Animated open_{0.f, 18.f};
};

}
