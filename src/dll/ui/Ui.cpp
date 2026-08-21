#include "Ui.hpp"

#include <windows.h>

#include <algorithm>

#include "core/Strings.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr float kRowLabelSize = 14.f;
constexpr float kRowDescriptionSize = 11.5f;
constexpr float kSwitchWidth = 34.f;
constexpr float kSwitchHeight = 20.f;

FontSpec makeFont(float size, FontWeight weight, TextAlign align, TextVAlign valign) {
    const auto& active = theme();

    FontSpec spec;
    spec.family = active.fontFamily;
    spec.size = size * active.fontScale;
    spec.weight = weight;
    spec.letterSpacing = active.letterSpacing;
    spec.align = align;
    spec.valign = valign;
    return spec;
}

}

UiId::UiId(std::string_view name, int index) {
    value = strings::hash32(name) ^ (static_cast<uint32_t>(index) * 2654435761u);
    if (value == 0) value = 1;
}

Ui& Ui::get() {
    static Ui instance;
    return instance;
}

void Ui::beginOverlayFrame() { drainInput(); }

void Ui::endOverlayFrame() {
    clicked_ = false;
    released_ = false;
    wheel_ = 0.f;
    mouseDelta_ = {};
    pendingChars_.clear();
    pendingKeys_.clear();

    if (!mouseDown_) active_ = UiId{};
}

void Ui::beginFrame(Renderer& renderer, float deltaSeconds) {
    renderer_ = &renderer;
    delta_ = deltaSeconds;
    hot_ = UiId{};
}

void Ui::endFrame() { renderer_ = nullptr; }

// The window procedure runs on the game's message thread while the widgets are
// walked from the render thread inside Present, so input is queued here and
// applied at the top of a frame instead. That keeps the per-frame state owned by
// one thread, and it is also what makes a click reliable: applied at the frame
// boundary, it stays true for the whole frame that tests it, where a click landing
// mid-frame used to be wiped by endFrame() before any widget saw it.
void Ui::feedMouse(const MouseEvent& event) {
    const std::lock_guard<std::mutex> guard(inputMutex_);
    if (queuedMouse_.size() < kMaxQueuedInput) queuedMouse_.push_back(event);
}

void Ui::feedKey(const KeyEvent& event) {
    if (!event.down) return;
    const std::lock_guard<std::mutex> guard(inputMutex_);
    if (queuedKeys_.size() < kMaxQueuedInput) queuedKeys_.push_back(event);
}

void Ui::feedChar(const CharEvent& event) {
    if (event.codepoint < 32 || event.codepoint == 127) return;
    const std::lock_guard<std::mutex> guard(inputMutex_);
    if (queuedChars_.size() < kMaxQueuedInput) queuedChars_.push_back(event.codepoint);
}

void Ui::drainInput() {
    std::vector<MouseEvent> mouse;
    std::vector<KeyEvent> keys;
    std::vector<unsigned int> characters;

    {
        const std::lock_guard<std::mutex> guard(inputMutex_);
        mouse.swap(queuedMouse_);
        keys.swap(queuedKeys_);
        characters.swap(queuedChars_);
    }

    for (const MouseEvent& event : mouse) {
        switch (event.action) {
            case MouseAction::Move:
                mouseDelta_ = mouseDelta_ + (event.position - mouse_);
                mouse_ = event.position;
                break;
            case MouseAction::Press:
                if (event.button == MouseButton::Left) {
                    mouseDown_ = true;
                    clicked_ = true;
                }
                break;
            case MouseAction::Release:
                if (event.button == MouseButton::Left) {
                    mouseDown_ = false;
                    released_ = true;
                }
                break;
            case MouseAction::Wheel:
                wheel_ += event.wheelDelta;
                break;
        }
    }

    for (const KeyEvent& event : keys) pendingKeys_.push_back(event.key);

    for (const unsigned int codepoint : characters) {
        if (codepoint < 0x80) {
            pendingChars_.push_back(static_cast<char>(codepoint));
        } else {
            pendingChars_ += strings::toUtf8(std::wstring(1, static_cast<wchar_t>(codepoint)));
        }
    }
}

void Ui::pushModal(const Rect& rect) { modalStack_.push_back(rect); }
void Ui::popModal() {
    if (!modalStack_.empty()) modalStack_.pop_back();
}

bool Ui::interactable(const Rect& rect) const {
    if (modalStack_.empty()) return true;

    return modalStack_.back().contains(mouse_) && rect.intersects(modalStack_.back());
}

bool Ui::hoverAndClick(const UiId& id, const Rect& rect, bool enabled) {
    if (!enabled) return false;

    const bool inside = rect.contains(mouse_) && interactable(rect);
    if (inside) hot_ = id;

    if (inside && clicked_) {
        active_ = id;
        focused_ = UiId{};
        focusedText_ = false;
        return true;
    }
    return false;
}

float Ui::animate(const UiId& id, bool active, float speed) {
    Animated& animation = animations_[id.value];
    animation.speed = theme().motion(speed);
    animation.to(active ? 1.f : 0.f);
    animation.update(delta_);
    return animation.value;
}

void Ui::panel(const Rect& rect, float radius, bool blur) {
    const auto& active = theme();
    const float r = radius < 0.f ? active.panelRadius : radius;

    if (active.shadows) {
        renderer_->dropShadow(rect, active.shadowColor, r, active.shadowSpread, {0.f, 6.f});
    }
    if (blur && active.blur) {
        renderer_->blurBehind(rect, active.blurSigma, r);
    }

    renderer_->fillRounded(rect, active.background.withAlpha(active.panelOpacity), r);
    renderer_->strokeRounded(rect, active.border.fade(0.75f), r, active.borderWidth);
}

void Ui::text(std::string_view value, const Rect& bounds, const Color& color, float size,
              FontWeight weight, TextAlign align) {
    renderer_->text(value, bounds, color, makeFont(size, weight, align, TextVAlign::Middle));
}

void Ui::sectionHeader(std::string_view label, const Rect& bounds) {
    const auto& active = theme();

    const Rect labelRect{bounds.left, bounds.top, bounds.right, bounds.bottom};
    renderer_->text(strings::toUpper(label), labelRect, active.textMuted.fade(0.85f),
                    makeFont(11.f, FontWeight::Bold, TextAlign::Left, TextVAlign::Middle));

    const float textWidth =
        renderer_->measure(strings::toUpper(label),
                           makeFont(11.f, FontWeight::Bold, TextAlign::Left, TextVAlign::Middle)).x;

    const float y = bounds.center().y;
    if (bounds.left + textWidth + 12.f < bounds.right) {
        renderer_->line({bounds.left + textWidth + 8.f, y}, {bounds.right, y},
                        active.border.fade(0.6f), 1.f);
    }
}

bool Ui::button(const UiId& id, const Rect& rect, std::string_view label, bool primary,
                bool enabled) {
    const auto& active = theme();
    const bool pressed = hoverAndClick(id, rect, enabled);
    const float hover = animate(id, hovered(id) && enabled);

    Color background;
    Color foreground;

    if (primary) {
        background = hover > 0.5f ? active.liveAccent().lighten(0.06f) : active.liveAccent();
        foreground = background.readableForeground();
    } else {
        background = active.surfaceHover.fade(hover * 0.8f);
        foreground = lerp(active.textMuted, active.text, hover);
    }

    if (!enabled) {
        background = background.fade(0.4f);
        foreground = foreground.fade(0.45f);
    }

    renderer_->fillRounded(rect, background, active.radius);
    if (!primary) {
        renderer_->strokeRounded(rect, active.border.fade(0.7f + hover * 0.3f), active.radius,
                                 active.borderWidth);
    }

    renderer_->text(label, rect, foreground,
                    makeFont(12.5f, FontWeight::SemiBold, TextAlign::Center, TextVAlign::Middle));

    return pressed;
}

bool Ui::iconButton(const UiId& id, const Rect& rect, std::string_view glyph, const Color& tint) {
    const auto& active = theme();
    const bool pressed = hoverAndClick(id, rect, true);
    const float hover = animate(id, hovered(id));

    if (hover > 0.01f) {
        renderer_->fillRounded(rect, active.surfaceHover.fade(hover), active.radius * 0.75f);
    }

    const Color color = tint.a > 0.f ? tint : active.textMuted;
    renderer_->text(glyph, rect, lerp(color, active.text, hover),
                    makeFont(15.f, FontWeight::Medium, TextAlign::Center, TextVAlign::Middle));

    return pressed;
}

bool Ui::toggle(const UiId& id, const Rect& rect, bool& value) {
    const auto& active = theme();

    const Rect track{rect.right - kSwitchWidth, rect.center().y - kSwitchHeight * 0.5f, rect.right,
                     rect.center().y + kSwitchHeight * 0.5f};

    const bool pressed = hoverAndClick(id, rect);
    if (pressed) value = !value;

    const float on = animate(UiId("toggle_state", static_cast<int>(id.value)), value, 18.f);
    const float hover = animate(id, hovered(id));

    const Color trackColor = lerp(active.border, active.liveAccent(), on);
    renderer_->fillRounded(track, trackColor, track.height() * 0.5f);

    const float knobRadius = track.height() * 0.5f - 3.f;
    const float knobX = lerp(track.left + knobRadius + 3.f, track.right - knobRadius - 3.f, on);

    if (hover > 0.01f) {
        renderer_->fillCircle({knobX, track.center().y}, knobRadius + 3.f * hover,
                              active.accentGlow.fade(0.2f * hover));
    }

    renderer_->fillCircle({knobX, track.center().y}, knobRadius,
                          lerp(active.textMuted, trackColor.readableForeground(), on));

    return pressed;
}

bool Ui::toggleRow(const UiId& id, const Rect& rect, std::string_view label,
                   std::string_view description, bool& value) {
    const auto& active = theme();
    const float hover = animate(UiId("row", static_cast<int>(id.value)), hovered(id));

    if (hover > 0.01f) {
        renderer_->fillRounded(rect, active.surfaceHover.fade(0.5f * hover), active.radius);
    }

    const float inset = active.spacing * 1.25f;
    const bool hasDescription = !description.empty();

    const Rect labelRect{rect.left + inset, rect.top + (hasDescription ? 6.f : 0.f),
                         rect.right - kSwitchWidth - inset * 2.f,
                         hasDescription ? rect.top + rect.height() * 0.55f : rect.bottom};
    renderer_->text(label, labelRect, active.text,
                    makeFont(kRowLabelSize, FontWeight::Medium, TextAlign::Left,
                             hasDescription ? TextVAlign::Middle : TextVAlign::Middle));

    if (hasDescription) {
        const Rect descriptionRect{labelRect.left, rect.top + rect.height() * 0.5f,
                                   labelRect.right, rect.bottom - 5.f};
        renderer_->text(description, descriptionRect, active.textMuted,
                        makeFont(kRowDescriptionSize, FontWeight::Regular, TextAlign::Left,
                                 TextVAlign::Middle));
    }

    return toggle(id, Rect{rect.left, rect.top, rect.right - inset, rect.bottom}, value);
}

bool Ui::slider(const UiId& id, const Rect& rect, float& value, float minimum, float maximum,
                std::string_view suffix, float step) {
    const auto& active = theme();

    const float trackHeight = 4.f;
    const Rect track{rect.left, rect.center().y - trackHeight * 0.5f, rect.right,
                     rect.center().y + trackHeight * 0.5f};

    const Rect grabArea = rect.inflated(4.f);
    hoverAndClick(id, grabArea);

    bool changed = false;
    if (active_ == id && mouseDown_) {
        const float t = clamp((mouse_.x - track.left) / std::max(1.f, track.width()), 0.f, 1.f);
        float next = lerp(minimum, maximum, t);

        if (step > 0.f) next = std::round(next / step) * step;
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    const float t = maximum > minimum ? clamp((value - minimum) / (maximum - minimum), 0.f, 1.f)
                                      : 0.f;
    const float hover = animate(id, hovered(id) || active_ == id);

    renderer_->fillRounded(track, active.surface, trackHeight * 0.5f);

    const Rect filled{track.left, track.top, lerp(track.left, track.right, t), track.bottom};
    if (filled.width() > 0.5f) {
        renderer_->fillGradient(filled, active.liveAccentDeep(), active.liveAccent(), 0.f,
                                trackHeight * 0.5f);
    }

    const Vec2 knob{filled.right, track.center().y};
    renderer_->fillCircle(knob, 8.f + hover * 2.f, active.backgroundDeep.withAlpha(0.45f));
    renderer_->fillCircle(knob, 6.f + hover * 0.5f, active.text);

    if (!suffix.empty() || hover > 0.01f) {
        const std::string label =
            strings::formatFloat(value, step >= 1.f ? 0 : 2) + std::string(suffix);
        renderer_->text(label, Rect{rect.left, rect.top - 18.f, rect.right, rect.top},
                        active.textMuted.fade(0.6f + hover * 0.4f),
                        makeFont(11.f, FontWeight::Medium, TextAlign::Right, TextVAlign::Middle));
    }

    return changed;
}

bool Ui::sliderRow(const UiId& id, const Rect& rect, std::string_view label,
                   std::string_view description, float& value, float minimum, float maximum,
                   std::string_view suffix, float step) {
    const auto& active = theme();
    const float inset = active.spacing * 1.25f;

    const Rect labelRect{rect.left + inset, rect.top, rect.right - inset,
                         rect.top + rect.height() * 0.45f};
    renderer_->text(label, labelRect, active.text,
                    makeFont(kRowLabelSize, FontWeight::Medium, TextAlign::Left, TextVAlign::Middle));

    const std::string valueText =
        strings::formatFloat(value, step >= 1.f ? 0 : 2) + std::string(suffix);
    renderer_->text(valueText, labelRect, active.liveAccent(),
                    makeFont(kRowLabelSize, FontWeight::SemiBold, TextAlign::Right,
                             TextVAlign::Middle));

    const Rect trackRect{rect.left + inset, rect.top + rect.height() * 0.5f, rect.right - inset,
                         rect.bottom - 6.f};
    return slider(id, trackRect, value, minimum, maximum, {}, step);
}

bool Ui::dropdown(const UiId& id, const Rect& rect, std::string& value,
                  const std::vector<std::string>& options) {
    const auto& active = theme();
    const bool open = openDropdown_ == id;

    if (hoverAndClick(id, rect)) {
        openDropdown_ = open ? UiId{} : id;
    }

    const float hover = animate(id, hovered(id) || open);
    renderer_->fillRounded(rect, lerp(active.surface, active.surfaceHover, hover), active.radius);
    renderer_->strokeRounded(rect, open ? active.liveAccent() : active.border.fade(0.6f),
                             active.radius, active.borderWidth);

    const float inset = active.spacing;
    renderer_->text(value, Rect{rect.left + inset, rect.top, rect.right - 20.f, rect.bottom},
                    active.text, makeFont(13.f, FontWeight::Medium, TextAlign::Left,
                                          TextVAlign::Middle));

    const Vec2 centre{rect.right - 14.f, rect.center().y};
    const float rotation = animate(UiId("chev", static_cast<int>(id.value)), open) * 180.f;
    renderer_->pushRotation(rotation, centre);
    renderer_->polyline({{centre.x - 4.f, centre.y - 2.f},
                         {centre.x, centre.y + 2.5f},
                         {centre.x + 4.f, centre.y - 2.f}},
                        active.textMuted, 1.6f);
    renderer_->popTransform();

    if (!open) return false;

    const float itemHeight = 28.f;
    const float listHeight = std::min(itemHeight * static_cast<float>(options.size()), 220.f);
    const Rect list{rect.left, rect.bottom + 4.f, rect.right, rect.bottom + 4.f + listHeight};

    pushModal(list);
    panel(list, active.radius, false);

    bool changed = false;
    float y = list.top;

    for (size_t i = 0; i < options.size(); ++i) {
        const Rect item{list.left + 3.f, y, list.right - 3.f, y + itemHeight};
        y += itemHeight;
        if (item.bottom > list.bottom) break;

        const UiId itemId("dropdown_item", static_cast<int>(id.value) + static_cast<int>(i) * 7);
        const bool selected = options[i] == value;
        const float itemHover = animate(itemId, hovered(itemId));

        if (itemHover > 0.01f || selected) {
            renderer_->fillRounded(item,
                                   selected ? active.liveAccent().fade(0.18f)
                                            : active.surfaceHover.fade(itemHover),
                                   active.radius * 0.7f);
        }

        renderer_->text(options[i], Rect{item.left + 8.f, item.top, item.right, item.bottom},
                        selected ? active.liveAccent() : active.text,
                        makeFont(13.f, selected ? FontWeight::SemiBold : FontWeight::Regular,
                                 TextAlign::Left, TextVAlign::Middle));

        if (hoverAndClick(itemId, item)) {
            value = options[i];
            changed = true;
            openDropdown_ = UiId{};
        }
    }

    popModal();

    if (clicked_ && !list.contains(mouse_) && !rect.contains(mouse_)) openDropdown_ = UiId{};

    return changed;
}

bool Ui::textField(const UiId& id, const Rect& rect, std::string& value,
                   std::string_view placeholder, size_t maxLength) {
    const auto& active = theme();
    const bool focused = focused_ == id;

    const bool inside = rect.contains(mouse_) && interactable(rect);
    if (inside) hot_ = id;

    if (clicked_) {
        if (inside) {
            focused_ = id;
            focusedText_ = true;
        } else if (focused) {
            focused_ = UiId{};
            focusedText_ = false;
        }
    }

    bool changed = false;

    if (focused) {
        for (const char c : pendingChars_) {
            if (value.size() < maxLength) {
                value.push_back(c);
                changed = true;
            }
        }
        for (const int key : pendingKeys_) {
            if (key == VK_BACK && !value.empty()) {

                do {
                    value.pop_back();
                } while (!value.empty() &&
                         (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80);
                changed = true;
            } else if (key == VK_ESCAPE || key == VK_RETURN) {
                focused_ = UiId{};
                focusedText_ = false;
            }
        }
    }

    const float glow = animate(id, focused || hovered(id));
    renderer_->fillRounded(rect, active.surface, active.radius);
    renderer_->strokeRounded(rect, focused ? active.liveAccent() : active.border.fade(0.5f + glow * 0.4f),
                             active.radius, active.borderWidth + (focused ? 0.5f : 0.f));

    const Rect inner{rect.left + active.spacing, rect.top, rect.right - active.spacing, rect.bottom};
    const bool showPlaceholder = value.empty() && !placeholder.empty();

    renderer_->text(showPlaceholder ? placeholder : value, inner,
                    showPlaceholder ? active.textMuted.fade(0.7f) : active.text,
                    makeFont(13.f, FontWeight::Regular, TextAlign::Left, TextVAlign::Middle));

    if (focused) {

        const auto ms = GetTickCount64() % 1000;
        if (ms < 550) {
            const float width =
                renderer_->measure(value, makeFont(13.f, FontWeight::Regular, TextAlign::Left,
                                                   TextVAlign::Middle)).x;
            const float x = std::min(inner.left + width + 1.f, inner.right - 2.f);
            renderer_->line({x, rect.top + 6.f}, {x, rect.bottom - 6.f}, active.liveAccent(), 1.5f);
        }
    }

    return changed;
}

bool Ui::colorPicker(const UiId& id, const Rect& rect, Color& value, bool withAlpha) {
    const auto& active = theme();

    float hue, saturation, brightness;
    value.toHsv(hue, saturation, brightness);

    auto& storedHue = pickerHue_[id.value];
    if (saturation > 0.01f) storedHue = hue;
    hue = storedHue;

    bool changed = false;

    const float trackHeight = 14.f;
    const float gap = 8.f;
    const float squareSize = rect.height() - (withAlpha ? (trackHeight + gap) * 2.f
                                                        : trackHeight + gap);

    const Rect square{rect.left, rect.top, rect.left + squareSize, rect.top + squareSize};

    renderer_->fillRect(square, Color::fromHsv(hue, 1.f, 1.f));
    renderer_->fillGradient(square, Color{1.f, 1.f, 1.f, 1.f}, Color{1.f, 1.f, 1.f, 0.f}, 0.f);
    renderer_->fillGradient(square, Color{0.f, 0.f, 0.f, 0.f}, Color{0.f, 0.f, 0.f, 1.f}, 90.f);
    renderer_->strokeRect(square, active.border, 1.f);

    const UiId squareId("sv", static_cast<int>(id.value));
    hoverAndClick(squareId, square);
    if (active_ == squareId && mouseDown_) {
        saturation = clamp((mouse_.x - square.left) / square.width(), 0.f, 1.f);
        brightness = 1.f - clamp((mouse_.y - square.top) / square.height(), 0.f, 1.f);
        value = Color::fromHsv(hue, saturation, brightness, value.a);
        changed = true;
    }

    const Vec2 marker{square.left + saturation * square.width(),
                      square.top + (1.f - brightness) * square.height()};
    renderer_->strokeCircle(marker, 5.f, Color{1.f, 1.f, 1.f, 0.9f}, 2.f);
    renderer_->strokeCircle(marker, 6.5f, Color{0.f, 0.f, 0.f, 0.35f}, 1.f);

    const Rect hueTrack{rect.left, square.bottom + gap, rect.right, square.bottom + gap + trackHeight};

    constexpr int kHueSteps = 24;
    for (int i = 0; i < kHueSteps; ++i) {
        const float t0 = static_cast<float>(i) / kHueSteps;
        const float t1 = static_cast<float>(i + 1) / kHueSteps;
        const Rect band{lerp(hueTrack.left, hueTrack.right, t0), hueTrack.top,
                        lerp(hueTrack.left, hueTrack.right, t1) + 1.f, hueTrack.bottom};
        renderer_->fillGradient(band, Color::fromHsv(t0 * 360.f, 1.f, 1.f),
                                Color::fromHsv(t1 * 360.f, 1.f, 1.f), 0.f);
    }
    renderer_->strokeRounded(hueTrack, active.border, trackHeight * 0.5f, 1.f);

    const UiId hueId("hue", static_cast<int>(id.value));
    hoverAndClick(hueId, hueTrack);
    if (active_ == hueId && mouseDown_) {
        storedHue = clamp((mouse_.x - hueTrack.left) / hueTrack.width(), 0.f, 1.f) * 360.f;
        value = Color::fromHsv(storedHue, std::max(saturation, 0.01f), std::max(brightness, 0.01f),
                               value.a);
        changed = true;
    }

    const float hueX = hueTrack.left + (hue / 360.f) * hueTrack.width();
    renderer_->fillCircle({hueX, hueTrack.center().y}, trackHeight * 0.5f,
                          Color::fromHsv(hue, 1.f, 1.f));
    renderer_->strokeCircle({hueX, hueTrack.center().y}, trackHeight * 0.5f,
                            Color{1.f, 1.f, 1.f, 0.9f}, 2.f);

    if (withAlpha) {
        const Rect alphaTrack{rect.left, hueTrack.bottom + gap, rect.right,
                              hueTrack.bottom + gap + trackHeight};

        const int columns = static_cast<int>(alphaTrack.width() / 7.f);
        for (int i = 0; i < columns; ++i) {
            const Rect cell{alphaTrack.left + static_cast<float>(i) * 7.f, alphaTrack.top,
                            alphaTrack.left + static_cast<float>(i + 1) * 7.f, alphaTrack.bottom};
            renderer_->fillRect(cell, i % 2 == 0 ? Color::rgb8(90, 100, 95)
                                                 : Color::rgb8(60, 70, 65));
        }
        renderer_->fillGradient(alphaTrack, value.withAlpha(0.f), value.withAlpha(1.f), 0.f,
                                trackHeight * 0.5f);
        renderer_->strokeRounded(alphaTrack, active.border, trackHeight * 0.5f, 1.f);

        const UiId alphaId("alpha", static_cast<int>(id.value));
        hoverAndClick(alphaId, alphaTrack);
        if (active_ == alphaId && mouseDown_) {
            value.a = clamp((mouse_.x - alphaTrack.left) / alphaTrack.width(), 0.f, 1.f);
            changed = true;
        }

        const float alphaX = alphaTrack.left + value.a * alphaTrack.width();
        renderer_->fillCircle({alphaX, alphaTrack.center().y}, trackHeight * 0.5f, value);
        renderer_->strokeCircle({alphaX, alphaTrack.center().y}, trackHeight * 0.5f,
                                Color{1.f, 1.f, 1.f, 0.9f}, 2.f);
    }

    return changed;
}

bool Ui::colorRow(const UiId& id, const Rect& rect, std::string_view label, Color& value) {
    const auto& active = theme();
    const float inset = active.spacing * 1.25f;
    const bool open = openPicker_ == id;

    const float hover = animate(UiId("crow", static_cast<int>(id.value)), hovered(id));
    if (hover > 0.01f) {
        renderer_->fillRounded(rect, active.surfaceHover.fade(0.5f * hover), active.radius);
    }

    renderer_->text(label, Rect{rect.left + inset, rect.top, rect.right - 60.f, rect.bottom},
                    active.text,
                    makeFont(kRowLabelSize, FontWeight::Medium, TextAlign::Left, TextVAlign::Middle));

    const Rect swatch{rect.right - inset - 46.f, rect.center().y - 11.f, rect.right - inset,
                      rect.center().y + 11.f};

    renderer_->fillRounded(swatch, value, active.radius * 0.7f);
    renderer_->strokeRounded(swatch, open ? active.liveAccent() : active.border,
                             active.radius * 0.7f, active.borderWidth);

    if (hoverAndClick(id, rect)) openPicker_ = open ? UiId{} : id;

    if (!open) return false;

    const Rect picker{rect.left + inset, rect.bottom + 4.f, rect.right - inset,
                      rect.bottom + 4.f + 190.f};

    pushModal(picker);
    panel(picker, active.radius, false);
    const bool changed = colorPicker(UiId("picker", static_cast<int>(id.value)),
                                     picker.inflated(-10.f), value);
    popModal();

    if (clicked_ && !picker.contains(mouse_) && !rect.contains(mouse_)) openPicker_ = UiId{};

    return changed;
}

bool Ui::keybindField(const UiId& id, const Rect& rect, Keybind& value) {
    const auto& active = theme();
    const bool capturing = capturingKeybind_ == id;

    if (hoverAndClick(id, rect)) {
        capturingKeybind_ = capturing ? UiId{} : id;
        return false;
    }

    bool changed = false;

    if (capturing) {
        for (const int key : pendingKeys_) {
            if (key == VK_ESCAPE) {
                value = Keybind{0, false, false, false, value.mode};
            } else if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU) {
                continue;
            } else {
                value.key = key;
                value.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                value.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                value.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            }
            capturingKeybind_ = UiId{};
            changed = true;
            break;
        }
    }

    const float glow = animate(id, capturing || hovered(id));
    renderer_->fillRounded(rect, active.surface, active.radius);
    renderer_->strokeRounded(rect, capturing ? active.liveAccent() : active.border.fade(0.5f + glow * 0.4f),
                             active.radius, active.borderWidth);

    renderer_->text(capturing ? "Appuyez sur une touche…" : describeKeybind(value), rect,
                    capturing ? active.liveAccent() : active.text,
                    makeFont(12.5f, FontWeight::Medium, TextAlign::Center, TextVAlign::Middle));

    return changed;
}

int Ui::segmented(const UiId& id, const Rect& rect, const std::vector<std::string>& options,
                  int selected) {
    if (options.empty()) return selected;

    const auto& active = theme();
    renderer_->fillRounded(rect, active.surface.fade(0.7f), active.radius);

    const float itemWidth = rect.width() / static_cast<float>(options.size());
    int result = selected;

    Animated& indicator = animations_[UiId("seg_indicator", static_cast<int>(id.value)).value];
    indicator.speed = active.motion(18.f);
    indicator.to(static_cast<float>(selected));
    indicator.update(delta_);

    const Rect pill{rect.left + indicator.value * itemWidth + 3.f, rect.top + 3.f,
                    rect.left + (indicator.value + 1.f) * itemWidth - 3.f, rect.bottom - 3.f};
    renderer_->fillRounded(pill, active.liveAccent().fade(0.9f), active.radius * 0.8f);

    for (size_t i = 0; i < options.size(); ++i) {
        const Rect item{rect.left + static_cast<float>(i) * itemWidth, rect.top,
                        rect.left + static_cast<float>(i + 1) * itemWidth, rect.bottom};

        const UiId itemId("seg", static_cast<int>(id.value) + static_cast<int>(i) * 31);
        if (hoverAndClick(itemId, item)) result = static_cast<int>(i);

        const bool isSelected = static_cast<int>(i) == selected;
        renderer_->text(options[i], item,
                        isSelected ? active.liveAccent().readableForeground() : active.textMuted,
                        makeFont(12.5f, isSelected ? FontWeight::SemiBold : FontWeight::Medium,
                                 TextAlign::Center, TextVAlign::Middle));
    }

    return result;
}

bool Ui::chip(const UiId& id, const Rect& rect, std::string_view label, bool selected) {
    const auto& active = theme();
    const bool pressed = hoverAndClick(id, rect);
    const float hover = animate(id, hovered(id));
    const float on = animate(UiId("chip_on", static_cast<int>(id.value)), selected, 20.f);

    const float radius = rect.height() * 0.5f;

    if (on > 0.01f) {
        renderer_->fillRounded(rect, active.liveAccent().fade(0.12f * on), radius);
        renderer_->strokeRounded(rect, active.liveAccent().fade(0.24f * on), radius,
                                 active.borderWidth);
    } else if (hover > 0.01f) {
        renderer_->fillRounded(rect, active.surfaceHover.fade(0.55f * hover), radius);
    }

    const Color idle = lerp(active.textDim, active.text, hover);
    renderer_->text(label, rect, lerp(idle, active.liveAccent(), on),
                    makeFont(12.f, selected ? FontWeight::SemiBold : FontWeight::Medium,
                             TextAlign::Center, TextVAlign::Middle));

    return pressed;
}

bool Ui::resetButton(const UiId& id, const Rect& rect, bool visible) {
    const auto& active = theme();
    const float show = animate(UiId("reset_show", static_cast<int>(id.value)), visible, 18.f);
    if (show <= 0.01f) return false;

    const bool pressed = hoverAndClick(id, rect, visible);
    const float hover = animate(id, hovered(id));

    const Vec2 centre = rect.center();
    const Color colour = lerp(active.textDim, active.text, hover).fade(show);

    renderer_->arc(centre, 5.5f, 1.4f, 40.f, 285.f, colour);
    renderer_->fillPolygon({{centre.x + 4.f, centre.y - 7.5f},
                            {centre.x + 8.f, centre.y - 3.5f},
                            {centre.x + 2.5f, centre.y - 3.f}},
                           colour);

    return pressed;
}

float Ui::beginScroll(const UiId& id, const Rect& rect, float contentHeight) {
    ScrollState& state = scrolls_[id.value];
    state.contentHeight = contentHeight;
    state.view = rect;

    const float maximum = std::max(0.f, contentHeight - rect.height());

    if (rect.contains(mouse_) && interactable(rect) && wheel_ != 0.f) {
        state.target = clamp(state.target - wheel_ * 60.f, 0.f, maximum);
        wheel_ = 0.f;
    }
    state.target = clamp(state.target, 0.f, maximum);
    state.offset = approach(state.offset, state.target, delta_, theme().motion(18.f));

    renderer_->pushClip(rect);
    scrollStack_.push_back(id);

    return -state.offset;
}

void Ui::endScroll() {
    if (scrollStack_.empty()) return;

    const UiId id = scrollStack_.back();
    scrollStack_.pop_back();
    renderer_->popClip();

    const ScrollState& state = scrolls_[id.value];
    const float viewHeight = state.view.height();
    if (state.contentHeight <= viewHeight || viewHeight <= 0.f) return;

    const auto& active = theme();
    const float ratio = viewHeight / state.contentHeight;
    const float barHeight = std::max(32.f, viewHeight * ratio);
    const float maximum = state.contentHeight - viewHeight;
    const float t = maximum > 0.f ? state.offset / maximum : 0.f;

    const float x = state.view.right - 5.f;
    const float top = lerp(state.view.top + 2.f, state.view.bottom - barHeight - 2.f, t);

    const Rect bar{x - 2.f, top, x + 2.f, top + barHeight};
    renderer_->fillRounded(bar, active.textMuted.fade(0.35f), 2.f);
}

}
