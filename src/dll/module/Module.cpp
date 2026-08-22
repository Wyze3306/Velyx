#include "Module.hpp"

#include "core/Log.hpp"

namespace velyx {
namespace {
constexpr const char* kLog = "Module";
}

const char* categoryName(ModuleCategory category) {
    switch (category) {
        case ModuleCategory::Movement: return "movement";
        case ModuleCategory::Combat:   return "combat";
        case ModuleCategory::Hud:      return "hud";
        case ModuleCategory::Render:   return "render";
        case ModuleCategory::Utility:  return "utility";
        case ModuleCategory::Misc:     return "misc";
        case ModuleCategory::Client:   return "client";
        case ModuleCategory::Script:   return "script";
    }
    return "misc";
}

const char* categoryLabel(ModuleCategory category) {
    switch (category) {
        case ModuleCategory::Movement: return "Movement";
        case ModuleCategory::Combat:   return "Combat";
        case ModuleCategory::Hud:      return "HUD";
        case ModuleCategory::Render:   return "Render";
        case ModuleCategory::Utility:  return "Utility";
        case ModuleCategory::Misc:     return "Misc";
        case ModuleCategory::Client:   return "Client";
        case ModuleCategory::Script:   return "Scripts";
    }
    return "Misc";
}

std::vector<std::string> ModulePermissions::describe() const {
    std::vector<std::string> list;
    if (network) list.emplace_back("Network");
    if (files) list.emplace_back("Files");
    if (inputSynthesis) list.emplace_back("Synthetic input");
    if (memoryPatch) list.emplace_back("Game memory");
    if (clipboard) list.emplace_back("Clipboard");
    if (system) list.emplace_back("System");
    return list;
}

Module::Module(std::string id, std::string name, ModuleCategory category, std::string description)
    : id_(std::move(id)),
      name_(std::move(name)),
      description_(std::move(description)),
      category_(category) {}

Module::~Module() {

    events().offOwner(this);
}

void Module::addKeywords(std::vector<std::string> keywords) {
    keywords_.insert(keywords_.end(), std::make_move_iterator(keywords.begin()),
                     std::make_move_iterator(keywords.end()));
}

void Module::subscribe() {
    for (const auto& factory : subscriptions_) factory();
}

void Module::unsubscribe() { events().offOwner(this); }

void Module::setEnabled(bool enabled, bool byUser) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;

    if (enabled_) {
        subscribe();
        onEnable();
    } else {
        onDisable();
        unsubscribe();
    }

    ModuleToggleEvent event;
    event.module = this;
    event.enabled = enabled_;
    event.byUser = byUser;
    events().emit(event);

    Log::debug(kLog, "{} {}", name_, enabled_ ? "enabled" : "disabled");
}

nlohmann::json Module::save() const {
    nlohmann::json json;
    if (!interfaceModule_) json["enabled"] = enabled_;
    json["favourite"] = favourite_;
    json["keybind"] = toJson(SettingValue{keybind_});
    json["settings"] = settings.save();
    return json;
}

void Module::load(const nlohmann::json& json) {
    if (!json.is_object()) return;

    favourite_ = json.value("favourite", favourite_);

    if (json.contains("keybind")) {
        // std::get throws when the stored value is not the alternative asked for, and
        // a profile written by an older build is exactly that case.
        const SettingValue value = fromJson(json["keybind"], SettingValue{keybind_});
        if (const Keybind* bind = std::get_if<Keybind>(&value)) keybind_ = *bind;
    }

    if (json.contains("settings")) settings.load(json["settings"]);

    // A profile written by an older build still carries the interfaces' own state.
    // Applying it would close the menu the moment it is used to switch profile.
    if (!interfaceModule_ && json.contains("enabled") && json["enabled"].is_boolean()) {
        setEnabled(json["enabled"].get<bool>(), false);
    }
}

}
