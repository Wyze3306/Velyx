#include "ColorMatrix.hpp"

namespace velyx {

ColorMatrix multiply(const ColorMatrix& a, const ColorMatrix& b) {
    ColorMatrix out{};

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a[row * 4 + k] * b[k * 4 + column];
            out[row * 4 + column] = sum;
        }
    }

    for (int column = 0; column < 4; ++column) {
        float sum = b[16 + column];
        for (int k = 0; k < 4; ++k) sum += a[16 + k] * b[k * 4 + column];
        out[16 + column] = sum;
    }

    return out;
}

ColorMatrix saturationMatrix(float amount) {
    constexpr float lr = 0.2126f;
    constexpr float lg = 0.7152f;
    constexpr float lb = 0.0722f;

    const float inverse = 1.f - amount;

    return ColorMatrix{lr * inverse + amount, lr * inverse,          lr * inverse,          0,
                       lg * inverse,          lg * inverse + amount, lg * inverse,          0,
                       lb * inverse,          lb * inverse,          lb * inverse + amount, 0,
                       0,                     0,                     0,                     1,
                       0,                     0,                     0,                     0};
}

ColorMatrix contrastMatrix(float amount) {
    const float offset = 0.5f * (1.f - amount);

    return ColorMatrix{amount, 0, 0, 0,
                       0, amount, 0, 0,
                       0, 0, amount, 0,
                       0, 0, 0, 1,
                       offset, offset, offset, 0};
}

ColorMatrix warmthMatrix(float amount) {
    const float blue = 1.f + (0.62f - 1.f) * amount;
    const float green = 1.f + (0.88f - 1.f) * amount;

    return ColorMatrix{1, 0, 0, 0,
                       0, green, 0, 0,
                       0, 0, blue, 0,
                       0, 0, 0, 1,
                       0, 0, 0, 0};
}

ColorMatrix colourBlindMatrix(std::string_view kind) {
    if (kind == "Protanopia") {
        return ColorMatrix{0.567f, 0.558f, 0.f,    0,
                           0.433f, 0.442f, 0.242f, 0,
                           0.f,    0.f,    0.758f, 0,
                           0, 0, 0, 1,
                           0, 0, 0, 0};
    }
    if (kind == "Deuteranopia") {
        return ColorMatrix{0.625f, 0.70f, 0.f,   0,
                           0.375f, 0.30f, 0.30f, 0,
                           0.f,    0.f,   0.70f, 0,
                           0, 0, 0, 1,
                           0, 0, 0, 0};
    }
    if (kind == "Tritanopia") {
        return ColorMatrix{0.95f, 0.f,    0.f,    0,
                           0.05f, 0.433f, 0.475f, 0,
                           0.f,   0.567f, 0.525f, 0,
                           0, 0, 0, 1,
                           0, 0, 0, 0};
    }
    return kIdentityMatrix;
}

ColorMatrix liftMatrix(float lift) {
    const float scale = 1.f - lift;

    return ColorMatrix{scale, 0, 0, 0,
                       0, scale, 0, 0,
                       0, 0, scale, 0,
                       0, 0, 0, 1,
                       lift, lift, lift, 0};
}

ColorMatrix gainMatrix(float gain) {
    return ColorMatrix{gain, 0, 0, 0,
                       0, gain, 0, 0,
                       0, 0, gain, 0,
                       0, 0, 0, 1,
                       0, 0, 0, 0};
}

}
