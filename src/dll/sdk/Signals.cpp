#include "Signals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/Log.hpp"
#include "dll/feature/Services.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Entities.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx::sdk {
namespace {

constexpr const char* kLog = "Signals";

constexpr const char* kHealthOffset = "Player::health";

// A hit of the player's is followed by the target's health dropping a moment later,
// once the server has agreed. Within this window the drop is the same news twice.
constexpr long long kHitFollowsMs = 600;

// After this a remembered hit is only history.
constexpr long long kForgetHitsMs = 5000;

// A world runs at twenty ticks a second, and so does this beat.
constexpr float kTickSeconds = 0.05f;

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool healthReadable() { return sig::offset(kHealthOffset) >= 0; }

}

Signals& Signals::get() {
    static Signals instance;
    return instance;
}

void Signals::noteAttack(uintptr_t actor) {
    if (actor == 0) return;

    const std::lock_guard lock(mutex_);

    // A burst of hits on one target in one frame is one hit: the marker fires once,
    // the reach is one number.
    for (const Attack& pending : pendingAttacks_) {
        if (pending.actor == actor) return;
    }
    if (pendingAttacks_.size() < 32) pendingAttacks_.push_back({actor, nowMs()});
}

void Signals::setAttackDoor(bool live) { attackDoor_.store(live, std::memory_order_release); }

bool Signals::hurtReady() { return Game::packSeesPlayer() && healthReadable(); }

bool Signals::actorHurtReady() {
    if (get().attackDoor_.load(std::memory_order_acquire)) return true;
    return Entities::packSeesActors() && healthReadable();
}

bool Signals::attackReady() { return get().attackDoor_.load(std::memory_order_acquire); }

void Signals::onFrame(FrameEvent& event) {
    watchPlayer();
    drainAttacks();
    watchActors();
    tick(event.deltaSeconds);
}

void Signals::onJoin(WorldJoinEvent&) { forgetPlayer(); }

void Signals::onLeave(WorldLeaveEvent&) {
    forgetPlayer();
    actorHealth_.clear();
    recentHits_.clear();
}

void Signals::forgetPlayer() {
    playerKnown_ = false;
    playerHealth_ = 0.f;
    dead_ = false;
}

// The health the player has to lose. Absorption goes first when there is any, and a
// blow that only takes golden hearts is still a blow.
void Signals::watchPlayer() {
    const PlayerState& player = game().player();
    if (!player.valid || !healthReadable()) {
        forgetPlayer();
        return;
    }

    const float health = player.health + std::max(0.f, player.absorption);
    if (!std::isfinite(health)) return;

    if (!playerKnown_) {
        playerKnown_ = true;
        playerHealth_ = health;
        dead_ = player.health <= 0.f;
        return;
    }

    if (health < playerHealth_ - 0.01f) {
        if (player.health <= 0.f) {
            if (!dead_) {
                dead_ = true;
                DeathEvent death;
                death.position = player.position;
                events().emit(death);
                Log::debug(kLog, "the player died");
            }
        } else if (!dead_) {
            HurtEvent hurt;
            hurt.damage = playerHealth_ - health;
            hurt.health = player.health;
            events().emit(hurt);
        }
    } else if (dead_ && player.health > 0.f) {
        dead_ = false;
        RespawnEvent respawn;
        events().emit(respawn);
        Log::debug(kLog, "the player respawned");
    }

    playerHealth_ = health;
}

void Signals::emitActorHurt(Actor& actor, float damage, bool byPlayer) {
    ActorHurtEvent event;
    event.actor = &actor;
    event.damage = damage;
    event.byPlayer = byPlayer;
    events().emit(event);
}

// The hits the game registered since the last frame, each turned into the entity it
// landed on: this frame's snapshot entry when the pack can walk the list, a copy read
// off the address the game named when it cannot, and only then an address alone.
void Signals::drainAttacks() {
    std::vector<Attack> attacks;
    {
        const std::lock_guard lock(mutex_);
        attacks.swap(pendingAttacks_);
    }
    if (attacks.empty()) return;

    const long long now = nowMs();
    const Entities& list = entities();

    for (const Attack& attack : attacks) {
        Actor target;
        bool placed = false;

        if (const Actor* known = list.find(attack.actor)) {
            target = *known;
            placed = true;
        } else if (list.read(attack.actor, target)) {
            placed = true;
        } else {
            target.address = attack.actor;
        }

        recentHits_[attack.actor] = now;

        // A reach is a distance, and an entity nobody could place has none to give.
        if (placed) {
            AttackEvent event;
            event.target = &target;
            events().emit(event);
        }

        emitActorHurt(target, 0.f, true);
    }
}

// Everything in the snapshot that has less health than it had a frame ago was hurt.
void Signals::watchActors() {
    const Entities& list = entities();
    if (!list.available() || !healthReadable()) {
        if (!actorHealth_.empty()) actorHealth_.clear();
        return;
    }

    const long long now = nowMs();

    std::unordered_map<uintptr_t, float> current;
    current.reserve(list.list().size());

    for (const Actor& actor : list.list()) {
        if (actor.self || !actor.living()) continue;
        current[actor.address] = actor.health;

        const auto previous = actorHealth_.find(actor.address);
        if (previous == actorHealth_.end()) continue;

        const float drop = previous->second - actor.health;
        if (drop <= 0.01f) continue;

        const auto hit = recentHits_.find(actor.address);
        const bool ours = hit != recentHits_.end() && now - hit->second <= kHitFollowsMs;

        Actor copy = actor;
        if (!ours) {
            emitActorHurt(copy, drop, false);
            continue;
        }

        // The hit itself was announced the moment it registered. What is new here is
        // the entity going down under it, which a marker shows differently.
        if (actor.health <= 0.f) {
            SessionStats::get().addKill();
            emitActorHurt(copy, drop, true);
        }
    }

    actorHealth_.swap(current);

    std::erase_if(recentHits_, [now](const auto& entry) { return now - entry.second > kForgetHitsMs; });
}

void Signals::tick(float deltaSeconds) {
    if (!game().available() || !game().player().valid) {
        tickAccumulator_ = 0.f;
        return;
    }

    tickAccumulator_ += std::max(0.f, deltaSeconds);

    // A frame that took a quarter of a second owes five ticks; a frame that took a
    // minute does not owe twelve hundred.
    int owed = 0;
    while (tickAccumulator_ >= kTickSeconds && owed < 8) {
        tickAccumulator_ -= kTickSeconds;
        ++owed;
    }
    if (owed == 8) tickAccumulator_ = 0.f;

    for (int i = 0; i < owed; ++i) {
        TickEvent event;
        event.tick = ++tick_;
        events().emit(event);
    }
}

void bindSignals() {
    Signals& instance = Signals::get();

    // Behind the facade and the snapshot, both of which it reads, and ahead of every
    // module, so that what is emitted here reaches them in the same frame.
    events().on<FrameEvent>(&instance, &Signals::onFrame, EventPriority::High);
    events().on<WorldJoinEvent>(&instance, &Signals::onJoin, EventPriority::First);
    events().on<WorldLeaveEvent>(&instance, &Signals::onLeave, EventPriority::First);

    Log::debug(kLog, "signals bound");
}

}
