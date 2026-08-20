#include "HudEditor.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "dll/Velyx.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/config/ProfileManager.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/ui/Theme.hpp"
#include "dll/ui/Ui.hpp"

namespace velyx {
namespace {

constexpr float kSnapDistance = 8.f;
constexpr float kToolbarHeight = 52.f;

}

HudEditor::HudEditor()
    : Module("hud_editor", "Éditeur de HUD", ModuleCategory::Client,
             "Placement libre, grille, alignement et groupes.") {
    markEssential();

    keybind() = config().hudEditorKey;

    settings.header("Grille");
    settings.toggle("showGrid", "Afficher la grille", true);
    settings.intSlider("gridSize", "Pas de la grille", 8, 2, 64, "", " px");
    settings.toggle("snapToGrid", "Aimanter à la grille", true);
    settings.toggle("snapToEdges", "Aimanter aux autres éléments", true);

    settings.header("Aide au placement");
    settings.toggle("showBounds", "Encadrer les éléments", true);
    settings.toggle("showNames", "Afficher les noms", true);
    settings.toggle("dimGame", "Assombrir le jeu", true);

    on(&HudEditor::onRender);
    on(&HudEditor::onMouse, EventPriority::First);
    on(&HudEditor::onKey, EventPriority::First);

    addKeywords({"hud", "éditeur", "placement", "grille", "layout"});
}

void HudEditor::onEnable() {
    WindowHook::setCaptureInput(true);
    fade_.set(0.f);
    fade_.to(1.f);
}

void HudEditor::onDisable() {
    WindowHook::setCaptureInput(false);
    dragged_ = nullptr;
    profiles().saveCurrent();
}

std::vector<HudModule*> HudEditor::movingGroup() const {
    std::vector<HudModule*> result;
    if (!dragged_) return result;

    result.push_back(dragged_);

    const std::string group = dragged_->group();
    if (group.empty()) return result;

    for (HudModule* element : modules().huds()) {
        if (element == dragged_ || !element->enabled()) continue;
        if (element->group() == group) result.push_back(element);
    }

    return result;
}

void HudEditor::onMouse(MouseEvent& event) {
    ui().feedMouse(event);

    if (event.action == MouseAction::Press && event.button == MouseButton::Left) {

        auto huds = modules().huds();
        for (auto it = huds.rbegin(); it != huds.rend(); ++it) {
            HudModule* element = *it;
            if (!element->enabled()) continue;
            if (!element->bounds().contains(event.position)) continue;

            dragged_ = element;
            dragOffset_ = event.position - element->bounds().topLeft();
            break;
        }
    } else if (event.action == MouseAction::Release && event.button == MouseButton::Left) {
        if (dragged_) {

            for (HudModule* element : movingGroup()) {
                element->reanchorToNearestEdge(Velyx::get().screenSize());
            }
            dragged_ = nullptr;
            guides_.clear();
        }
    }

    event.cancel();
}

void HudEditor::onKey(KeyEvent& event) {
    if (event.key == keybind().key) return;

    if (event.down && event.key == VK_ESCAPE) {
        setEnabled(false);
        event.cancel();
        return;
    }

    HudModule* target = dragged_ ? dragged_ : hovered_;
    if (event.down && target) {
        const float step = event.shift
                               ? static_cast<float>(settings.value<int>("gridSize", 8))
                               : 1.f;

        Vec2 delta;
        switch (event.key) {
            case VK_LEFT:  delta.x = -step; break;
            case VK_RIGHT: delta.x = step;  break;
            case VK_UP:    delta.y = -step; break;
            case VK_DOWN:  delta.y = step;  break;
            default: break;
        }

        if (delta.x != 0.f || delta.y != 0.f) {
            const Vec2 screen = Velyx::get().screenSize();
            target->moveTo(target->bounds().topLeft() + delta + Vec2{0.f, 0.f}, screen);
            target->reanchorToNearestEdge(screen);
            event.cancel();
            return;
        }
    }

    ui().feedKey(event);
    event.cancel();
}

void HudEditor::drawGrid(Renderer& renderer, Vec2 screenSize) {
    if (!settings.value<bool>("showGrid", true)) return;

    const auto& active = theme();
    const float step = static_cast<float>(settings.value<int>("gridSize", 8));
    if (step < 2.f) return;

    const Color fine = active.border.fade(0.18f);
    const Color coarse = active.border.fade(0.4f);

    int index = 0;
    for (float x = 0.f; x < screenSize.x; x += step, ++index) {
        renderer.line({x, 0.f}, {x, screenSize.y}, index % 8 == 0 ? coarse : fine, 1.f);
    }

    index = 0;
    for (float y = 0.f; y < screenSize.y; y += step, ++index) {
        renderer.line({0.f, y}, {screenSize.x, y}, index % 8 == 0 ? coarse : fine, 1.f);
    }

    renderer.line({screenSize.x * 0.5f, 0.f}, {screenSize.x * 0.5f, screenSize.y},
                  active.liveAccent().fade(0.25f), 1.f);
    renderer.line({0.f, screenSize.y * 0.5f}, {screenSize.x, screenSize.y * 0.5f},
                  active.liveAccent().fade(0.25f), 1.f);
}

Vec2 HudEditor::snap(Vec2 position, const Rect& bounds, const std::vector<Rect>& others,
                     Vec2 screenSize) const {
    Vec2 result = position;

    if (settings.value<bool>("snapToGrid", true)) {
        const float step = static_cast<float>(settings.value<int>("gridSize", 8));
        if (step >= 2.f) {
            result.x = std::round(result.x / step) * step;
            result.y = std::round(result.y / step) * step;
        }
    }

    if (!settings.value<bool>("snapToEdges", true)) return result;

    const float width = bounds.width();
    const float height = bounds.height();

    std::vector<float> verticals{0.f, screenSize.x * 0.5f - width * 0.5f, screenSize.x - width};
    std::vector<float> horizontals{0.f, screenSize.y * 0.5f - height * 0.5f, screenSize.y - height};

    for (const Rect& other : others) {
        verticals.push_back(other.left);
        verticals.push_back(other.right);
        verticals.push_back(other.right - width);
        verticals.push_back(other.left - width);
        verticals.push_back(other.center().x - width * 0.5f);

        horizontals.push_back(other.top);
        horizontals.push_back(other.bottom);
        horizontals.push_back(other.bottom - height);
        horizontals.push_back(other.top - height);
        horizontals.push_back(other.center().y - height * 0.5f);
    }

    for (const float candidate : verticals) {
        if (std::abs(result.x - candidate) <= kSnapDistance) {
            result.x = candidate;
            break;
        }
    }
    for (const float candidate : horizontals) {
        if (std::abs(result.y - candidate) <= kSnapDistance) {
            result.y = candidate;
            break;
        }
    }

    return result;
}

// Guides are drawn only for edges that actually line up.
void HudEditor::drawGuides(Renderer& renderer, const Rect& moving, const std::vector<Rect>& others) {
    const auto& active = theme();
    const Color guide = active.liveAccent().fade(0.7f);

    for (const Rect& other : others) {
        const auto nearly = [](float a, float b) { return std::abs(a - b) < 0.75f; };

        if (nearly(moving.left, other.left) || nearly(moving.right, other.right) ||
            nearly(moving.center().x, other.center().x)) {
            const float x = nearly(moving.left, other.left)     ? moving.left
                            : nearly(moving.right, other.right) ? moving.right
                                                                : moving.center().x;
            renderer.line({x, std::min(moving.top, other.top) - 20.f},
                          {x, std::max(moving.bottom, other.bottom) + 20.f}, guide, 1.f);
        }

        if (nearly(moving.top, other.top) || nearly(moving.bottom, other.bottom) ||
            nearly(moving.center().y, other.center().y)) {
            const float y = nearly(moving.top, other.top)         ? moving.top
                            : nearly(moving.bottom, other.bottom) ? moving.bottom
                                                                  : moving.center().y;
            renderer.line({std::min(moving.left, other.left) - 20.f, y},
                          {std::max(moving.right, other.right) + 20.f, y}, guide, 1.f);
        }
    }
}

void HudEditor::drawToolbar(Renderer& renderer, Vec2 screenSize) {
    Ui& gui = ui();
    const auto& active = theme();

    const Rect toolbar{screenSize.x * 0.5f - 300.f, 16.f, screenSize.x * 0.5f + 300.f,
                       16.f + kToolbarHeight};

    gui.panel(toolbar, active.panelRadius, true);

    gui.text("Éditeur de HUD", Rect{toolbar.left + 18.f, toolbar.top, toolbar.left + 150.f,
                                    toolbar.bottom},
             active.text, 14.f, FontWeight::Bold);

    bool showGrid = settings.value<bool>("showGrid", true);
    if (gui.toggle(UiId("editor_grid"),
                   Rect{toolbar.left + 160.f, toolbar.top, toolbar.left + 210.f, toolbar.bottom},
                   showGrid)) {
        settings.set("showGrid", SettingValue{showGrid});
    }
    gui.text("Grille", Rect{toolbar.left + 214.f, toolbar.top, toolbar.left + 262.f, toolbar.bottom},
             active.textMuted, 12.f);

    bool snapGrid = settings.value<bool>("snapToGrid", true);
    if (gui.toggle(UiId("editor_snap"),
                   Rect{toolbar.left + 270.f, toolbar.top, toolbar.left + 320.f, toolbar.bottom},
                   snapGrid)) {
        settings.set("snapToGrid", SettingValue{snapGrid});
    }
    gui.text("Aimant", Rect{toolbar.left + 324.f, toolbar.top, toolbar.left + 380.f, toolbar.bottom},
             active.textMuted, 12.f);

    float gridSize = static_cast<float>(settings.value<int>("gridSize", 8));
    if (gui.slider(UiId("editor_gridsize"),
                   Rect{toolbar.left + 396.f, toolbar.center().y - 6.f, toolbar.right - 110.f,
                        toolbar.center().y + 6.f},
                   gridSize, 2.f, 64.f, " px", 1.f)) {
        settings.set("gridSize", SettingValue{static_cast<int>(gridSize)});
    }

    if (gui.button(UiId("editor_done"),
                   Rect{toolbar.right - 100.f, toolbar.center().y - 14.f, toolbar.right - 14.f,
                        toolbar.center().y + 14.f},
                   "Terminer", true)) {
        setEnabled(false);
    }
}

void HudEditor::onRender(RenderTopEvent& event) {
    Renderer& renderer = *event.renderer;
    const auto& active = theme();

    fade_.speed = active.motion(14.f);
    fade_.to(1.f);
    fade_.update(event.deltaSeconds);

    Ui& gui = ui();
    gui.beginFrame(renderer, event.deltaSeconds);

    renderer.pushOpacity(fade_.value);

    if (settings.value<bool>("dimGame", true)) {
        renderer.fillRect(Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y),
                          active.backgroundDeep.withAlpha(0.35f));
    }

    drawGrid(renderer, event.screenSize);

    const auto huds = modules().huds();
    const auto group = movingGroup();

    std::vector<Rect> others;
    for (HudModule* element : huds) {
        if (!element->enabled()) continue;
        if (std::ranges::find(group, element) != group.end()) continue;
        others.push_back(element->bounds());
    }

    hovered_ = nullptr;
    for (HudModule* element : huds) {
        if (element->enabled() && element->bounds().contains(gui.mouse())) hovered_ = element;
    }

    if (dragged_ && gui.mouseDown()) {
        const Rect bounds = dragged_->bounds();
        const Vec2 wanted = gui.mouse() - dragOffset_;
        const Vec2 snapped = snap(wanted, bounds, others, event.screenSize);
        const Vec2 delta = snapped - bounds.topLeft();

        for (HudModule* element : group) {
            element->moveTo(element->bounds().topLeft() + delta, event.screenSize);
        }

        drawGuides(renderer, Rect::fromSize(snapped.x, snapped.y, bounds.width(), bounds.height()),
                   others);
    }

    if (settings.value<bool>("showBounds", true)) {
        for (HudModule* element : huds) {
            if (!element->enabled()) continue;

            const Rect bounds = element->bounds();
            if (bounds.width() <= 0.f) continue;

            const bool isDragged = std::ranges::find(group, element) != group.end();
            const bool isHovered = element == hovered_;

            const Color outline = isDragged  ? active.liveAccent()
                                  : isHovered ? active.accentGlow.fade(0.8f)
                                              : active.border.fade(0.75f);

            renderer.strokeRounded(bounds.inflated(2.f), outline, active.radius,
                                   isDragged ? 2.f : 1.f);

            const float tick = 6.f;
            for (const Vec2 corner : {bounds.topLeft(), Vec2{bounds.right, bounds.top},
                                      Vec2{bounds.left, bounds.bottom},
                                      Vec2{bounds.right, bounds.bottom}}) {
                renderer.fillRounded(Rect::fromSize(corner.x - tick * 0.5f, corner.y - tick * 0.5f,
                                                    tick, tick),
                                     outline, 1.5f);
            }

            if (settings.value<bool>("showNames", true) && (isHovered || isDragged)) {
                FontSpec spec;
                spec.family = active.fontFamily;
                spec.size = 11.f;
                spec.weight = FontWeight::SemiBold;
                spec.valign = TextVAlign::Middle;

                const std::string label =
                    element->group().empty()
                        ? element->name()
                        : std::format("{}  ·  groupe « {} »", element->name(), element->group());

                const Vec2 size = renderer.measure(label, spec);
                const Rect tag{bounds.left, bounds.top - size.y - 8.f, bounds.left + size.x + 12.f,
                               bounds.top - 2.f};

                renderer.fillRounded(tag, active.liveAccent(), active.radius * 0.6f);
                renderer.text(label,
                              Rect{tag.left + 6.f, tag.top, tag.right, tag.bottom},
                              active.liveAccent().readableForeground(), spec);
            }
        }
    }

    drawToolbar(renderer, event.screenSize);

    renderer.popOpacity();
    gui.endFrame();
}

}
