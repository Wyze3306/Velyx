#pragma once

#include <array>

#include "core/Math.hpp"
#include "dll/event/Events.hpp"

namespace velyx::sdk {

// Turns a world position into a screen one, which is the whole of what an overlay
// needs from the renderer it is drawing over.
//
// Two ways to get there, in order of preference:
//
//  * the game's own view-projection matrix, when `ClientInstance::viewMatrix` is in
//    the signature pack. Exact, third person and every camera effect included;
//  * one derived from the player's eye, rotation and an assumed field of view. Needs
//    nothing the coordinates HUD does not already read, which is why the boxes work
//    on a pack that only knows `Actor::position`.
//
// `exact()` says which one answered, so an interface can say so rather than leaving
// someone to wonder why a box sits a few pixels off in third person.
class Camera {
public:
    static Camera& get();

    void requireSignatures();

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] bool exact() const { return exact_; }

    [[nodiscard]] Vec3 origin() const { return origin_; }
    [[nodiscard]] Vec3 forward() const { return forward_; }
    [[nodiscard]] Vec3 right() const { return right_; }
    [[nodiscard]] Vec3 up() const { return up_; }
    [[nodiscard]] float fieldOfView() const { return fov_; }

    // The derived path only. Ignored the moment the game's matrix is available.
    void setCalibration(float fieldOfView, float eyeHeight);

    // Screen pixels. False when the point is behind the camera or off in a way that
    // would project to a mirrored position in front of it.
    bool project(const Vec3& world, Vec2& screen) const;

    // The screen rectangle covering an axis-aligned world box, from its eight corners.
    // False as soon as one corner is behind the camera: a box straddling the near
    // plane has no honest rectangle, and half of one is worse than none.
    bool projectBox(const Vec3& minimum, const Vec3& maximum, Rect& screen) const;

    // Degrees between where the camera looks and where a point is, which is what
    // "under the crosshair" means when there is no ray to cast.
    [[nodiscard]] float angleTo(const Vec3& world) const;

private:
    Camera() = default;

    friend void bindWorld();
    void onFrame(FrameEvent& event);

    bool readGameMatrix();
    void derive();

    std::array<float, 16> matrix_{};
    Vec3 origin_;
    Vec3 forward_{0.f, 0.f, 1.f};
    Vec3 right_{1.f, 0.f, 0.f};
    Vec3 up_{0.f, 1.f, 0.f};

    Vec2 screenSize_;
    float fov_ = 70.f;
    float eyeHeight_ = 1.62f;

    bool valid_ = false;
    bool exact_ = false;
};

inline Camera& camera() { return Camera::get(); }

}
