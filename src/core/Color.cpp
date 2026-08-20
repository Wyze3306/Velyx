#include "Color.hpp"

#include <chrono>
#include <cstdio>

namespace velyx {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}

Color Color::fromHex(std::string_view hex, Color fallback) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);

    for (const char c : hex) {
        if (hexDigit(c) < 0) return fallback;
    }

    const auto pair = [&](size_t index) {
        return static_cast<float>(hexDigit(hex[index]) * 16 + hexDigit(hex[index + 1])) / 255.f;
    };
    const auto single = [&](size_t index) {
        const int v = hexDigit(hex[index]);
        return static_cast<float>(v * 16 + v) / 255.f;
    };

    switch (hex.size()) {
        case 3:
            return {single(0), single(1), single(2), 1.f};
        case 4:
            return {single(0), single(1), single(2), single(3)};
        case 6:
            return {pair(0), pair(2), pair(4), 1.f};
        case 8:
            return {pair(0), pair(2), pair(4), pair(6)};
        default:
            return fallback;
    }
}

Color Color::fromHsv(float h, float s, float v, float a) {
    h = std::fmod(h, 360.f);
    if (h < 0.f) h += 360.f;
    s = clamp(s, 0.f, 1.f);
    v = clamp(v, 0.f, 1.f);

    const float c = v * s;
    const float x = c * (1.f - std::abs(std::fmod(h / 60.f, 2.f) - 1.f));
    const float m = v - c;

    float r = 0.f, g = 0.f, b = 0.f;
    if (h < 60.f)       { r = c; g = x; }
    else if (h < 120.f) { r = x; g = c; }
    else if (h < 180.f) { g = c; b = x; }
    else if (h < 240.f) { g = x; b = c; }
    else if (h < 300.f) { r = x; b = c; }
    else                { r = c; b = x; }

    return {r + m, g + m, b + m, a};
}

std::string Color::toHex(bool includeAlpha) const {
    const auto byte = [](float v) { return static_cast<int>(clamp(v, 0.f, 1.f) * 255.f + 0.5f); };

    char buffer[10]{};
    if (includeAlpha) {
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", byte(r), byte(g), byte(b), byte(a));
    } else {
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", byte(r), byte(g), byte(b));
    }
    return buffer;
}

void Color::toHsv(float& h, float& s, float& v) const {
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float delta = maxC - minC;

    v = maxC;
    s = maxC <= 0.f ? 0.f : delta / maxC;

    if (delta <= 0.f) {
        h = 0.f;
        return;
    }

    if (maxC == r)      h = 60.f * std::fmod((g - b) / delta, 6.f);
    else if (maxC == g) h = 60.f * (((b - r) / delta) + 2.f);
    else                h = 60.f * (((r - g) / delta) + 4.f);

    if (h < 0.f) h += 360.f;
}

Color Color::lighten(float amount) const {
    float h, s, v;
    toHsv(h, s, v);
    return fromHsv(h, s * (1.f - amount * 0.35f), clamp(v + amount, 0.f, 1.f), a);
}

Color Color::darken(float amount) const {
    float h, s, v;
    toHsv(h, s, v);
    return fromHsv(h, s, clamp(v - amount, 0.f, 1.f), a);
}

Color rainbow(float speed, float saturation, float value, float offset) {
    using namespace std::chrono;
    const auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    const float hue = std::fmod(static_cast<float>(now) * 0.036f * speed + offset * 360.f, 360.f);
    return Color::fromHsv(hue, saturation, value);
}

}
