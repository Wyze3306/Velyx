#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dll/event/Events.hpp"
#include "dll/sdk/Actor.hpp"

namespace velyx::sdk {

/// What the game does, read off what it shows.
///
/// Bedrock says nothing a client can hear when the player is hurt, dies, respawns or
/// lands a hit — not without a signature for the function that does it, and the one
/// the clients used to stand on for damage went away in 1.26. What it cannot hide is
/// the number: the health it draws is the health it holds, and the snapshot the SDK
/// takes every frame already carries it, for the player and for everything around
/// them. So these events are read off the snapshot, the frame the number moves.
///
/// The one thing heard rather than read is the attack. GameMode::attack runs on the
/// game's thread the moment a hit registers, with the target's address; the hook only
/// remembers it, and the frame emits it, so a handler is free to draw.
class Signals {
public:
    static Signals& get();

    // From the attack hook, on the game's own thread. Remembers, nothing else.
    void noteAttack(uintptr_t actor);

    // Whether the attack hook is standing. Set by the hook.
    void setAttackDoor(bool live);

    // Whether the derived events have what they need: the player's health for
    // HurtEvent, DeathEvent and RespawnEvent; a hit registering or the entities'
    // health for ActorHurtEvent; the attack hook for AttackEvent.
    [[nodiscard]] static bool hurtReady();
    [[nodiscard]] static bool actorHurtReady();
    [[nodiscard]] static bool attackReady();

private:
    Signals() = default;

    friend void bindSignals();

    void onFrame(FrameEvent& event);
    void onJoin(WorldJoinEvent& event);
    void onLeave(WorldLeaveEvent& event);

    void watchPlayer();
    void drainAttacks();
    void watchActors();
    void tick(float deltaSeconds);

    void emitActorHurt(Actor& actor, float damage, bool byPlayer);
    void forgetPlayer();

    struct Attack {
        uintptr_t actor = 0;
        long long atMs = 0;
    };

    std::mutex mutex_;
    std::vector<Attack> pendingAttacks_;
    std::atomic<bool> attackDoor_{false};

    // What the previous frame knew, so that a drop is a drop and not a first sighting.
    bool playerKnown_ = false;
    float playerHealth_ = 0.f;
    bool dead_ = false;

    std::unordered_map<uintptr_t, float> actorHealth_;

    // When the player's own hit last registered on each entity, so that the drop in
    // health that follows it is not announced a second time.
    std::unordered_map<uintptr_t, long long> recentHits_;

    float tickAccumulator_ = 0.f;
    uint64_t tick_ = 0;
};

inline Signals& signals() { return Signals::get(); }

// Binds the watcher behind the SDK facade and the entity snapshot, which it reads.
void bindSignals();

}
