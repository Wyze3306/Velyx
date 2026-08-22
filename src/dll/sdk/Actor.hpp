#pragma once

#include <cstdint>
#include <string>

#include "core/Math.hpp"

namespace velyx {

enum class ActorKind {
    Unknown,
    Player,
    Hostile,
    Passive,
    Item,
    Projectile,
    Vehicle,
};

const char* actorKindLabel(ActorKind kind);

// One entity, copied out of the game once per frame. `address` is an identity, never
// something a module dereferences: by the time a module sees this the entity may
// already be gone, and only the values read under the SDK's own guards are safe.
class Actor {
public:
    uintptr_t address = 0;

    ActorKind kind = ActorKind::Unknown;
    std::string name;

    // Where the game keeps it, which for Bedrock is the feet. `Origin` on the modules
    // that draw boxes exists because a signature pack may point at the eye instead.
    Vec3 position;
    Vec3 velocity;

    float yaw = 0.f;
    float pitch = 0.f;

    float health = 0.f;
    float maxHealth = 0.f;

    float width = 0.6f;
    float height = 1.8f;

    float distance = 0.f;

    bool self = false;

    [[nodiscard]] bool isPlayer() const { return kind == ActorKind::Player; }
    [[nodiscard]] bool living() const { return maxHealth > 0.f; }

    [[nodiscard]] Vec3 head() const { return {position.x, position.y + height, position.z}; }
    [[nodiscard]] Vec3 centre() const {
        return {position.x, position.y + height * 0.5f, position.z};
    }

    [[nodiscard]] Vec3 minimum(float expand = 0.f) const {
        const float half = width * 0.5f + expand;
        return {position.x - half, position.y - expand, position.z - half};
    }

    [[nodiscard]] Vec3 maximum(float expand = 0.f) const {
        const float half = width * 0.5f + expand;
        return {position.x + half, position.y + height + expand, position.z + half};
    }

    [[nodiscard]] float healthFraction() const {
        if (maxHealth <= 0.f) return 0.f;
        return clamp(health / maxHealth, 0.f, 1.f);
    }
};

}
