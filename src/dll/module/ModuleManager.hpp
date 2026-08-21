#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <json/json.hpp>

#include "dll/module/HudModule.hpp"
#include "dll/module/Module.hpp"

namespace velyx {

class ModuleManager {
public:
    static ModuleManager& get();

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        auto module = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = module.get();
        modules_.push_back(std::move(module));
        return raw;
    }

    void initialize();
    void shutdown();

    [[nodiscard]] Module* find(std::string_view id) const;

    template <typename T>
    [[nodiscard]] T* get() const {
        for (const auto& module : modules_) {
            if (auto* typed = dynamic_cast<T*>(module.get())) return typed;
        }
        return nullptr;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Module>>& all() const { return modules_; }
    [[nodiscard]] std::vector<Module*> byCategory(ModuleCategory category) const;
    [[nodiscard]] std::vector<Module*> favourites() const;
    [[nodiscard]] std::vector<HudModule*> huds() const;
    [[nodiscard]] std::vector<Module*> enabled() const;

    // Whether anything is still on screen. Each interface releases the cursor as it
    // closes, and must not do so while another one is up.
    [[nodiscard]] bool anyInterfaceOpen() const;

    // Which side of that line a call means: everything except the interfaces, or the
    // interfaces alone.
    enum class Interfaces { Leave, Only, Include };

    struct SearchHit {
        Module* module = nullptr;
        const Setting* setting = nullptr;
        int score = 0;
    };
    [[nodiscard]] std::vector<SearchHit> search(std::string_view query, size_t limit = 24) const;

    void handleKey(const KeyEvent& event);

    // A key that does something other than toggle a module — the menu's search key is
    // the only one so far. The bind is read through the pointer at every keystroke, so
    // rebinding it in the Raccourcis page takes effect straight away.
    void addShortcut(const Keybind* bind, std::function<void()> action);

    // Keybinds arrive on the game's message thread, where onEnable/onDisable would
    // run — profile writes included — while the render thread walks these same
    // modules. The change is queued here and applied between frames instead.
    void requestEnabled(Module* module, bool enabled);
    void applyPendingToggles();

    void setSafeMode(bool safeMode);
    [[nodiscard]] bool safeMode() const { return safeMode_; }

    // A profile carries the modules and nothing else. What the interfaces themselves
    // look like — where the menu sits, whether it dims the game, the editor's grid —
    // is the same whichever profile is active, so it is saved apart from them.
    [[nodiscard]] nlohmann::json save(Interfaces which = Interfaces::Leave) const;
    void load(const nlohmann::json& json, Interfaces which = Interfaces::Leave);

    // Switching profile takes every module down before it brings the new set up. The
    // interfaces are not part of that set: the menu doing this is the one asking.
    void disableAll(Interfaces interfaces = Interfaces::Leave);

private:
    ModuleManager() = default;

    std::vector<std::unique_ptr<Module>> modules_;
    bool safeMode_ = false;
    bool initialised_ = false;

    struct Shortcut {
        const Keybind* bind = nullptr;
        std::function<void()> action;
    };
    std::vector<Shortcut> shortcuts_;

    std::mutex pendingMutex_;
    std::vector<std::pair<Module*, bool>> pendingToggles_;
};

void registerBuiltInModules(ModuleManager& manager);

inline ModuleManager& modules() { return ModuleManager::get(); }

}
