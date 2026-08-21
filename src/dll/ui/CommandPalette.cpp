#include "CommandPalette.hpp"

#include <windows.h>

#include <algorithm>

#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/config/ProfileManager.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"
#include "dll/ui/Ui.hpp"

namespace velyx {
namespace {

constexpr float kWidth = 620.f;
constexpr float kInputHeight = 52.f;
constexpr float kRowHeight = 46.f;
constexpr size_t kMaxRows = 8;

struct CustomCommand {
    std::string title;
    std::string subtitle;
    std::string category;
    std::function<void()> action;
};

std::vector<CustomCommand>& customCommands() {
    static std::vector<CustomCommand> commands;
    return commands;
}

}

CommandPalette::CommandPalette()
    : Module("command_palette", "Palette de commandes", ModuleCategory::Client,
             "Ctrl+K pour chercher n'importe quel module, réglage ou action.") {
    markEssential();

    keybind() = config().paletteKey;

    settings.header("Comportement");
    settings.toggle("closeAfterRun", "Fermer après une action", true);
    settings.toggle("closeAfterToggle", "Fermer après un changement d'état", false);
    settings.toggle("showState", "Afficher une pastille d'état", true);
    settings.toggle("searchSettings", "Inclure les réglages", true);
    settings.toggle("searchProfiles", "Inclure les profils et thèmes", true);

    on(&CommandPalette::onRender);
    on(&CommandPalette::onKey, EventPriority::First);
    on(&CommandPalette::onChar, EventPriority::First);
    on(&CommandPalette::onMouse, EventPriority::First);

    addKeywords({"palette", "commandes", "recherche", "ctrl+k"});
}

void CommandPalette::registerCommand(std::string title, std::string subtitle,
                                     std::function<void()> action, std::string category) {
    customCommands().push_back(
        CustomCommand{std::move(title), std::move(subtitle), std::move(category), std::move(action)});
}

void CommandPalette::onEnable() {
    query_.clear();
    resultsValid_ = false;
    highlighted_ = 0;
    open_.set(0.f);
    open_.to(1.f);
    WindowHook::setCaptureInput(true);
}

void CommandPalette::onDisable() {

    Module* gui = modules().find("clickgui");
    Module* editor = modules().find("hud_editor");
    if ((!gui || !gui->enabled()) && (!editor || !editor->enabled())) {
        WindowHook::setCaptureInput(false);
    }
}

std::vector<CommandPalette::Entry> CommandPalette::matches() const {
    std::vector<Entry> entries;

    const auto addAction = [&](std::string title, std::string subtitle, std::string category,
                               std::function<void()> action) {
        Entry entry;
        entry.title = std::move(title);
        entry.subtitle = std::move(subtitle);
        entry.category = std::move(category);
        entry.action = std::move(action);
        entries.push_back(std::move(entry));
    };

    addAction("Éditeur de HUD", "Placer les éléments à l'écran", "Client", [] {
        if (Module* editor = modules().find("hud_editor")) editor->setEnabled(true);
    });
    addAction("Enregistrer le profil", "Écrit les réglages sur le disque", "Profil",
              [] { profiles().saveCurrent(); Notifications::success("Profil enregistré"); });
    addAction("Point de restauration", "Sauvegarde l'état actuel du profil", "Profil", [] {
        profiles().snapshot("manuel");
        Notifications::success("Point de restauration créé");
    });
    addAction("Copier le code de partage", "Exporte le profil actuel", "Profil", [] {
        const std::string code = profiles().exportCode();
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            if (const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, code.size() + 1)) {
                if (void* memory = GlobalLock(handle)) {
                    std::memcpy(memory, code.c_str(), code.size() + 1);
                    GlobalUnlock(handle);
                    SetClipboardData(CF_TEXT, handle);
                }
            }
            CloseClipboard();
            Notifications::success("Code copié", "Collez-le pour partager votre configuration.");
        }
    });

    for (const CustomCommand& command : customCommands()) {
        addAction(command.title, command.subtitle, command.category, command.action);
    }

    for (const auto& module : modules().all()) {
        Module* raw = module.get();

        // The interface modules are closed whenever the palette is the thing on
        // screen, so reporting their state would always read "off" and mean nothing.
        // They get an action label instead.
        const bool isInterface = raw->id() == "clickgui" || raw->id() == "hud_editor" ||
                                 raw->id() == "command_palette" || raw->id() == "onboarding";

        Entry entry;
        entry.title = raw->name();
        entry.subtitle = isInterface ? raw->description() : std::string{};
        entry.category = categoryLabel(raw->category());
        entry.module = raw;
        entry.interfaceModule = isInterface;
        entry.keepOpen = !isInterface;
        entry.action = [raw, isInterface] {
            if (isInterface) {
                modules().requestEnabled(raw, true);
            } else {
                modules().requestEnabled(raw, !raw->enabled());
            }
        };
        entries.push_back(std::move(entry));
    }

    if (settings.value<bool>("searchProfiles", true)) {
        for (const std::string& name : profiles().names()) {
            Entry entry;
            entry.title = "Profil : " + name;
            entry.subtitle = "Basculer vers ce profil";
            entry.category = "Profil";
            entry.keepOpen = true;
            entry.action = [name] {
                profiles().switchTo(name);
                config().activeProfile = name;
                config().save();
            };
            entries.push_back(std::move(entry));
        }

        for (const std::string& name : ThemeManager::get().names()) {
            Entry entry;
            entry.title = "Thème : " + name;
            entry.subtitle = "Appliquer ce thème";
            entry.category = "Thème";
            entry.keepOpen = true;
            entry.action = [name] { ThemeManager::get().apply(name); };
            entries.push_back(std::move(entry));
        }
    }

    if (settings.value<bool>("searchSettings", true) && !query_.empty()) {
        for (const auto& hit : modules().search(query_, 30)) {
            if (!hit.setting) continue;

            Module* owner = hit.module;
            const std::string settingId = hit.setting->id;

            Entry entry;
            entry.title = hit.setting->label;
            entry.subtitle = owner->name();
            entry.category = "Réglage";
            entry.action = [owner, settingId] {

                if (Module* gui = modules().find("clickgui")) gui->setEnabled(true);
                Notifications::info(owner->name(), "Réglage « " + settingId + " »");
            };
            entries.push_back(std::move(entry));
        }
    }

    if (query_.empty()) {
        if (entries.size() > kMaxRows) entries.resize(kMaxRows);
        return entries;
    }

    for (Entry& entry : entries) {
        const auto titleScore = strings::fuzzyScore(query_, entry.title);
        const auto categoryScore = strings::fuzzyScore(query_, entry.category);

        entry.score = titleScore ? *titleScore + 30 : (categoryScore ? *categoryScore : -1);
    }

    std::erase_if(entries, [](const Entry& entry) { return entry.score < 0; });
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) { return a.score > b.score; });

    if (entries.size() > kMaxRows) entries.resize(kMaxRows);
    return entries;
}

const std::vector<CommandPalette::Entry>& CommandPalette::results() {
    if (!resultsValid_ || resultsQuery_ != query_) {
        results_ = matches();
        resultsQuery_ = query_;
        resultsValid_ = true;
    }

    // Only the state each row reports is refreshed; the order stays as the reader
    // last saw it, which is the whole point of caching it.
    for (Entry& entry : results_) {
        if (!entry.module || entry.interfaceModule) continue;
        entry.subtitle = entry.module->enabled() ? "Activé — appuyez pour désactiver"
                                                 : "Désactivé — appuyez pour activer";
    }

    return results_;
}

void CommandPalette::run(const Entry& entry) {
    if (entry.action) entry.action();

    const bool close = entry.keepOpen ? settings.value<bool>("closeAfterToggle", false)
                                      : settings.value<bool>("closeAfterRun", true);
    if (close) modules().requestEnabled(this, false);
}

// Cancelling stays here, synchronously: it is what keeps the keystroke out of the
// game. Only the state the renderer reads is deferred.
void CommandPalette::onKey(KeyEvent& event) {
    if (!event.down) {
        event.cancel();
        return;
    }
    if (event.key == keybind().key && event.ctrl == keybind().ctrl) return;

    {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        if (queuedKeys_.size() < kMaxQueuedInput) queuedKeys_.push_back(event);
    }

    event.cancel();
}

void CommandPalette::onChar(CharEvent& event) {
    if (event.codepoint >= 32 && event.codepoint != 127) {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        if (queuedChars_.size() < kMaxQueuedInput) queuedChars_.push_back(event.codepoint);
    }
    event.cancel();
}

void CommandPalette::processInput() {
    std::vector<KeyEvent> keys;
    std::vector<unsigned int> characters;
    {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        keys.swap(queuedKeys_);
        characters.swap(queuedChars_);
    }

    for (const KeyEvent& event : keys) {
        const auto& results = this->results();

        switch (event.key) {
            case VK_ESCAPE:
                modules().requestEnabled(this, false);
                break;
            case VK_UP:
                highlighted_ = std::max(0, highlighted_ - 1);
                break;
            case VK_DOWN:
                highlighted_ = std::min(static_cast<int>(results.size()) - 1, highlighted_ + 1);
                break;
            case VK_RETURN:
                if (highlighted_ >= 0 && highlighted_ < static_cast<int>(results.size())) {
                    run(results[static_cast<size_t>(highlighted_)]);
                }
                break;
            case VK_BACK:
                if (!query_.empty()) {
                    do {
                        query_.pop_back();
                    } while (!query_.empty() &&
                             (static_cast<unsigned char>(query_.back()) & 0xC0) == 0x80);
                    highlighted_ = 0;
                }
                break;
            default:
                break;
        }
    }

    for (const unsigned int codepoint : characters) {
        if (codepoint < 0x80) {
            query_.push_back(static_cast<char>(codepoint));
        } else {
            query_ += strings::toUtf8(std::wstring(1, static_cast<wchar_t>(codepoint)));
        }
        highlighted_ = 0;
    }
}

void CommandPalette::onMouse(MouseEvent& event) {
    ui().feedMouse(event);
    event.cancel();
}

void CommandPalette::onRender(RenderTopEvent& event) {
    processInput();

    Renderer& renderer = *event.renderer;
    const auto& active = theme();

    open_.speed = active.motion(18.f);
    open_.update(event.deltaSeconds);

    const float appear = ease(active.easing, open_.value);
    if (appear <= 0.001f) return;

    Ui& gui = ui();
    gui.beginFrame(renderer, event.deltaSeconds);

    renderer.fillRect(Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y),
                      active.backgroundDeep.withAlpha(0.5f * appear));

    const auto& results = this->results();

    const float width = std::min(kWidth, event.screenSize.x - 60.f);
    const float height = kInputHeight + static_cast<float>(results.size()) * kRowHeight +
                         (results.empty() ? 0.f : 10.f);

    const Rect box = Rect::fromSize((event.screenSize.x - width) * 0.5f,
                                    event.screenSize.y * 0.28f, width, height);

    renderer.pushScale({lerp(0.97f, 1.f, appear), lerp(0.97f, 1.f, appear)}, box.center());
    renderer.pushOpacity(appear);

    if (active.blur) renderer.blurBehind(box, active.blurSigma * 1.4f, active.panelRadius);
    gui.panel(box, active.panelRadius, false);

    const Rect input{box.left, box.top, box.right, box.top + kInputHeight};

    renderer.text("⌕", Rect{input.left + 18.f, input.top, input.left + 44.f, input.bottom},
                  active.liveAccent(),
                  [&] {
                      FontSpec spec;
                      spec.family = active.fontFamily;
                      spec.size = 18.f;
                      spec.align = TextAlign::Center;
                      spec.valign = TextVAlign::Middle;
                      return spec;
                  }());

    gui.text(query_.empty() ? "Rechercher un module, un réglage, un profil…" : query_,
             Rect{input.left + 50.f, input.top, input.right - 90.f, input.bottom},
             query_.empty() ? active.textMuted.fade(0.7f) : active.text, 15.f, FontWeight::Medium);

    gui.text("Échap", Rect{input.right - 80.f, input.top, input.right - 18.f, input.bottom},
             active.textMuted.fade(0.6f), 11.f, FontWeight::Medium, TextAlign::Right);

    if (!results.empty()) {
        renderer.line({box.left + 12.f, input.bottom}, {box.right - 12.f, input.bottom},
                      active.border.fade(0.6f), 1.f);
    }

    float y = input.bottom + 5.f;

    for (size_t i = 0; i < results.size(); ++i) {
        const Entry& entry = results[i];
        const Rect row{box.left + 6.f, y, box.right - 6.f, y + kRowHeight};
        y += kRowHeight;

        const UiId id("palette_row", static_cast<int>(i));
        const bool pressed = gui.hoverAndClick(id, row);
        if (gui.hovered(id)) highlighted_ = static_cast<int>(i);

        const bool isHighlighted = static_cast<int>(i) == highlighted_;

        const float glow = gui.animate(UiId("palette_glow", static_cast<int>(i)), isHighlighted, 22.f);
        if (glow > 0.01f) {
            renderer.fillRounded(row, active.liveAccent().fade(0.14f * glow), active.radius);
        }

        gui.text(entry.title, Rect{row.left + 16.f, row.top + 5.f, row.right - 120.f,
                                   row.top + kRowHeight * 0.55f},
                 active.text, 13.5f, FontWeight::SemiBold);
        gui.text(entry.subtitle, Rect{row.left + 16.f, row.top + kRowHeight * 0.5f,
                                      row.right - 120.f, row.bottom - 4.f},
                 active.textMuted, 11.f, FontWeight::Regular);

        // A state written only in grey prose at the end of a subtitle is not readable
        // at a glance, which is the one thing a palette row has to be.
        float categoryRight = row.right - 16.f;
        if (entry.module && !entry.interfaceModule && settings.value<bool>("showState", true)) {
            const bool on = entry.module->enabled();
            const Rect pill{row.right - 58.f, row.center().y - 9.f, row.right - 16.f,
                            row.center().y + 9.f};

            renderer.fillRounded(pill, on ? active.liveAccent().fade(0.22f)
                                          : active.surface.fade(0.6f),
                                 pill.height() * 0.5f);
            renderer.strokeRounded(pill, on ? active.liveAccent().fade(0.7f)
                                            : active.border.fade(0.7f),
                                   pill.height() * 0.5f, 1.f);
            gui.text(on ? "ON" : "OFF", pill, on ? active.liveAccent() : active.textMuted, 10.5f,
                     FontWeight::SemiBold, TextAlign::Center);

            categoryRight = pill.left - 10.f;
        }

        gui.text(entry.category, Rect{categoryRight - 94.f, row.top, categoryRight, row.bottom},
                 active.liveAccent().fade(0.8f), 11.f, FontWeight::Medium, TextAlign::Right);

        if (pressed) run(entry);
    }

    renderer.popOpacity();
    renderer.popTransform();
    gui.endFrame();
}

}
