#include "ModuleManager.hpp"

#include <algorithm>
#include <atomic>

#include "core/Lang.hpp"
#include "core/Log.hpp"
#include "core/Strings.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/hook/hooks/UserInputHook.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Modules";

// Whether the game has a line of text open in front of the player — its chat, most of
// the time. A module bound to a plain letter has no business firing while a message is
// being written, and the letter has every business reaching the game.
//
// The game's own screen name is the answer whenever the pack carries the offset that
// reads it. Without it the chat is followed from the keys that open and close it,
// which is not proof but covers the way it actually happens.
std::atomic<bool> g_chatOpen{false};

bool typesACharacter(int key) {
    if (key >= '0' && key <= '9') return true;
    if (key >= 'A' && key <= 'Z') return true;
    if (key == VK_SPACE) return true;
    if (key >= VK_NUMPAD0 && key <= VK_DIVIDE) return true;
    if (key >= VK_OEM_1 && key <= VK_OEM_3) return true;
    if (key >= VK_OEM_4 && key <= VK_OEM_8) return true;
    return key == VK_OEM_102 || key == VK_OEM_PLUS || key == VK_OEM_COMMA ||
           key == VK_OEM_MINUS || key == VK_OEM_PERIOD;
}

// Ctrl and Alt are what tell a shortcut apart from a keystroke, so a bind carrying
// either still answers while the chat is open — as does anything that writes nothing
// at all, Insert and the function keys included.
bool wouldType(const Keybind& bind) {
    return !bind.ctrl && !bind.alt && typesACharacter(bind.key);
}

bool gameIsTyping() {
    // With an interface open the keyboard is the client's, and the game is not
    // reading a thing.
    if (WindowHook::captureInput()) return false;

    if (const sdk::Game& game = sdk::game(); game.screenKnown()) {
        // The game's own answer, and it puts the guess below back on its feet too.
        const bool chat = game.chatOnScreen();
        g_chatOpen.store(chat, std::memory_order_relaxed);
        return chat;
    }

    return g_chatOpen.load(std::memory_order_relaxed);
}

void followGameChat(const KeyEvent& event) {
    if (!event.down || event.repeat || WindowHook::captureInput()) return;

    if (event.key == VK_RETURN || event.key == VK_ESCAPE) {
        g_chatOpen.store(false, std::memory_order_relaxed);
        return;
    }

    const Keybind& chat = config().gameChatKey;
    if (chat.bound() && event.key == chat.key && !event.ctrl && !event.alt) {
        g_chatOpen.store(true, std::memory_order_relaxed);
    }
}

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

    // The other way into the chat, and the one a keyboard layout cannot spoil: whatever
    // key carries it, a typed slash opens the game's command line.
    events().on<CharEvent>(
        [](CharEvent& event) {
            if (event.codepoint == '/' && !WindowHook::captureInput()) {
                g_chatOpen.store(true, std::memory_order_relaxed);
            }
        },
        EventPriority::Low, this);

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

namespace {

// Whether the modifiers held match the ones the bind asks for. A bind on a modifier
// itself — FreeLook on Alt — is pressed with that modifier down by definition, so that
// one is not compared: asked to be Alt without Alt, it could never fire.
bool modifiersMatch(const Keybind& bind, const KeyEvent& event) {
    const bool onCtrl = bind.key == VK_CONTROL || bind.key == VK_LCONTROL || bind.key == VK_RCONTROL;
    const bool onShift = bind.key == VK_SHIFT || bind.key == VK_LSHIFT || bind.key == VK_RSHIFT;
    const bool onAlt = bind.key == VK_MENU || bind.key == VK_LMENU || bind.key == VK_RMENU;

    return (onCtrl || bind.ctrl == event.ctrl) && (onShift || bind.shift == event.shift) &&
           (onAlt || bind.alt == event.alt);
}

}

void ModuleManager::handleKey(KeyEvent& event) {
    followGameChat(event);

    // Only what is being pressed is held back. A release always goes through, or a
    // module held down when the chat opened would have no way of letting go.
    const bool typing = event.down && gameIsTyping();

    // A key the client answers to is the client's. Left uncancelled it also reaches
    // the game, which is how Ctrl+K opened the menu and made the game act on the
    // keystroke behind it.
    bool consumed = false;

    if (event.down && !event.repeat) {
        for (const Shortcut& shortcut : shortcuts_) {
            const Keybind& bind = *shortcut.bind;
            if (!bind.bound() || bind.key != event.key) continue;
            if (typing && wouldType(bind)) continue;
            if (!modifiersMatch(bind, event)) continue;

            Log::debug(kLog, "shortcut fired for key {}{}", event.ctrl ? "ctrl+" : "", event.key);
            shortcut.action();
            consumed = true;
        }
    }

    for (const auto& module : modules_) {
        const Keybind& bind = module->keybind();
        if (!bind.bound() || bind.key != event.key) continue;

        if (!modifiersMatch(bind, event)) continue;

        if (typing && wouldType(bind)) continue;

        if (safeMode_ && !module->essential()) continue;

        consumed = true;

        switch (bind.mode) {
            case Keybind::Mode::Toggle:
                if (event.down && !event.repeat) {
                    requestEnabled(module.get(), !module->enabled());
                }
                break;
            case Keybind::Mode::Hold:
                if (event.down) {
                    requestEnabled(module.get(), true);
                    const std::lock_guard<std::mutex> guard(pendingMutex_);
                    std::erase_if(pendingReleases_, [&](const auto& release) {
                        return release.first == module.get();
                    });
                } else {
                    const std::lock_guard<std::mutex> guard(pendingMutex_);
                    pendingReleases_.emplace_back(module.get(), event.key);
                }
                break;
            case Keybind::Mode::Once:
                if (event.down && !event.repeat) {
                    requestEnabled(module.get(), true);
                    requestEnabled(module.get(), false);
                }
                break;
        }
    }

    if (consumed) event.cancel();
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
    std::vector<std::pair<Module*, int>> releases;
    {
        const std::lock_guard<std::mutex> guard(pendingMutex_);
        pending.swap(pendingToggles_);
        releases.swap(pendingReleases_);
    }

    for (const auto& [module, enabled] : pending) {
        if (safeMode_ && !module->essential() && enabled) continue;
        module->setEnabled(enabled);
    }

    // A release the system still calls a press was the key repeating, not the player
    // letting go; the real release comes as a key-up of its own.
    for (const auto& [module, key] : releases) {
        if ((UserInputHook::realAsyncKeyState(key) & 0x8000) != 0) continue;
        module->setEnabled(false);
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
