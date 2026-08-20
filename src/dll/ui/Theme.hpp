#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Color.hpp"
#include "core/Math.hpp"

namespace velyx {

struct Theme {
    std::string name = "Velyx";
    std::string author = "Velyx";
    std::string description;

    Color background = palette::kSlate;
    Color backgroundDeep = palette::kInk;
    Color surface = palette::kGraphite;
    Color surfaceHover = palette::kSteel;
    Color border = palette::kLine;
    Color accent = palette::kMint;
    Color accentDeep = palette::kJade;
    Color accentGlow = palette::kGlow;
    Color text = palette::kSnow;
    Color textMuted = palette::kAsh;
    Color textDim = palette::kDim;
    Color danger = palette::kEmber;
    Color warning = palette::kHoney;
    Color success = palette::kMint;

    bool rgbAccent = false;
    float rgbSpeed = 1.f;

    float radius = 9.f;
    float panelRadius = 14.f;
    float borderWidth = 1.f;
    float spacing = 8.f;

    std::string fontFamily = "Space Grotesk";
    std::string monoFamily = "JetBrains Mono";
    float fontScale = 1.f;
    float letterSpacing = 0.f;

    bool blur = true;
    float blurSigma = 9.f;
    bool shadows = true;
    float shadowSpread = 14.f;
    Color shadowColor = Color::rgb8(0, 0, 0, 150);
    float panelOpacity = 0.96f;

    float animationSpeed = 1.f;
    Easing easing = Easing::OutCubic;

    [[nodiscard]] Color liveAccent() const;
    [[nodiscard]] Color liveAccentDeep() const;

    [[nodiscard]] Color onAccent() const { return liveAccent().readableForeground(); }

    [[nodiscard]] float motion(float base = 12.f) const;

    [[nodiscard]] float scaled(float size) const { return size * fontScale; }
};

class ThemeManager {
public:
    static ThemeManager& get();

    void load();

    [[nodiscard]] const Theme& current() const { return current_; }
    [[nodiscard]] Theme& mutableCurrent() { return current_; }

    bool apply(const std::string& name);

    bool save(const Theme& theme);

    bool remove(const std::string& name);

    bool exportTo(const std::string& name, const std::filesystem::path& destination) const;
    bool importFrom(const std::filesystem::path& source, std::string* importedName = nullptr);

    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] const std::vector<Theme>& all() const { return themes_; }
    [[nodiscard]] static bool isBuiltIn(const std::string& name);

    [[nodiscard]] static const Theme& active() { return get().current(); }

private:
    ThemeManager() = default;

    void registerBuiltIns();
    [[nodiscard]] const Theme* find(const std::string& name) const;

    std::vector<Theme> themes_;
    Theme current_;
};

inline const Theme& theme() { return ThemeManager::active(); }

}
