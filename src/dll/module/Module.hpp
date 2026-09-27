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
    Combat,
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

    // Anything that reaches past the game into the process or the machine: priority,
    // core affinity, timer resolution. Nothing a module does to the game itself.
    bool system = false;

    [[nodiscard]] bool any() const {
        return network || files || inputSynthesis || memoryPatch || clipboard || system;
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

    // An interface is on screen or it is not, and that is where the resemblance to a
    // module ends: its state belongs to the session, never to a profile, and nothing
    // that swaps a profile in or out is allowed to open or close it.
    [[nodiscard]] bool isInterfaceModule() const { return interfaceModule_; }

    [[nodiscard]] bool experimental() const { return experimental_; }

    // Why this module cannot do anything right now, or empty when it can. A module
    // that reads the game is not broken on a build with no signature pack — it is
    // waiting — and the difference is invisible unless the menu says it.
    [[nodiscard]] std::string inertReason() const;

    // Whether it can do anything at all right now. A module whose handlers have teeth
    // — one that cancels an event rather than merely reading it — has to ask this
    // itself: the menu saying it is waiting does not stop its subscriptions running,
    // and half a feature is often worse than none.
    [[nodiscard]] bool inert() const { return !inertReason().empty(); }

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
    void markInterfaceModule() {
        interfaceModule_ = true;
        essential_ = true;
    }
    void markExperimental() { experimental_ = true; }

    // Reads the game through the SDK, so it needs a pack that reaches the client
    // instance. Says nothing about which offsets: missing one of those makes a
    // reading show as unknown, not the whole module go quiet.
    void markNeedsGame() { needsGame_ = true; }

    // For a module that has nothing to draw without the list of actors. Kept apart
    // from needsGame: the pack can find the game and still not know what is in it,
    // and drawing nothing while saying nothing is the worst of both.
    void markNeedsEntities() {
        needsGame_ = true;
        needsEntities_ = true;
    }

    // Waits on something no hook emits yet. The name is what the menu shows, so it
    // is the thing a person would recognise: "the FOV hook", not "FovEvent".
    void markWaitingFor(std::string what) { waitingFor_ = std::move(what); }

    // Waits on a hook that exists but may not have found its footing — GameInput is
    // there or it is not, and which one is not known while the catalogue is being
    // built. So the answer is asked for every time the menu draws rather than stored.
    void markWaitingUnless(std::string what, std::function<bool()> ready) {
        waitingFor_ = std::move(what);
        waitingReady_ = std::move(ready);
    }

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
    bool interfaceModule_ = false;
    bool experimental_ = false;
    bool needsGame_ = false;
    bool needsEntities_ = false;
    std::string waitingFor_;
    std::function<bool()> waitingReady_;
};

}
