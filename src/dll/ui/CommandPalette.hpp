#pragma once

#include <functional>
#include <mutex>
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

        // Set for a module row, so its state can be read live without rebuilding —
        // and reordering — the list under the reader's cursor.
        Module* module = nullptr;
        bool interfaceModule = false;
        bool keepOpen = false;
    };

    void onRender(RenderTopEvent& event);
    void onKey(KeyEvent& event);
    void onChar(CharEvent& event);
    void onMouse(MouseEvent& event);

    // The query is read while the palette is drawn, so the message thread only
    // queues here and the render thread edits it in processInput().
    void processInput();

    [[nodiscard]] std::vector<Entry> matches() const;

    // The list is settled once per query rather than once per frame: toggling a
    // module from the palette must not move the row out from under the pointer.
    [[nodiscard]] const std::vector<Entry>& results();
    void run(const Entry& entry);

    std::string query_;
    std::string resultsQuery_;
    std::vector<Entry> results_;
    bool resultsValid_ = false;
    int highlighted_ = 0;
    Animated open_{0.f, 18.f};

    std::mutex inputMutex_;
    std::vector<KeyEvent> queuedKeys_;
    std::vector<unsigned int> queuedChars_;
};

}
