#include "TextHud.hpp"

#include <algorithm>

#include "dll/ui/Theme.hpp"

namespace velyx {

TextHud::TextHud(std::string id, std::string name, std::string description, Vec2 defaultPosition,
                 HudAnchor defaultAnchor)
    : HudModule(std::move(id), std::move(name), std::move(description), defaultPosition,
                defaultAnchor) {}

void TextHud::addTextSettings(bool defaultShowLabel) {
    settings.header("Content");
    settings.toggle("showLabel", "Show labels", defaultShowLabel);
    settings.toggle("labelAccent", "Labels in the accent colour", true);
    settings.dropdown("textAlign", "Alignment", "Left", {"Left", "Centre", "Right"});
    settings.slider("lineSpacing", "Line spacing", 1.15f, 0.8f, 2.f, "", "x");

    if (Setting* accent = settings.find("labelAccent")) {
        accent->visibleWhen = [this] { return settings.value<bool>("showLabel", true); };
    }
}

float TextHud::lineSpacing() const { return settings.value<float>("lineSpacing", 1.15f); }

Vec2 TextHud::contentSize(Renderer& renderer) {
    cached_ = rows();
    if (cached_.empty()) {
        cachedSize_ = {};
        return cachedSize_;
    }

    const FontSpec labelSpec = fontFor(1.f, FontWeight::SemiBold);
    const FontSpec valueSpec = fontFor(1.f, FontWeight::Medium);
    const bool showLabel = settings.value<bool>("showLabel", true);

    float width = 0.f;
    float height = 0.f;
    const float lineHeight = valueSpec.size * lineSpacing();

    for (const Row& row : cached_) {
        float rowWidth = 0.f;

        if (showLabel && !row.label.empty()) {
            rowWidth += renderer.measure(row.label + " ", labelSpec).x;
        }
        rowWidth += renderer.measure(row.value, valueSpec).x;

        width = std::max(width, rowWidth);
        height += lineHeight;
    }

    cachedSize_ = {std::ceil(width), std::ceil(height)};
    return cachedSize_;
}

void TextHud::drawContent(Renderer& renderer, const Rect& content) {
    if (cached_.empty()) return;

    const auto& active = theme();
    const FontSpec labelSpec = fontFor(1.f, FontWeight::SemiBold);
    const FontSpec valueSpec = fontFor(1.f, FontWeight::Medium);

    const bool showLabel = settings.value<bool>("showLabel", true);
    const bool labelAccent = settings.value<bool>("labelAccent", true);
    const bool shadow = settings.value<bool>("textShadow", false);
    const std::string align = settings.value<std::string>("textAlign", "Left");

    const Color baseText = textColor();
    const Color labelColor = labelAccent ? active.liveAccent() : baseText.fade(0.7f);

    const float lineHeight = valueSpec.size * lineSpacing();
    float y = content.top;

    for (const Row& row : cached_) {
        const bool hasLabel = showLabel && !row.label.empty();
        const std::string labelText = hasLabel ? row.label + " " : std::string{};

        const float labelWidth = hasLabel ? renderer.measure(labelText, labelSpec).x : 0.f;
        const float valueWidth = renderer.measure(row.value, valueSpec).x;
        const float rowWidth = labelWidth + valueWidth;

        float x = content.left;
        if (align == "Centre") {
            x = content.left + (content.width() - rowWidth) * 0.5f;
        } else if (align == "Right") {
            x = content.right - rowWidth;
        }

        const Rect line{x, y, content.right, y + lineHeight};

        if (hasLabel) {
            const Rect labelRect{x, y, x + labelWidth + 1.f, line.bottom};
            if (shadow) {
                renderer.textShadowed(labelText, labelRect, labelColor, labelSpec);
            } else {
                renderer.text(labelText, labelRect, labelColor, labelSpec);
            }
        }

        const Color valueColor = row.valueColor.a > 0.f ? row.valueColor : baseText;
        const Rect valueRect{x + labelWidth, y, content.right, line.bottom};

        if (shadow) {
            renderer.textShadowed(row.value, valueRect, valueColor, valueSpec);
        } else {
            renderer.text(row.value, valueRect, valueColor, valueSpec);
        }

        y += lineHeight;
    }
}

}
