#include "HudEditor.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "core/Lang.hpp"
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
    : Module("hud_editor", "HUD editor", ModuleCategory::Client,
             "Free placement, grid, alignment and groups.") {
    markInterfaceModule();

    keybind() = config().hudEditorKey;

    settings.header("Grid");
    settings.toggle("showGrid", "Show the grid", true);
    settings.intSlider("gridSize", "Grid step", 8, 2, 64, "", " px");
    settings.toggle("snapToGrid", "Snap to the grid", true);
    settings.toggle("snapToEdges", "Snap to other elements", true);

    settings.header("Placement help");
    settings.toggle("showBounds", "Outline the elements", true);
    settings.toggle("showNames", "Show names", true);
    settings.toggle("dimGame", "Dim the game", true);

    on(&HudEditor::onRender);
    on(&HudEditor::onMouse, EventPriority::First);
    on(&HudEditor::onKey, EventPriority::First);

    addKeywords({"hud", "editor", "placement", "grid", "layout"});
}

void HudEditor::onEnable() {
    WindowHook::setCaptureInput(true);
    fade_.set(0.f);
    fade_.to(1.f);
}

void HudEditor::onDisable() {
    if (!modules().anyInterfaceOpen()) WindowHook::setCaptureInput(false);
    dragged_ = nullptr;

    profiles().saveCurrent();
    ProfileManager::saveInterfaceState();
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

    if (event.action == MouseAction::Press || event.action == MouseAction::Release) {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        if (queuedMouse_.size() < kMaxQueuedInput) queuedMouse_.push_back(event);
    }

    event.cancel();
}

void HudEditor::processInput() {
    std::vector<MouseEvent> mouse;
    {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        if (queuedMouse_.empty()) return;
        mouse.swap(queuedMouse_);
    }

    for (const MouseEvent& event : mouse) {
        if (event.button != MouseButton::Left) continue;

        if (event.action == MouseAction::Press) {
            auto huds = modules().huds();
            for (auto it = huds.rbegin(); it != huds.rend(); ++it) {
                HudModule* element = *it;
                if (!element->enabled()) continue;
                if (!element->bounds().contains(event.position)) continue;

                selected_ = element;
                dragged_ = element;
                dragOffset_ = event.position - element->bounds().topLeft();
                break;
            }
        } else if (event.action == MouseAction::Release && dragged_) {
            for (HudModule* element : movingGroup()) {
                element->reanchorToNearestEdge(Velyx::get().screenSize());
            }
            dragged_ = nullptr;
            guides_.clear();
        }
    }
}

void HudEditor::onKey(KeyEvent& event) {
    if (event.key == keybind().key) return;

    if (event.down && event.key == VK_ESCAPE) {
        modules().requestEnabled(this, false);
        event.cancel();
        return;
    }

    HudModule* target = dragged_ ? dragged_ : (hovered_ ? hovered_ : selected_);
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

void HudEditor::drawSelection(Renderer& renderer, HudModule& element) {
    Ui& gui = ui();
    const auto& active = theme();

    const Rect bounds = element.bounds();
    if (bounds.width() <= 0.f) return;

    const Rect ring = bounds.inflated(3.f);
    renderer.strokeRounded(ring, active.liveAccent(), active.radius + 3.f, 1.5f);

    for (const Vec2 corner : {ring.topLeft(), Vec2{ring.right, ring.top},
                              Vec2{ring.left, ring.bottom}, Vec2{ring.right, ring.bottom}}) {
        const Rect handle = Rect::fromSize(corner.x - 4.f, corner.y - 4.f, 8.f, 8.f);
        renderer.fillRounded(handle, active.backgroundDeep, 2.f);
        renderer.strokeRounded(handle, active.liveAccent(), 2.f, 1.5f);
    }

    FontSpec label;
    label.family = active.fontFamily;
    label.size = 11.5f * active.fontScale;
    label.weight = FontWeight::Medium;
    label.valign = TextVAlign::Middle;

    const std::string name = element.group().empty()
                                 ? element.name()
                                 : element.name() + "  ·  " + element.group();
    const float nameWidth = renderer.measure(name, label).x;
    const float barWidth = 30.f + nameWidth + 12.f + 3 * 28.f + 10.f;

    Rect bar{ring.left, ring.top - 44.f, ring.left + barWidth, ring.top - 8.f};
    if (bar.top < 6.f) bar = Rect{ring.left, ring.bottom + 8.f, ring.left + barWidth,
                                  ring.bottom + 44.f};

    renderer.dropShadow(bar, active.shadowColor, active.radius + 2.f, 10.f, {0.f, 4.f});
    renderer.fillRounded(bar, active.background.withAlpha(0.97f), active.radius + 2.f);
    renderer.strokeRounded(bar, active.border, active.radius + 2.f, active.borderWidth);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            renderer.fillCircle({bar.left + 11.f + static_cast<float>(j) * 5.f,
                                 bar.center().y - 4.f + static_cast<float>(i) * 4.f},
                                1.1f, active.textDim);
        }
    }

    renderer.text(tr(name), Rect{bar.left + 30.f, bar.top, bar.left + 30.f + nameWidth + 4.f,
                             bar.bottom},
                  active.text, label);

    float x = bar.left + 30.f + nameWidth + 8.f;
    renderer.line({x, bar.top + 9.f}, {x, bar.bottom - 9.f}, active.border, 1.f);
    x += 4.f;

    const bool locked = locked_.count(element.id()) > 0;

    if (gui.iconButton(UiId("hud_gear"), Rect{x, bar.top + 4.f, x + 28.f, bar.bottom - 4.f}, "⚙",
                       active.textMuted)) {
        if (Module* menu = modules().find("clickgui")) menu->setEnabled(true);
        setEnabled(false);
        return;
    }
    x += 28.f;

    if (gui.iconButton(UiId("hud_lock"), Rect{x, bar.top + 4.f, x + 28.f, bar.bottom - 4.f},
                       locked ? "⯀" : "⯁",
                       locked ? active.liveAccent() : active.textMuted)) {
        if (locked) {
            locked_.erase(element.id());
        } else {
            locked_.insert(element.id());
        }
    }
    x += 28.f;

    if (gui.iconButton(UiId("hud_close"), Rect{x, bar.top + 4.f, x + 28.f, bar.bottom - 4.f}, "✕",
                       active.danger)) {
        element.setEnabled(false);
        selected_ = nullptr;
        dragged_ = nullptr;
    }
}

void HudEditor::drawBottomBar(Renderer& renderer, Vec2 screenSize) {
    Ui& gui = ui();
    const auto& active = theme();

    const float width = 560.f;
    const Rect bar{screenSize.x * 0.5f - width * 0.5f, screenSize.y - 72.f,
                   screenSize.x * 0.5f + width * 0.5f, screenSize.y - 26.f};

    gui.panel(bar, active.panelRadius, true);

    gui.text("HUD editor", Rect{bar.left + 18.f, bar.top, bar.left + 130.f, bar.bottom},
             active.text, 12.5f, FontWeight::SemiBold);

    renderer.line({bar.left + 140.f, bar.top + 12.f}, {bar.left + 140.f, bar.bottom - 12.f},
                  active.border, 1.f);

    bool showGrid = settings.value<bool>("showGrid", true);
    if (gui.chip(UiId("editor_grid"),
                 Rect{bar.left + 152.f, bar.center().y - 15.f, bar.left + 224.f,
                      bar.center().y + 15.f},
                 "Grid", showGrid)) {
        settings.set("showGrid", SettingValue{!showGrid});
    }

    bool snapGrid = settings.value<bool>("snapToGrid", true);
    if (gui.chip(UiId("editor_snap"),
                 Rect{bar.left + 230.f, bar.center().y - 15.f, bar.left + 300.f,
                      bar.center().y + 15.f},
                 "Snap", snapGrid)) {
        settings.set("snapToGrid", SettingValue{!snapGrid});
    }

    const int size = settings.value<int>("gridSize", 8);
    const Rect stepper{bar.left + 310.f, bar.center().y - 15.f, bar.left + 412.f,
                       bar.center().y + 15.f};
    renderer.fillRounded(stepper, active.surface, active.radius);
    renderer.strokeRounded(stepper, active.border, active.radius, active.borderWidth);

    if (gui.iconButton(UiId("grid_down"),
                       Rect{stepper.left + 3.f, stepper.top + 3.f, stepper.left + 27.f,
                            stepper.bottom - 3.f},
                       "−", active.textMuted)) {
        settings.set("gridSize", SettingValue{std::max(2, size / 2)});
    }

    gui.text(std::format("{} px", size),
             Rect{stepper.left + 27.f, stepper.top, stepper.right - 27.f, stepper.bottom},
             active.text, 11.5f, FontWeight::Medium, TextAlign::Center);

    if (gui.iconButton(UiId("grid_up"),
                       Rect{stepper.right - 27.f, stepper.top + 3.f, stepper.right - 3.f,
                            stepper.bottom - 3.f},
                       "+", active.textMuted)) {
        settings.set("gridSize", SettingValue{std::min(64, size * 2)});
    }

    if (gui.button(UiId("editor_done"),
                   Rect{bar.right - 108.f, bar.center().y - 15.f, bar.right - 16.f,
                        bar.center().y + 15.f},
                   "Done", true)) {
        setEnabled(false);
    }

    gui.text("Drag to move · Shift + arrows to nudge · Esc to leave",
             Rect{bar.left - 60.f, bar.top - 26.f, bar.right + 60.f, bar.top - 6.f},
             active.textDim, 11.f, FontWeight::Regular, TextAlign::Center);
}

void HudEditor::onRender(RenderTopEvent& event) {
    processInput();

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

    if (!selected_) {
        for (HudModule* element : huds) {
            if (element->enabled()) {
                selected_ = element;
                break;
            }
        }
    }

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

    if (dragged_ && gui.mouseDown() && locked_.count(dragged_->id()) == 0) {
        const Rect bounds = dragged_->bounds();
        const Vec2 wanted = gui.mouse() - dragOffset_;
        const Vec2 snapped = snap(wanted, bounds, others, event.screenSize);
        const Vec2 delta = snapped - bounds.topLeft();

        for (HudModule* element : group) {
            element->moveTo(element->bounds().topLeft() + delta, event.screenSize);
        }

        if (settings.value<bool>("snapToEdges", true)) {
            drawGuides(renderer,
                       Rect::fromSize(snapped.x, snapped.y, bounds.width(), bounds.height()),
                       others);
        }
    }

    if (settings.value<bool>("showBounds", true)) {
        for (HudModule* element : huds) {
            if (!element->enabled() || element == selected_) continue;

            const Rect bounds = element->bounds();
            if (bounds.width() <= 0.f) continue;

            const bool isHovered = element == hovered_;
            renderer.strokeRounded(bounds.inflated(3.f),
                                   isHovered ? active.accentGlow.fade(0.55f)
                                             : active.border.fade(0.45f),
                                   active.radius + 3.f, 1.f);
        }
    }

    if (selected_ && selected_->enabled()) drawSelection(renderer, *selected_);

    drawBottomBar(renderer, event.screenSize);

    renderer.popOpacity();
    gui.endFrame();
}

}
