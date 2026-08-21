#include "ModuleManager.hpp"

#include <algorithm>

#include "core/Lang.hpp"
#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx {
namespace {
constexpr const char* kLog = "Modules";
}

ModuleManager& ModuleManager::get() {
    static ModuleManager instance;
    return instance;
}

void ModuleManager::initialize() {
    if (initialised_) return;
    initialised_ = true;

    registerBuiltInModules(*this);

    for (const auto& module : modules_) module->onRegistered();

    int hudCount = 0;
    for (const auto& module : modules_) {
        if (dynamic_cast<HudModule*>(module.get())) ++hudCount;
    }

    // Nothing dispatched keybinds before this: handleKey() existed but was never
    // reached, so no module could be toggled from the keyboard. Low priority keeps
    // the open interface first — it cancels the keys it consumes.
    events().on<KeyEvent>([this](KeyEvent& event) { handleKey(event); }, EventPriority::Low,
                          this);

    Log::info(kLog, "registered {} modules ({} HUD elements)", modules_.size(), hudCount);
}

void ModuleManager::shutdown() {
    disableAll(Interfaces::Include);
    shortcuts_.clear();
    {
        const std::lock_guard<std::mutex> guard(pendingMutex_);
        pendingToggles_.clear();
    }
    modules_.clear();
    initialised_ = false;
}

Module* ModuleManager::find(std::string_view id) const {
    const auto it = std::ranges::find_if(modules_, [&](const std::unique_ptr<Module>& module) {
        return module->id() == id;
    });
    return it == modules_.end() ? nullptr : it->get();
}

std::vector<Module*> ModuleManager::byCategory(ModuleCategory category) const {
    std::vector<Module*> result;
    for (const auto& module : modules_) {
        if (module->category() == category) result.push_back(module.get());
    }

    std::ranges::sort(result, [](const Module* a, const Module* b) { return a->name() < b->name(); });
    return result;
}

std::vector<Module*> ModuleManager::favourites() const {
    std::vector<Module*> result;
    for (const auto& module : modules_) {
        if (module->favourite()) result.push_back(module.get());
    }
    return result;
}

std::vector<HudModule*> ModuleManager::huds() const {
    std::vector<HudModule*> result;
    for (const auto& module : modules_) {
        if (auto* hud = dynamic_cast<HudModule*>(module.get())) result.push_back(hud);
    }
    return result;
}

std::vector<Module*> ModuleManager::enabled() const {
    std::vector<Module*> result;
    for (const auto& module : modules_) {
        if (module->enabled()) result.push_back(module.get());
    }
    return result;
}

std::vector<ModuleManager::SearchHit> ModuleManager::search(std::string_view query,
                                                            size_t limit) const {
    std::vector<SearchHit> hits;
    if (query.empty()) return hits;

    for (const auto& module : modules_) {
        int best = -1;

        // Both languages are searched: the names live in the source in English, but
        // what the reader has in front of them is whatever the table says.
        for (const std::string_view name : {std::string_view(module->name()), tr(module->name())}) {
            if (const auto score = strings::fuzzyScore(query, name)) {
                best = std::max(best, *score + 40);
            }
        }
        for (const std::string_view text :
             {std::string_view(module->description()), tr(module->description())}) {
            if (const auto score = strings::fuzzyScore(query, text)) {
                best = std::max(best, *score - 10);
            }
        }
        for (const std::string& keyword : module->keywords()) {
            if (const auto score = strings::fuzzyScore(query, keyword)) {
                best = std::max(best, *score + 15);
            }
        }

        if (best >= 0) hits.push_back(SearchHit{module.get(), nullptr, best});

        for (const Setting& setting : module->settings.list()) {
            if (!setting.hasValue() || setting.label.empty()) continue;

            int settingScore = -1;
            for (const std::string_view label :
                 {std::string_view(setting.label), tr(setting.label)}) {
                if (const auto score = strings::fuzzyScore(query, label)) {
                    settingScore = std::max(settingScore, *score);
                }
            }
            for (const std::string& keyword : setting.keywords) {
                if (const auto score = strings::fuzzyScore(query, keyword)) {
                    settingScore = std::max(settingScore, *score);
                }
            }

            if (settingScore >= 0) {
                hits.push_back(SearchHit{module.get(), &setting, settingScore});
            }
        }
    }

    std::ranges::sort(hits, [](const SearchHit& a, const SearchHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.module->name() < b.module->name();
    });

    if (hits.size() > limit) hits.resize(limit);
    return hits;
}

bool ModuleManager::anyInterfaceOpen() const {
    return std::ranges::any_of(modules_, [](const std::unique_ptr<Module>& module) {
        return module->isInterfaceModule() && module->enabled();
    });
}

void ModuleManager::handleKey(const KeyEvent& event) {
    if (event.down && !event.repeat) {
        for (const Shortcut& shortcut : shortcuts_) {
            const Keybind& bind = *shortcut.bind;
            if (!bind.bound() || bind.key != event.key) continue;
            if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) {
                continue;
            }

            shortcut.action();
        }
    }

    for (const auto& module : modules_) {
        const Keybind& bind = module->keybind();
        if (!bind.bound() || bind.key != event.key) continue;

        if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) continue;

        if (safeMode_ && !module->essential()) continue;

        switch (bind.mode) {
            case Keybind::Mode::Toggle:
                if (event.down && !event.repeat) {
                    requestEnabled(module.get(), !module->enabled());
                }
                break;
            case Keybind::Mode::Hold:
                requestEnabled(module.get(), event.down);
                break;
            case Keybind::Mode::Once:
                if (event.down && !event.repeat) {
                    requestEnabled(module.get(), true);
                    requestEnabled(module.get(), false);
                }
                break;
        }
    }
}

void ModuleManager::addShortcut(const Keybind* bind, std::function<void()> action) {
    if (!bind || !action) return;
    shortcuts_.push_back(Shortcut{bind, std::move(action)});
}

void ModuleManager::requestEnabled(Module* module, bool enabled) {
    if (!module) return;
    const std::lock_guard<std::mutex> guard(pendingMutex_);
    pendingToggles_.emplace_back(module, enabled);
}

void ModuleManager::applyPendingToggles() {
    std::vector<std::pair<Module*, bool>> pending;
    {
        const std::lock_guard<std::mutex> guard(pendingMutex_);
        if (pendingToggles_.empty()) return;
        pending.swap(pendingToggles_);
    }

    for (const auto& [module, enabled] : pending) {
        if (safeMode_ && !module->essential() && enabled) continue;
        module->setEnabled(enabled);
    }
}

void ModuleManager::setSafeMode(bool safeMode) {
    safeMode_ = safeMode;
    if (!safeMode_) return;

    int disabled = 0;
    for (const auto& module : modules_) {
        if (module->essential() || !module->enabled()) continue;
        module->setEnabled(false, false);
        ++disabled;
    }

    Log::warn(kLog, "safe mode: disabled {} module(s)", disabled);
}

void ModuleManager::disableAll(Interfaces interfaces) {
    for (const auto& module : modules_) {
        if (!module->enabled()) continue;
        if (interfaces == Interfaces::Leave && module->isInterfaceModule()) continue;

        module->setEnabled(false, false);
    }
}

namespace {

bool inScope(const Module& module, ModuleManager::Interfaces which) {
    switch (which) {
        case ModuleManager::Interfaces::Leave:   return !module.isInterfaceModule();
        case ModuleManager::Interfaces::Only:    return module.isInterfaceModule();
        case ModuleManager::Interfaces::Include: return true;
    }
    return true;
}

}

nlohmann::json ModuleManager::save(Interfaces which) const {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& module : modules_) {
        if (!inScope(*module, which)) continue;
        json[module->id()] = module->save();
    }
    return json;
}

void ModuleManager::load(const nlohmann::json& json, Interfaces which) {
    if (!json.is_object()) return;

    for (const auto& module : modules_) {
        if (!inScope(*module, which)) continue;

        const auto it = json.find(module->id());
        if (it == json.end()) continue;

        if (safeMode_ && !module->essential()) {

            nlohmann::json filtered = *it;
            filtered.erase("enabled");
            module->load(filtered);
            continue;
        }

        module->load(*it);
    }
}

}
