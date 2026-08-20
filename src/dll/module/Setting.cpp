#include "Setting.hpp"

#include <algorithm>
#include <stdexcept>

#include "core/Log.hpp"
#include "dll/hook/hooks/WindowHook.hpp"

namespace velyx {
namespace {
constexpr const char* kLog = "Settings";
}

Setting& Settings::add(Setting setting) {
    if (!setting.id.empty() && has(setting.id)) {

        throw std::runtime_error("duplicate setting id: " + setting.id);
    }

    settings_.push_back(std::move(setting));
    return settings_.back();
}

Setting& Settings::toggle(std::string id, std::string label, bool value, std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Toggle;
    setting.value = value;
    setting.fallback = value;
    return add(std::move(setting));
}

Setting& Settings::slider(std::string id, std::string label, float value, float min, float max,
                          std::string description, std::string unit) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Slider;
    setting.value = value;
    setting.fallback = value;
    setting.min = min;
    setting.max = max;
    setting.unit = std::move(unit);
    return add(std::move(setting));
}

Setting& Settings::intSlider(std::string id, std::string label, int value, int min, int max,
                             std::string description, std::string unit) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::IntSlider;
    setting.value = value;
    setting.fallback = value;
    setting.min = static_cast<float>(min);
    setting.max = static_cast<float>(max);
    setting.step = 1.f;
    setting.unit = std::move(unit);
    return add(std::move(setting));
}

Setting& Settings::text(std::string id, std::string label, std::string value,
                        std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Text;
    setting.value = value;
    setting.fallback = std::move(value);
    return add(std::move(setting));
}

Setting& Settings::dropdown(std::string id, std::string label, std::string value,
                            std::vector<std::string> options, std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Dropdown;
    setting.value = value;
    setting.fallback = std::move(value);
    setting.options = std::move(options);
    return add(std::move(setting));
}

Setting& Settings::color(std::string id, std::string label, Color value, std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Color;
    setting.value = value;
    setting.fallback = value;
    return add(std::move(setting));
}

Setting& Settings::keybind(std::string id, std::string label, Keybind value,
                           std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Keybind;
    setting.value = value;
    setting.fallback = value;
    return add(std::move(setting));
}

Setting& Settings::position(std::string id, std::string label, Vec2 value,
                            std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Position;
    setting.value = value;
    setting.fallback = value;
    return add(std::move(setting));
}

Setting& Settings::header(std::string label) {
    Setting setting;
    setting.label = std::move(label);
    setting.type = SettingType::Header;
    return add(std::move(setting));
}

Setting& Settings::button(std::string id, std::string label, std::function<void()> action,
                          std::string description) {
    Setting setting;
    setting.id = std::move(id);
    setting.label = std::move(label);
    setting.description = std::move(description);
    setting.type = SettingType::Button;
    setting.action = std::move(action);
    return add(std::move(setting));
}

Setting* Settings::find(std::string_view id) {
    const auto it = std::ranges::find_if(settings_, [&](const Setting& s) { return s.id == id; });
    return it == settings_.end() ? nullptr : &*it;
}

const Setting* Settings::find(std::string_view id) const {
    const auto it = std::ranges::find_if(settings_, [&](const Setting& s) { return s.id == id; });
    return it == settings_.end() ? nullptr : &*it;
}

bool Settings::set(std::string_view id, const SettingValue& value) {
    Setting* setting = find(id);
    if (!setting) return false;

    if (setting->value.index() != value.index()) {
        Log::warn(kLog, "type mismatch writing '{}'", id);
        return false;
    }

    setting->value = value;
    if (setting->onChange) setting->onChange();
    return true;
}

void Settings::resetAll() {
    for (Setting& setting : settings_) {
        if (!setting.hasValue()) continue;
        setting.reset();
        if (setting.onChange) setting.onChange();
    }
}

nlohmann::json toJson(const SettingValue& value) {
    return std::visit(
        [](const auto& held) -> nlohmann::json {
            using T = std::decay_t<decltype(held)>;

            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, Color>) {
                return held.toHex(true);
            } else if constexpr (std::is_same_v<T, Keybind>) {
                nlohmann::json json;
                json["key"] = held.key;
                json["ctrl"] = held.ctrl;
                json["shift"] = held.shift;
                json["alt"] = held.alt;
                json["mode"] = static_cast<int>(held.mode);
                return json;
            } else if constexpr (std::is_same_v<T, Vec2>) {
                return nlohmann::json{{"x", held.x}, {"y", held.y}};
            } else {
                return held;
            }
        },
        value);
}

SettingValue fromJson(const nlohmann::json& json, const SettingValue& fallback) {
    return std::visit(
        [&](const auto& held) -> SettingValue {
            using T = std::decay_t<decltype(held)>;

            if constexpr (std::is_same_v<T, std::monostate>) {
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, Color>) {
                if (!json.is_string()) return held;
                return Color::fromHex(json.get<std::string>(), held);
            } else if constexpr (std::is_same_v<T, Keybind>) {
                if (!json.is_object()) return held;
                Keybind bind = held;
                bind.key = json.value("key", bind.key);
                bind.ctrl = json.value("ctrl", bind.ctrl);
                bind.shift = json.value("shift", bind.shift);
                bind.alt = json.value("alt", bind.alt);
                bind.mode = static_cast<Keybind::Mode>(
                    json.value("mode", static_cast<int>(bind.mode)));
                return bind;
            } else if constexpr (std::is_same_v<T, Vec2>) {
                if (!json.is_object()) return held;
                return Vec2{json.value("x", held.x), json.value("y", held.y)};
            } else if constexpr (std::is_same_v<T, bool>) {
                return json.is_boolean() ? json.get<bool>() : held;
            } else if constexpr (std::is_same_v<T, int>) {
                return json.is_number() ? json.get<int>() : held;
            } else if constexpr (std::is_same_v<T, float>) {
                return json.is_number() ? json.get<float>() : held;
            } else {
                return json.is_string() ? json.get<std::string>() : held;
            }
        },
        fallback);
}

nlohmann::json Settings::save() const {
    nlohmann::json json = nlohmann::json::object();

    for (const Setting& setting : settings_) {
        if (!setting.hasValue() || setting.id.empty()) continue;
        json[setting.id] = toJson(setting.value);
    }

    return json;
}

void Settings::load(const nlohmann::json& json) {
    if (!json.is_object()) return;

    for (Setting& setting : settings_) {
        if (!setting.hasValue() || setting.id.empty()) continue;

        const auto it = json.find(setting.id);
        if (it == json.end()) continue;

        setting.value = fromJson(*it, setting.value);

        if (setting.type == SettingType::Slider) {
            if (auto* held = std::get_if<float>(&setting.value)) {
                *held = clamp(*held, setting.min, setting.max);
            }
        } else if (setting.type == SettingType::IntSlider) {
            if (auto* held = std::get_if<int>(&setting.value)) {
                *held = clamp(*held, static_cast<int>(setting.min), static_cast<int>(setting.max));
            }
        } else if (setting.type == SettingType::Dropdown) {
            if (auto* held = std::get_if<std::string>(&setting.value)) {
                if (!setting.options.empty() &&
                    std::ranges::find(setting.options, *held) == setting.options.end()) {
                    *held = std::get<std::string>(setting.fallback);
                }
            }
        }

        if (setting.onChange) setting.onChange();
    }
}

std::string describeKeybind(const Keybind& bind) {
    if (!bind.bound()) return "Non assignée";

    std::string label;
    if (bind.ctrl) label += "Ctrl + ";
    if (bind.shift) label += "Maj + ";
    if (bind.alt) label += "Alt + ";
    label += WindowHook::keyName(bind.key);

    return label;
}

}
