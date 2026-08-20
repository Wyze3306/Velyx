#pragma once

#include <memory>
#include <string>
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

    struct SearchHit {
        Module* module = nullptr;
        const Setting* setting = nullptr;
        int score = 0;
    };
    [[nodiscard]] std::vector<SearchHit> search(std::string_view query, size_t limit = 24) const;

    void handleKey(const KeyEvent& event);

    void setSafeMode(bool safeMode);
    [[nodiscard]] bool safeMode() const { return safeMode_; }

    [[nodiscard]] nlohmann::json save() const;
    void load(const nlohmann::json& json);

    void disableAll();

private:
    ModuleManager() = default;

    std::vector<std::unique_ptr<Module>> modules_;
    bool safeMode_ = false;
    bool initialised_ = false;
};

void registerBuiltInModules(ModuleManager& manager);

inline ModuleManager& modules() { return ModuleManager::get(); }

}
