#pragma once

#include <deque>
#include <string>

#include "dll/module/Module.hpp"

namespace velyx {

enum class NotificationKind { Info, Success, Warning, Error };

struct Notification {
    NotificationKind kind = NotificationKind::Info;
    std::string title;
    std::string body;
    long long createdAtMs = 0;
    float lifetimeSeconds = 4.f;

    Animated slide{0.f, 16.f};
    bool dismissed = false;
};

class Notifications final : public Module {
public:
    Notifications();

    static void push(NotificationKind kind, std::string title, std::string body = {},
                     float lifetimeSeconds = 4.f);

    static void info(std::string title, std::string body = {});
    static void success(std::string title, std::string body = {});
    static void warning(std::string title, std::string body = {});
    static void error(std::string title, std::string body = {});

    [[nodiscard]] const std::deque<Notification>& history() const { return history_; }

private:
    void onRender(RenderTopEvent& event);
    void onProfileChange(ProfileChangeEvent& event);
    void onModuleToggle(ModuleToggleEvent& event);
    void onThemeChange(ThemeChangeEvent& event);

    static Notifications*& instance();

    std::deque<Notification> active_;
    std::deque<Notification> history_;
};

}
