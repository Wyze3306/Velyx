#include "Notifications.hpp"

#include <chrono>

#include "core/Lang.hpp"
#include "core/Log.hpp"
#include "dll/render/Renderer.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr size_t kMaxVisible = 5;
constexpr size_t kMaxHistory = 200;
constexpr float kWidth = 320.f;
constexpr float kGap = 8.f;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Color colorFor(NotificationKind kind, const Theme& active) {
    switch (kind) {
        case NotificationKind::Success: return active.success;
        case NotificationKind::Warning: return active.warning;
        case NotificationKind::Error:   return active.danger;
        case NotificationKind::Info:    break;
    }
    return active.liveAccent();
}

const char* glyphFor(NotificationKind kind) {
    switch (kind) {
        case NotificationKind::Success: return "✓";
        case NotificationKind::Warning: return "!";
        case NotificationKind::Error:   return "✕";
        case NotificationKind::Info:    break;
    }
    return "i";
}

}

Notifications*& Notifications::instance() {
    static Notifications* pointer = nullptr;
    return pointer;
}

Notifications::Notifications()
    : Module("notifications", "Notification centre", ModuleCategory::Client,
             "The client's own messages, with a history.") {
    markEssential();
    instance() = this;

    settings.header("Display");
    settings.dropdown("corner", "Coin", "Top right",
                      {"Top right", "Top left", "Bottom right", "Bottom left"});
    settings.slider("duration", "Time on screen", 4.f, 1.f, 15.f, "", " s");
    settings.slider("scale", "Size", 1.f, 0.7f, 1.6f, "", "x");

    settings.header("Events to notify");
    settings.toggle("onProfile", "Profile changed", true);
    settings.toggle("onModule", "Module toggled", false);
    settings.toggle("onTheme", "Theme changed", false);

    always(&Notifications::onRender);
    always(&Notifications::onProfileChange);
    always(&Notifications::onModuleToggle);
    always(&Notifications::onThemeChange);

    setEnabled(true, false);
    addKeywords({"notifications", "alerts", "messages"});
}

void Notifications::push(NotificationKind kind, std::string title, std::string body,
                         float lifetimeSeconds) {
    Notifications* self = instance();
    if (!self) return;

    Notification notification;
    notification.kind = kind;
    notification.title = std::move(title);
    notification.body = std::move(body);
    notification.createdAtMs = nowMs();
    notification.lifetimeSeconds = self->settings.has("duration")
                                       ? self->settings.value<float>("duration", lifetimeSeconds)
                                       : lifetimeSeconds;
    notification.slide.to(1.f);

    self->active_.push_back(notification);
    self->history_.push_back(notification);

    while (self->active_.size() > kMaxVisible) self->active_.pop_front();
    while (self->history_.size() > kMaxHistory) self->history_.pop_front();
}

void Notifications::info(std::string title, std::string body) {
    push(NotificationKind::Info, std::move(title), std::move(body));
}
void Notifications::success(std::string title, std::string body) {
    push(NotificationKind::Success, std::move(title), std::move(body));
}
void Notifications::warning(std::string title, std::string body) {
    push(NotificationKind::Warning, std::move(title), std::move(body));
}
void Notifications::error(std::string title, std::string body) {
    push(NotificationKind::Error, std::move(title), std::move(body), 8.f);
}

void Notifications::onProfileChange(ProfileChangeEvent& event) {
    if (!settings.value<bool>("onProfile", true)) return;

    push(NotificationKind::Info, "Profile « " + event.current + " »",
         event.automatic ? "Picked automatically for this server." : "");
}

void Notifications::onModuleToggle(ModuleToggleEvent& event) {
    if (!settings.value<bool>("onModule", false) || !event.byUser || !event.module) return;

    push(event.enabled ? NotificationKind::Success : NotificationKind::Info, event.module->name(),
         event.enabled ? "On" : "Off", 2.f);
}

void Notifications::onThemeChange(ThemeChangeEvent& event) {
    if (!settings.value<bool>("onTheme", false)) return;
    push(NotificationKind::Info, "Theme « " + event.name + " »", "", 2.5f);
}

void Notifications::onRender(RenderTopEvent& event) {
    if (active_.empty()) return;

    Renderer& renderer = *event.renderer;
    const auto& active = theme();
    const float scale = settings.value<float>("scale", 1.f);
    const std::string corner = settings.value<std::string>("corner", "Top right");

    const bool right = corner.find("right") != std::string::npos;
    const bool bottom = corner.find("Bottom") != std::string::npos;

    const float width = kWidth * scale;
    const float margin = 16.f;

    const long long now = nowMs();
    float offset = 0.f;

    for (auto it = active_.begin(); it != active_.end();) {
        Notification& notification = *it;

        const float age = static_cast<float>(now - notification.createdAtMs) / 1000.f;
        const bool expired = age > notification.lifetimeSeconds;

        notification.slide.speed = active.motion(16.f);
        notification.slide.to(expired ? 0.f : 1.f);
        notification.slide.update(event.deltaSeconds);

        if (expired && notification.slide.value < 0.01f) {
            it = active_.erase(it);
            continue;
        }

        const float appear = ease(active.easing, notification.slide.value);

        FontSpec titleSpec;
        titleSpec.family = active.fontFamily;
        titleSpec.size = 13.5f * scale * active.fontScale;
        titleSpec.weight = FontWeight::SemiBold;
        titleSpec.valign = TextVAlign::Middle;

        FontSpec bodySpec = titleSpec;
        bodySpec.size = 11.5f * scale * active.fontScale;
        bodySpec.weight = FontWeight::Regular;
        bodySpec.wrap = true;

        const bool hasBody = !notification.body.empty();
        const float height = (hasBody ? 58.f : 42.f) * scale;

        const float x = right ? event.screenSize.x - margin - width : margin;
        const float y = bottom ? event.screenSize.y - margin - offset - height : margin + offset;

        const float slideDistance = (width + margin) * (1.f - appear);
        const Rect card = Rect::fromSize(x + (right ? slideDistance : -slideDistance), y, width,
                                         height);

        renderer.pushOpacity(appear);

        if (active.shadows) {
            renderer.dropShadow(card, active.shadowColor, active.radius, active.shadowSpread * 0.6f,
                                {0.f, 4.f});
        }
        renderer.fillRounded(card, active.background.withAlpha(0.96f), active.radius);
        renderer.strokeRounded(card, active.border.fade(0.7f), active.radius, active.borderWidth);

        const Color accent = colorFor(notification.kind, active);

        renderer.fillRounded(Rect{card.left, card.top + 6.f, card.left + 3.f, card.bottom - 6.f},
                             accent, 1.5f);

        const float remaining =
            clamp(1.f - age / std::max(0.1f, notification.lifetimeSeconds), 0.f, 1.f);
        renderer.fillRounded(Rect{card.left + 10.f, card.bottom - 4.f,
                                  card.left + 10.f + (card.width() - 20.f) * remaining,
                                  card.bottom - 2.5f},
                             accent.fade(0.5f), 1.f);

        const Rect badge{card.left + 14.f, card.center().y - 11.f, card.left + 36.f,
                         card.center().y + 11.f};
        renderer.fillRounded(badge, accent.fade(0.18f), active.radius * 0.7f);
        renderer.text(glyphFor(notification.kind), badge, accent,
                      [&] {
                          FontSpec spec = titleSpec;
                          spec.align = TextAlign::Center;
                          return spec;
                      }());

        const Rect textArea{card.left + 46.f, card.top + 8.f, card.right - 12.f, card.bottom - 8.f};

        if (hasBody) {
            renderer.text(tr(notification.title),
                          Rect{textArea.left, textArea.top, textArea.right,
                               textArea.top + 20.f * scale},
                          active.text, titleSpec);
            renderer.text(tr(notification.body),
                          Rect{textArea.left, textArea.top + 20.f * scale, textArea.right,
                               textArea.bottom},
                          active.textMuted, bodySpec);
        } else {
            renderer.text(tr(notification.title), textArea, active.text, titleSpec);
        }

        renderer.popOpacity();

        offset += height + kGap;
        ++it;
    }
}

}
