#include "Camera.hpp"

#include <cmath>
#include <cstring>

#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx::sdk {
namespace {

constexpr const char* kViewMatrix = "ClientInstance::viewMatrix";
constexpr const char* kCameraOrigin = "ClientInstance::cameraOrigin";
constexpr const char* kFieldOfView = "ClientInstance::fieldOfView";

// Minecraft's own convention, and the reason nothing here looks like a textbook
// look-at: yaw grows clockwise from south, and pitch is positive looking down.
Vec3 lookVector(float yaw, float pitch) {
    const float y = toRadians(yaw);
    const float p = toRadians(pitch);
    return {-std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)};
}

}

Camera& Camera::get() {
    static Camera instance;
    return instance;
}

void Camera::requireSignatures() {
    Signatures& registry = Signatures::get();
    registry.requireOffset(kViewMatrix, "Camera");
    registry.requireOffset(kCameraOrigin, "Camera");
    registry.requireOffset(kFieldOfView, "Camera");
}

void Camera::setCalibration(float fieldOfView, float eyeHeight) {
    fov_ = clamp(fieldOfView, 30.f, 150.f);
    eyeHeight_ = clamp(eyeHeight, 0.f, 3.f);
}

void Camera::onFrame(FrameEvent& event) {
    screenSize_ = event.screenSize;

    valid_ = false;
    exact_ = false;

    if (!game().player().valid) return;
    if (screenSize_.x <= 0.f || screenSize_.y <= 0.f) return;

    exact_ = readGameMatrix();
    derive();
    valid_ = true;
}

// The matrix the game hands out is row-major and already multiplied through, so the
// projection below is the standard divide by w and nothing else.
bool Camera::readGameMatrix() {
    const int offset = sig::offset(kViewMatrix);
    if (offset < 0) return false;

    const uintptr_t instance = game().clientInstance();
    if (instance == 0) return false;

    const uintptr_t address = instance + static_cast<uintptr_t>(offset);
    if (!memory::readable(reinterpret_cast<const void*>(address), sizeof(matrix_))) return false;

    std::array<float, 16> read{};
    std::memcpy(read.data(), reinterpret_cast<const void*>(address), sizeof(read));

    // An all-zero or non-finite matrix is a signature pointing at the wrong field, and
    // projecting through it puts every box in the top-left corner rather than nowhere.
    float magnitude = 0.f;
    for (const float value : read) {
        if (!std::isfinite(value)) return false;
        magnitude += std::abs(value);
    }
    if (magnitude < 1e-4f) return false;

    matrix_ = read;

    if (const int originOffset = sig::offset(kCameraOrigin); originOffset >= 0) {
        origin_ = memory::read<Vec3>(instance + static_cast<uintptr_t>(originOffset), origin_);
    }
    if (const int fovOffset = sig::offset(kFieldOfView); fovOffset >= 0) {
        const float read = memory::read<float>(instance + static_cast<uintptr_t>(fovOffset), fov_);
        if (read > 10.f && read < 179.f) fov_ = read;
    }

    return true;
}

void Camera::derive() {
    const PlayerState& player = game().player();

    if (!exact_) origin_ = {player.position.x, player.position.y + eyeHeight_, player.position.z};

    forward_ = lookVector(player.yaw, player.pitch);

    constexpr Vec3 worldUp{0.f, 1.f, 0.f};
    right_ = forward_.cross(worldUp).normalised();
    if (right_.length() < 1e-4f) right_ = {1.f, 0.f, 0.f};
    up_ = right_.cross(forward_).normalised();
}

bool Camera::project(const Vec3& world, Vec2& screen) const {
    if (!valid_) return false;

    if (exact_) {
        const auto& m = matrix_;
        const float w = m[12] * world.x + m[13] * world.y + m[14] * world.z + m[15];
        if (w < 0.01f) return false;

        const float x = m[0] * world.x + m[1] * world.y + m[2] * world.z + m[3];
        const float y = m[4] * world.x + m[5] * world.y + m[6] * world.z + m[7];

        screen = {(screenSize_.x * 0.5f) * (1.f + x / w),
                  (screenSize_.y * 0.5f) * (1.f - y / w)};
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    }

    const Vec3 delta = world - origin_;
    const float depth = delta.dot(forward_);
    if (depth < 0.05f) return false;

    const float tanHalf = std::tan(toRadians(fov_) * 0.5f);
    if (tanHalf < 1e-4f) return false;

    const float aspect = screenSize_.x / screenSize_.y;
    const float x = delta.dot(right_) / (depth * tanHalf * aspect);
    const float y = delta.dot(up_) / (depth * tanHalf);

    screen = {(screenSize_.x * 0.5f) * (1.f + x), (screenSize_.y * 0.5f) * (1.f - y)};
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

bool Camera::projectBox(const Vec3& minimum, const Vec3& maximum, Rect& screen) const {
    if (!valid_) return false;

    Rect bounds{1e9f, 1e9f, -1e9f, -1e9f};

    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point{(corner & 1) ? maximum.x : minimum.x,
                         (corner & 2) ? maximum.y : minimum.y,
                         (corner & 4) ? maximum.z : minimum.z};

        Vec2 projected;
        if (!project(point, projected)) return false;

        bounds.left = std::min(bounds.left, projected.x);
        bounds.top = std::min(bounds.top, projected.y);
        bounds.right = std::max(bounds.right, projected.x);
        bounds.bottom = std::max(bounds.bottom, projected.y);
    }

    if (bounds.width() <= 0.f || bounds.height() <= 0.f) return false;

    screen = bounds;
    return true;
}

float Camera::angleTo(const Vec3& world) const {
    if (!valid_) return 180.f;

    const Vec3 delta = (world - origin_).normalised();
    if (delta.length() < 1e-4f) return 0.f;

    return toDegrees(std::acos(clamp(delta.dot(forward_), -1.f, 1.f)));
}

}
