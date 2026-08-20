#pragma once

#include <functional>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "dll/event/EventBus.hpp"
#include "dll/event/Events.hpp"
#include "dll/module/Setting.hpp"

namespace velyx {

enum class ModuleCategory {
    Movement,
    Hud,
    Render,
    Utility,
    Misc,
    Client,
    Script,
};

const char* categoryName(ModuleCategory category);
const char* categoryLabel(ModuleCategory category);

struct ModulePermissions {
    bool network = false;
    bool files = false;
    bool inputSynthesis = false;
    bool memoryPatch = false;
    bool clipboard = false;

    [[nodiscard]] bool any() const {
        return network || files || inputSynthesis || memoryPatch || clipboard;
    }
    [[nodiscard]] std::vector<std::string> describe() const;
};

class Module {
public:
    Module(std::string id, std::string name, ModuleCategory category, std::string description);
    virtual ~Module();

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void setEnabled(bool enabled, bool byUser = true);
    void toggle() { setEnabled(!enabled_); }
    [[nodiscard]] bool enabled() const { return enabled_; }

    virtual void onEnable() {}
    virtual void onDisable() {}

    virtual void onRegistered() {}

    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& description() const { return description_; }
    [[nodiscard]] ModuleCategory category() const { return category_; }

    [[nodiscard]] const std::vector<std::string>& keywords() const { return keywords_; }
    void addKeywords(std::vector<std::string> keywords);

    [[nodiscard]] bool favourite() const { return favourite_; }
    void setFavourite(bool favourite) { favourite_ = favourite; }

    [[nodiscard]] bool essential() const { return essential_; }

    [[nodiscard]] bool experimental() const { return experimental_; }

    [[nodiscard]] const ModulePermissions& permissions() const { return permissions_; }

    Keybind& keybind() { return keybind_; }
    [[nodiscard]] const Keybind& keybind() const { return keybind_; }

    Settings settings;

    [[nodiscard]] nlohmann::json save() const;
    void load(const nlohmann::json& json);

protected:

    // Registers a subscription factory rather than the subscription itself, so it
    // only exists while the module is enabled and a disabled module costs nothing.
    template <typename E, typename T>
    void on(void (T::*method)(E&), EventPriority priority = EventPriority::Normal) {
        subscriptions_.push_back([this, method, priority] {
            events().on<E>(static_cast<T*>(this), method, priority);
        });
    }

    template <typename E, typename T>
    void always(void (T::*method)(E&), EventPriority priority = EventPriority::Normal) {
        events().on<E>(static_cast<T*>(this), method, priority);
    }

    void markEssential() { essential_ = true; }
    void markExperimental() { experimental_ = true; }
    ModulePermissions& mutablePermissions() { return permissions_; }

private:
    void subscribe();
    void unsubscribe();

    std::string id_;
    std::string name_;
    std::string description_;
    ModuleCategory category_;
    std::vector<std::string> keywords_;

    Keybind keybind_;
    ModulePermissions permissions_;

    std::vector<std::function<void()>> subscriptions_;

    bool enabled_ = false;
    bool favourite_ = false;
    bool essential_ = false;
    bool experimental_ = false;
};

}
