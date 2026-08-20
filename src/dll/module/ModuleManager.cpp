#include "ModuleManager.hpp"

#include <algorithm>

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

    Log::info(kLog, "registered {} modules ({} HUD elements)", modules_.size(), hudCount);
}

void ModuleManager::shutdown() {
    disableAll();
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

        if (const auto score = strings::fuzzyScore(query, module->name())) {
            best = std::max(best, *score + 40);
        }
        if (const auto score = strings::fuzzyScore(query, module->description())) {
            best = std::max(best, *score - 10);
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
            if (const auto score = strings::fuzzyScore(query, setting.label)) {
                settingScore = std::max(settingScore, *score);
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

void ModuleManager::handleKey(const KeyEvent& event) {
    for (const auto& module : modules_) {
        const Keybind& bind = module->keybind();
        if (!bind.bound() || bind.key != event.key) continue;

        if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) continue;

        if (safeMode_ && !module->essential()) continue;

        switch (bind.mode) {
            case Keybind::Mode::Toggle:
                if (event.down && !event.repeat) module->toggle();
                break;
            case Keybind::Mode::Hold:
                module->setEnabled(event.down);
                break;
            case Keybind::Mode::Once:
                if (event.down && !event.repeat) {
                    module->setEnabled(true);
                    module->setEnabled(false);
                }
                break;
        }
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

void ModuleManager::disableAll() {
    for (const auto& module : modules_) {
        if (module->enabled()) module->setEnabled(false, false);
    }
}

nlohmann::json ModuleManager::save() const {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& module : modules_) json[module->id()] = module->save();
    return json;
}

void ModuleManager::load(const nlohmann::json& json) {
    if (!json.is_object()) return;

    for (const auto& module : modules_) {
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
