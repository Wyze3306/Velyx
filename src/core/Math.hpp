#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace velyx {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;

    constexpr Vec2() = default;
    constexpr Vec2(float x, float y) : x(x), y(y) {}

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }

    [[nodiscard]] float length() const { return std::sqrt(x * x + y * y); }
    [[nodiscard]] float distanceTo(const Vec2& o) const { return (*this - o).length(); }
};

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    constexpr Vec3() = default;
    constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }

    [[nodiscard]] float length() const { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] float distanceTo(const Vec3& o) const { return (*this - o).length(); }

    [[nodiscard]] float flatDistanceTo(const Vec3& o) const {
        const float dx = x - o.x;
        const float dz = z - o.z;
        return std::sqrt(dx * dx + dz * dz);
    }
};

struct Rect {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;

    constexpr Rect() = default;
    constexpr Rect(float l, float t, float r, float b) : left(l), top(t), right(r), bottom(b) {}

    static constexpr Rect fromSize(float x, float y, float w, float h) {
        return {x, y, x + w, y + h};
    }

    [[nodiscard]] constexpr float width() const { return right - left; }
    [[nodiscard]] constexpr float height() const { return bottom - top; }
    [[nodiscard]] constexpr Vec2 topLeft() const { return {left, top}; }
    [[nodiscard]] constexpr Vec2 size() const { return {width(), height()}; }
    [[nodiscard]] constexpr Vec2 center() const {
        return {left + width() * 0.5f, top + height() * 0.5f};
    }

    [[nodiscard]] constexpr bool contains(const Vec2& p) const {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    }

    [[nodiscard]] constexpr bool intersects(const Rect& o) const {
        return left < o.right && right > o.left && top < o.bottom && bottom > o.top;
    }

    [[nodiscard]] constexpr Rect inflated(float amount) const {
        return {left - amount, top - amount, right + amount, bottom + amount};
    }

    [[nodiscard]] constexpr Rect translated(const Vec2& d) const {
        return {left + d.x, top + d.y, right + d.x, bottom + d.y};
    }
};

inline constexpr float kPi = std::numbers::pi_v<float>;

template <typename T>
constexpr T clamp(T value, T lo, T hi) {
    return std::max(lo, std::min(hi, value));
}

constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

constexpr Vec2 lerp(const Vec2& a, const Vec2& b, float t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
}

constexpr float remap(float value, float inMin, float inMax, float outMin, float outMax) {
    if (inMax == inMin) return outMin;
    const float t = clamp((value - inMin) / (inMax - inMin), 0.f, 1.f);
    return lerp(outMin, outMax, t);
}

constexpr float toRadians(float degrees) { return degrees * (kPi / 180.f); }
constexpr float toDegrees(float radians) { return radians * (180.f / kPi); }

inline float wrapAngle(float degrees) {
    degrees = std::fmod(degrees + 180.f, 360.f);
    if (degrees < 0.f) degrees += 360.f;
    return degrees - 180.f;
}

enum class Easing {
    Linear,
    OutQuad,
    OutCubic,
    OutQuart,
    OutExpo,
    InOutQuad,
    InOutCubic,
    OutBack,
    OutElastic,
};

inline float ease(Easing type, float t) {
    t = clamp(t, 0.f, 1.f);
    switch (type) {
        case Easing::Linear:
            return t;
        case Easing::OutQuad:
            return 1.f - (1.f - t) * (1.f - t);
        case Easing::OutCubic:
            return 1.f - std::pow(1.f - t, 3.f);
        case Easing::OutQuart:
            return 1.f - std::pow(1.f - t, 4.f);
        case Easing::OutExpo:
            return t >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * t);
        case Easing::InOutQuad:
            return t < 0.5f ? 2.f * t * t : 1.f - std::pow(-2.f * t + 2.f, 2.f) / 2.f;
        case Easing::InOutCubic:
            return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
        case Easing::OutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.f;
            return 1.f + c3 * std::pow(t - 1.f, 3.f) + c1 * std::pow(t - 1.f, 2.f);
        }
        case Easing::OutElastic: {
            if (t == 0.f || t == 1.f) return t;
            constexpr float c4 = (2.f * kPi) / 3.f;
            return std::pow(2.f, -10.f * t) * std::sin((t * 10.f - 0.75f) * c4) + 1.f;
        }
    }
    return t;
}

inline float approach(float current, float target, float deltaSeconds, float speed) {
    if (deltaSeconds <= 0.f) return current;
    const float factor = 1.f - std::exp(-speed * deltaSeconds);
    return current + (target - current) * factor;
}

inline Vec2 approach(const Vec2& current, const Vec2& target, float deltaSeconds, float speed) {
    return {approach(current.x, target.x, deltaSeconds, speed),
            approach(current.y, target.y, deltaSeconds, speed)};
}

class Animated {
public:
    Animated() = default;
    explicit Animated(float initial, float speed = 12.f)
        : value(initial), target(initial), speed(speed) {}

    float value = 0.f;
    float target = 0.f;
    float speed = 12.f;

    void set(float v) { value = target = v; }
    void to(float v) { target = v; }
    void update(float deltaSeconds) { value = approach(value, target, deltaSeconds, speed); }
    [[nodiscard]] bool settled(float epsilon = 0.001f) const {
        return std::abs(target - value) < epsilon;
    }
};

}
