#pragma once

#include "core/Math.hpp"
#include "dll/module/Module.hpp"
#include "dll/render/Font.hpp"
#include "dll/render/Renderer.hpp"

namespace velyx {

enum class HudAnchor {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
};

const char* anchorLabel(HudAnchor anchor);
HudAnchor anchorFromLabel(std::string_view label);

class HudModule : public Module {
public:
    HudModule(std::string id, std::string name, std::string description,
              Vec2 defaultPosition = {0.02f, 0.02f}, HudAnchor defaultAnchor = HudAnchor::TopLeft);

    virtual Vec2 contentSize(Renderer& renderer) = 0;

    virtual void drawContent(Renderer& renderer, const Rect& content) = 0;

    [[nodiscard]] virtual bool relevantNow() const { return true; }

    [[nodiscard]] virtual std::string editorPreview() const { return name(); }

    [[nodiscard]] Rect bounds() const { return lastBounds_; }

    void moveTo(Vec2 screenPosition, Vec2 screenSize);

    void reanchorToNearestEdge(Vec2 screenSize);

    [[nodiscard]] Vec2 normalisedPosition() const;
    [[nodiscard]] HudAnchor anchor() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] float rotation() const;
    [[nodiscard]] float elementOpacity() const;
    [[nodiscard]] std::string group() const;

    [[nodiscard]] bool hiddenInScreenshots() const;

    [[nodiscard]] FontSpec fontFor(float sizeMultiplier = 1.f,
                                   FontWeight weight = FontWeight::Medium) const;

    [[nodiscard]] Color textColor() const;

protected:

    void addLayoutSettings(Vec2 defaultPosition, HudAnchor defaultAnchor);

    [[nodiscard]] float padding() const;

private:
    void onRender(RenderEvent& event);

    [[nodiscard]] Rect computeBounds(Vec2 size, Vec2 screenSize) const;

    Rect lastBounds_;
    Animated fade_{0.f, 12.f};
};

}
