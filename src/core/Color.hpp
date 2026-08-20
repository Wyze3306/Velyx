#pragma once

#include <cstdint>
#include <string>

#include "Math.hpp"

namespace velyx {

struct Color {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;

    constexpr Color() = default;
    constexpr Color(float r, float g, float b, float a = 1.f) : r(r), g(g), b(b), a(a) {}

    static constexpr Color rgb8(int r, int g, int b, int a = 255) {
        return {static_cast<float>(r) / 255.f, static_cast<float>(g) / 255.f,
                static_cast<float>(b) / 255.f, static_cast<float>(a) / 255.f};
    }

    static Color fromHex(std::string_view hex, Color fallback = Color{1.f, 1.f, 1.f, 1.f});

    static Color fromHsv(float h, float s, float v, float a = 1.f);

    [[nodiscard]] std::string toHex(bool includeAlpha = false) const;

    void toHsv(float& h, float& s, float& v) const;

    [[nodiscard]] constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }

    [[nodiscard]] constexpr Color fade(float factor) const { return {r, g, b, a * factor}; }

    [[nodiscard]] Color lighten(float amount) const;
    [[nodiscard]] Color darken(float amount) const;

    [[nodiscard]] constexpr float luminance() const {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    [[nodiscard]] constexpr Color readableForeground() const {
        return luminance() > 0.55f ? Color{0.04f, 0.09f, 0.07f, a} : Color{1.f, 1.f, 1.f, a};
    }

    [[nodiscard]] constexpr uint32_t toArgb() const {
        return (static_cast<uint32_t>(clamp(a, 0.f, 1.f) * 255.f) << 24) |
               (static_cast<uint32_t>(clamp(r, 0.f, 1.f) * 255.f) << 16) |
               (static_cast<uint32_t>(clamp(g, 0.f, 1.f) * 255.f) << 8) |
               (static_cast<uint32_t>(clamp(b, 0.f, 1.f) * 255.f));
    }

    constexpr bool operator==(const Color& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
};

constexpr Color lerp(const Color& x, const Color& y, float t) {
    return {lerp(x.r, y.r, t), lerp(x.g, y.g, t), lerp(x.b, y.b, t), lerp(x.a, y.a, t)};
}

Color rainbow(float speed = 1.f, float saturation = 0.75f, float value = 1.f, float offset = 0.f);

namespace palette {

inline constexpr Color kVoid = Color::rgb8(6, 16, 12);
inline constexpr Color kForest = Color::rgb8(11, 31, 23);
inline constexpr Color kMoss = Color::rgb8(18, 48, 31);
inline constexpr Color kSage = Color::rgb8(30, 71, 48);
inline constexpr Color kJade = Color::rgb8(45, 168, 106);
inline constexpr Color kMint = Color::rgb8(61, 220, 132);
inline constexpr Color kGlow = Color::rgb8(124, 255, 178);
inline constexpr Color kSnow = Color::rgb8(244, 250, 246);
inline constexpr Color kAsh = Color::rgb8(163, 184, 172);
inline constexpr Color kEmber = Color::rgb8(232, 96, 82);
inline constexpr Color kHoney = Color::rgb8(240, 186, 84);

}

}
