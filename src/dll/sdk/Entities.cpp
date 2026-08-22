#include "Entities.hpp"

#include <algorithm>
#include <cmath>

#include "core/Log.hpp"
#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Camera.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx::sdk {
namespace {

constexpr const char* kLog = "Entities";

namespace names {
constexpr const char* kActorList = "Level::runtimeActorList";
constexpr const char* kPosition = "Actor::position";
constexpr const char* kVelocity = "Actor::velocity";
constexpr const char* kRotation = "Actor::rotation";
constexpr const char* kNameTag = "Actor::nameTag";
constexpr const char* kDimensions = "Actor::aabbDimensions";
constexpr const char* kTypeId = "Actor::entityTypeId";
constexpr const char* kHealth = "Player::health";
constexpr const char* kMaxHealth = "Player::maxHealth";
}

// A world with more entities than this in it is a world where drawing a box over each
// of them costs more than it tells you. The cap is on the read, not on the drawing:
// nothing further down has to defend itself against a list that grew without bound.
constexpr size_t kMaxActors = 512;

// The values the community's tables agree on for Bedrock's ActorType. Being wrong here
// mislabels an entity; it never reads anything it should not, because the type is only
// ever used to pick a colour and a checkbox.
constexpr int kTypePlayer = 0x13F;
constexpr int kTypeItem = 0x40;
constexpr int kMaskMob = 0x100;
constexpr int kMaskMonster = 0x800;
constexpr int kMaskAnimal = 0x1000;
constexpr int kMaskWaterAnimal = 0x2000;
constexpr int kMaskProjectile = 0x4000;

ActorKind classify(int typeId, bool named) {
    if (typeId == kTypePlayer) return ActorKind::Player;
    if (typeId == kTypeItem) return ActorKind::Item;

    if (typeId > 0) {
        if (typeId & kMaskProjectile) return ActorKind::Projectile;
        if (typeId & kMaskMonster) return ActorKind::Hostile;
        if ((typeId & kMaskAnimal) || (typeId & kMaskWaterAnimal)) return ActorKind::Passive;
        if (typeId & kMaskMob) return ActorKind::Passive;
        return ActorKind::Unknown;
    }

    // No type offset in the pack. A nametag is the one thing only players reliably
    // carry, which is enough to keep the player-only modules honest.
    return named ? ActorKind::Player : ActorKind::Unknown;
}

std::string readNameTag(uintptr_t address) {
    if (!address || !memory::readable(reinterpret_cast<const void*>(address), 32)) return {};

    const auto size = memory::read<uint64_t>(address + 16);
    const auto capacity = memory::read<uint64_t>(address + 24);

    if (size == 0 || size > 256) return {};

    if (capacity > 15) {
        const auto data = memory::read<uintptr_t>(address);
        if (!memory::readable(reinterpret_cast<const void*>(data), size)) return {};
        return std::string(reinterpret_cast<const char*>(data), size);
    }

    return std::string(reinterpret_cast<const char*>(address), size);
}

}

Entities& Entities::get() {
    static Entities instance;
    return instance;
}

void Entities::requireSignatures() {
    Signatures& registry = Signatures::get();
    for (const char* offset : {names::kDimensions, names::kTypeId}) {
        registry.requireOffset(offset, "Entities");
    }
}

bool Entities::readActor(uintptr_t address, Actor& out) const {
    if (!memory::readable(reinterpret_cast<const void*>(address), 8)) return false;

    const int positionOffset = sig::offset(names::kPosition);
    if (positionOffset < 0) return false;

    out.address = address;
    out.position = memory::read<Vec3>(address + static_cast<uintptr_t>(positionOffset));

    // Coordinates outside the world are how a stale pointer announces itself, and the
    // list is walked often enough that one of them is a matter of when, not whether.
    if (!std::isfinite(out.position.x) || !std::isfinite(out.position.y) ||
        !std::isfinite(out.position.z)) {
        return false;
    }
    if (std::abs(out.position.x) > 4.0e7f || std::abs(out.position.z) > 4.0e7f ||
        std::abs(out.position.y) > 1.0e4f) {
        return false;
    }

    if (const int offset = sig::offset(names::kVelocity); offset >= 0) {
        out.velocity = memory::read<Vec3>(address + static_cast<uintptr_t>(offset));
    }
    if (const int offset = sig::offset(names::kRotation); offset >= 0) {
        out.pitch = memory::read<float>(address + static_cast<uintptr_t>(offset));
        out.yaw = memory::read<float>(address + static_cast<uintptr_t>(offset) + 4);
    }
    if (const int offset = sig::offset(names::kNameTag); offset >= 0) {
        out.name = readNameTag(address + static_cast<uintptr_t>(offset));
    }

    if (const int offset = sig::offset(names::kDimensions); offset >= 0) {
        const float width = memory::read<float>(address + static_cast<uintptr_t>(offset), 0.f);
        const float height = memory::read<float>(address + static_cast<uintptr_t>(offset) + 4, 0.f);
        if (width > 0.01f && width < 64.f) out.width = width;
        if (height > 0.01f && height < 64.f) out.height = height;
    }

    if (const int offset = sig::offset(names::kHealth); offset >= 0) {
        const float health = memory::read<float>(address + static_cast<uintptr_t>(offset), 0.f);
        if (std::isfinite(health) && health >= 0.f && health < 10000.f) out.health = health;
    }
    if (const int offset = sig::offset(names::kMaxHealth); offset >= 0) {
        const float maximum = memory::read<float>(address + static_cast<uintptr_t>(offset), 0.f);
        if (std::isfinite(maximum) && maximum > 0.f && maximum < 10000.f) out.maxHealth = maximum;
    }

    int typeId = 0;
    if (const int offset = sig::offset(names::kTypeId); offset >= 0) {
        typeId = memory::read<int>(address + static_cast<uintptr_t>(offset), 0);
    }

    out.self = address == game().localPlayer();
    out.kind = out.self ? ActorKind::Player : classify(typeId, !out.name.empty());

    return true;
}

// The list is a plain vector of pointers: first and last, eight bytes apart. Anything
// that does not look like one is treated as no list at all rather than walked.
bool Entities::readList() {
    const int offset = sig::offset(names::kActorList);
    const uintptr_t level = game().level();
    if (offset < 0 || level == 0) return false;

    const uintptr_t vector = level + static_cast<uintptr_t>(offset);
    if (!memory::readable(reinterpret_cast<const void*>(vector), 16)) return false;

    const auto first = memory::read<uintptr_t>(vector);
    const auto last = memory::read<uintptr_t>(vector + 8);

    if (first == 0 || last < first) return false;

    const size_t span = static_cast<size_t>(last - first);
    if (span % sizeof(uintptr_t) != 0) return false;

    size_t count = span / sizeof(uintptr_t);
    if (count > kMaxActors * 8) return false;

    dropped_ = count > kMaxActors ? static_cast<int>(count - kMaxActors) : 0;
    count = std::min(count, kMaxActors);

    if (!memory::readable(reinterpret_cast<const void*>(first), count * sizeof(uintptr_t))) {
        return false;
    }

    const Vec3 eye = camera().origin();

    actors_.clear();
    actors_.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const auto address = memory::read<uintptr_t>(first + i * sizeof(uintptr_t));
        if (address == 0) continue;

        Actor actor;
        if (!readActor(address, actor)) continue;

        actor.distance = actor.centre().distanceTo(eye);
        actors_.push_back(std::move(actor));
    }

    std::ranges::sort(actors_, [](const Actor& a, const Actor& b) {
        return a.distance < b.distance;
    });

    return true;
}

void Entities::onFrame(FrameEvent& event) {
    if (!game().available() || !game().player().valid) {
        if (!actors_.empty()) actors_.clear();
        available_ = false;
        dropped_ = 0;
        return;
    }

    if (!readList()) {
        actors_.clear();
        available_ = false;
        dropped_ = 0;
        return;
    }

    available_ = true;
}

const Actor* Entities::nearestPlayer(float maxDistance) const {
    for (const Actor& actor : actors_) {
        if (actor.self || !actor.isPlayer()) continue;
        if (actor.distance > maxDistance) break;
        return &actor;
    }
    return nullptr;
}

const Actor* Entities::underCrosshair(float maxDistance, float coneDegrees) const {
    const Actor* best = nullptr;
    float bestAngle = coneDegrees;

    for (const Actor& actor : actors_) {
        if (actor.self) continue;
        if (actor.distance > maxDistance) break;

        const float angle = camera().angleTo(actor.centre());
        if (angle > bestAngle) continue;

        bestAngle = angle;
        best = &actor;
    }

    return best;
}

const Actor* Entities::find(uintptr_t address) const {
    if (address == 0) return nullptr;

    const auto found = std::ranges::find_if(actors_, [address](const Actor& actor) {
        return actor.address == address;
    });
    return found != actors_.end() ? &*found : nullptr;
}

int Entities::count(ActorKind kind) const {
    return static_cast<int>(std::ranges::count_if(actors_, [kind](const Actor& actor) {
        return actor.kind == kind && !actor.self;
    }));
}

void bindWorld() {
    Camera& view = Camera::get();
    Entities& list = Entities::get();

    view.requireSignatures();
    list.requireSignatures();

    // Behind the SDK facade, which refreshes the player this frame reads, and ahead of
    // every module: the camera has to be current before the list measures against it.
    events().on<FrameEvent>(&view, &Camera::onFrame, EventPriority::First);
    events().on<FrameEvent>(&list, &Entities::onFrame, EventPriority::High);

    Log::debug(kLog, "world snapshot bound");
}

}

namespace velyx {

const char* actorKindLabel(ActorKind kind) {
    switch (kind) {
        case ActorKind::Player:     return "Player";
        case ActorKind::Hostile:    return "Hostile";
        case ActorKind::Passive:    return "Passive";
        case ActorKind::Item:       return "Item";
        case ActorKind::Projectile: return "Projectile";
        case ActorKind::Vehicle:    return "Vehicle";
        case ActorKind::Unknown:    break;
    }
    return "Other";
}

}
