#pragma once

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <json/json.hpp>

#include "core/Color.hpp"
#include "core/Math.hpp"

namespace velyx {

struct Keybind {
    int key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    enum class Mode { Toggle, Hold, Once };
    Mode mode = Mode::Toggle;

    [[nodiscard]] bool bound() const { return key != 0; }
    bool operator==(const Keybind&) const = default;
};

enum class SettingType {
    Toggle,
    Slider,
    IntSlider,
    Text,
    Dropdown,
    Color,
    Keybind,
    Position,
    Header,
    Button,
};

using SettingValue = std::variant<std::monostate, bool, int, float, std::string, Color, Keybind, Vec2>;

struct Setting {
    std::string id;
    std::string label;
    std::string description;
    SettingType type = SettingType::Toggle;

    SettingValue value;
    SettingValue fallback;

    float min = 0.f;
    float max = 1.f;
    float step = 0.f;
    std::string unit;

    std::vector<std::string> options;
    std::vector<std::string> keywords;

    std::function<bool()> visibleWhen;

    std::function<void()> action;

    std::function<void()> onChange;

    bool advanced = false;

    [[nodiscard]] bool hasValue() const {
        return type != SettingType::Header && type != SettingType::Button;
    }
    [[nodiscard]] bool visible() const { return !visibleWhen || visibleWhen(); }
    void reset() { value = fallback; }
};

class Settings {
public:
    Setting& toggle(std::string id, std::string label, bool value, std::string description = {});
    Setting& slider(std::string id, std::string label, float value, float min, float max,
                    std::string description = {}, std::string unit = {});
    Setting& intSlider(std::string id, std::string label, int value, int min, int max,
                       std::string description = {}, std::string unit = {});
    Setting& text(std::string id, std::string label, std::string value,
                  std::string description = {});
    Setting& dropdown(std::string id, std::string label, std::string value,
                      std::vector<std::string> options, std::string description = {});
    Setting& color(std::string id, std::string label, Color value, std::string description = {});
    Setting& keybind(std::string id, std::string label, Keybind value,
                     std::string description = {});
    Setting& position(std::string id, std::string label, Vec2 value,
                      std::string description = {});
    Setting& header(std::string label);
    Setting& button(std::string id, std::string label, std::function<void()> action,
                    std::string description = {});

    [[nodiscard]] Setting* find(std::string_view id);
    [[nodiscard]] const Setting* find(std::string_view id) const;
    [[nodiscard]] bool has(std::string_view id) const { return find(id) != nullptr; }

    template <typename T>
    T& get(std::string_view id) {
        Setting* setting = find(id);
        if (!setting) throw std::runtime_error("unknown setting: " + std::string(id));
        return std::get<T>(setting->value);
    }

    template <typename T>
    const T& get(std::string_view id) const {
        const Setting* setting = find(id);
        if (!setting) throw std::runtime_error("unknown setting: " + std::string(id));
        return std::get<T>(setting->value);
    }

    template <typename T>
    T value(std::string_view id, T fallback = T{}) const {
        const Setting* setting = find(id);
        if (!setting) return fallback;
        if (const auto* typed = std::get_if<T>(&setting->value)) return *typed;
        return fallback;
    }

    bool set(std::string_view id, const SettingValue& value);

    void resetAll();

    [[nodiscard]] std::vector<Setting>& list() { return settings_; }
    [[nodiscard]] const std::vector<Setting>& list() const { return settings_; }
    [[nodiscard]] bool empty() const { return settings_.empty(); }

    [[nodiscard]] nlohmann::json save() const;
    void load(const nlohmann::json& json);

private:
    Setting& add(Setting setting);

    std::vector<Setting> settings_;
};

nlohmann::json toJson(const SettingValue& value);
SettingValue fromJson(const nlohmann::json& json, const SettingValue& fallback);

std::string describeKeybind(const Keybind& bind);

}
