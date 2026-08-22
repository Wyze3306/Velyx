#pragma once

#include <array>
#include <string_view>

namespace velyx {

// A Direct2D colour matrix: four rows of coefficients and one of offsets, in the
// order the effect expects them. Screen-wide grading is the one thing the overlay can
// do to the game's picture without a single game signature, so more than one module
// wants these and none of them should be writing them out again.
using ColorMatrix = std::array<float, 20>;

inline constexpr ColorMatrix kIdentityMatrix{1, 0, 0, 0,
                                             0, 1, 0, 0,
                                             0, 0, 1, 0,
                                             0, 0, 0, 1,
                                             0, 0, 0, 0};

ColorMatrix multiply(const ColorMatrix& a, const ColorMatrix& b);

ColorMatrix saturationMatrix(float amount);
ColorMatrix contrastMatrix(float amount);
ColorMatrix warmthMatrix(float amount);
ColorMatrix colourBlindMatrix(std::string_view kind);

// Raises the shadows without touching what is already bright: out = in * (1 - lift) +
// lift. It is what a gamma slider is reaching for, within what a linear matrix can do.
ColorMatrix liftMatrix(float lift);

ColorMatrix gainMatrix(float gain);

}
