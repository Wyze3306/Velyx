#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Color.hpp"
#include "core/Math.hpp"
#include "dll/event/Events.hpp"
#include "dll/module/Setting.hpp"
#include "dll/render/Renderer.hpp"

namespace velyx {

struct UiId {
    uint32_t value = 0;

    UiId() = default;
    UiId(std::string_view name, int index = 0);

    bool operator==(const UiId& other) const { return value == other.value; }
    explicit operator bool() const { return value != 0; }
};

class Ui {
public:
    static Ui& get();

    void beginFrame(Renderer& renderer, float deltaSeconds);
    void endFrame();

    [[nodiscard]] Renderer& renderer() const { return *renderer_; }
    [[nodiscard]] float delta() const { return delta_; }

    void feedMouse(const MouseEvent& event);
    void feedKey(const KeyEvent& event);
    void feedChar(const CharEvent& event);

    [[nodiscard]] bool capturingText() const { return focusedText_; }

    [[nodiscard]] Vec2 mouse() const { return mouse_; }
    [[nodiscard]] bool mouseDown() const { return mouseDown_; }
    [[nodiscard]] bool clicked() const { return clicked_; }

    void pushModal(const Rect& rect);
    void popModal();

    void panel(const Rect& rect, float radius = -1.f, bool blur = true);

    void text(std::string_view value, const Rect& bounds, const Color& color, float size,
              FontWeight weight = FontWeight::Medium, TextAlign align = TextAlign::Left);

    void sectionHeader(std::string_view label, const Rect& bounds);

    bool button(const UiId& id, const Rect& rect, std::string_view label, bool primary = false,
                bool enabled = true);

    bool iconButton(const UiId& id, const Rect& rect, std::string_view glyph,
                    const Color& tint = Color{});

    bool toggle(const UiId& id, const Rect& rect, bool& value);

    bool toggleRow(const UiId& id, const Rect& rect, std::string_view label,
                   std::string_view description, bool& value);

    bool slider(const UiId& id, const Rect& rect, float& value, float minimum, float maximum,
                std::string_view suffix = {}, float step = 0.f);

    bool sliderRow(const UiId& id, const Rect& rect, std::string_view label,
                   std::string_view description, float& value, float minimum, float maximum,
                   std::string_view suffix = {}, float step = 0.f);

    bool dropdown(const UiId& id, const Rect& rect, std::string& value,
                  const std::vector<std::string>& options);

    bool textField(const UiId& id, const Rect& rect, std::string& value,
                   std::string_view placeholder = {}, size_t maxLength = 64);

    bool colorPicker(const UiId& id, const Rect& rect, Color& value, bool withAlpha = true);

    bool colorRow(const UiId& id, const Rect& rect, std::string_view label, Color& value);

    bool keybindField(const UiId& id, const Rect& rect, Keybind& value);

    int segmented(const UiId& id, const Rect& rect, const std::vector<std::string>& options,
                  int selected);

    float beginScroll(const UiId& id, const Rect& rect, float contentHeight);
    void endScroll();

    float animate(const UiId& id, bool active, float speed = 16.f);

    [[nodiscard]] bool hovered(const UiId& id) const { return hot_ == id; }

private:
    Ui() = default;

    [[nodiscard]] bool interactable(const Rect& rect) const;
    bool hoverAndClick(const UiId& id, const Rect& rect, bool enabled = true);

    Renderer* renderer_ = nullptr;
    float delta_ = 0.f;

    Vec2 mouse_;
    Vec2 mouseDelta_;
    bool mouseDown_ = false;
    bool clicked_ = false;
    bool released_ = false;
    float wheel_ = 0.f;

    UiId hot_;
    UiId active_;
    UiId focused_;
    UiId capturingKeybind_;
    UiId openDropdown_;
    UiId openPicker_;

    bool focusedText_ = false;
    std::string pendingChars_;
    std::vector<int> pendingKeys_;

    std::vector<Rect> modalStack_;

    struct ScrollState {
        float offset = 0.f;
        float target = 0.f;
        float contentHeight = 0.f;
        Rect view;
    };
    std::unordered_map<uint32_t, ScrollState> scrolls_;
    std::vector<UiId> scrollStack_;

    std::unordered_map<uint32_t, Animated> animations_;
    std::unordered_map<uint32_t, float> pickerHue_;
};

inline Ui& ui() { return Ui::get(); }

}
