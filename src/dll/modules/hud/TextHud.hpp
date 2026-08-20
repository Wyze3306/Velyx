#pragma once

#include <string>
#include <utility>
#include <vector>

#include "dll/module/HudModule.hpp"

namespace velyx {

class TextHud : public HudModule {
public:
    struct Row {
        std::string label;
        std::string value;
        Color valueColor{};
    };

    TextHud(std::string id, std::string name, std::string description, Vec2 defaultPosition,
            HudAnchor defaultAnchor = HudAnchor::TopLeft);

    virtual std::vector<Row> rows() = 0;

    Vec2 contentSize(Renderer& renderer) override;
    void drawContent(Renderer& renderer, const Rect& content) override;

protected:

    void addTextSettings(bool defaultShowLabel = true);

    [[nodiscard]] float lineSpacing() const;

private:
    std::vector<Row> cached_;
    Vec2 cachedSize_;
};

}
