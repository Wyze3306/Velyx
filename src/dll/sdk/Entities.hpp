#pragma once

#include <vector>

#include "dll/event/Events.hpp"
#include "dll/sdk/Actor.hpp"

namespace velyx::sdk {

// Everything the level is holding, copied once a frame and sorted near to far.
//
// Modules never walk the game's list themselves. They get this snapshot, which is
// finished being read before the first of them looks at it — an entity that dies
// mid-frame cannot take a module down with it, and eight modules reading the list
// cost one read between them rather than eight.
class Entities {
public:
    static Entities& get();

    void requireSignatures();

    [[nodiscard]] bool available() const { return available_; }

    [[nodiscard]] const std::vector<Actor>& list() const { return actors_; }

    // How much of the list was thrown away because it was longer than the cap. Shown
    // by the diagnostics readout; a non-zero value means a busy world, not a fault.
    [[nodiscard]] int dropped() const { return dropped_; }

    [[nodiscard]] const Actor* nearestPlayer(float maxDistance) const;

    // The entity closest to where the camera points, within a cone. There is no ray to
    // cast without the world's collision, so the cone is what "aiming at" means here.
    [[nodiscard]] const Actor* underCrosshair(float maxDistance, float coneDegrees) const;

    [[nodiscard]] const Actor* find(uintptr_t address) const;

    [[nodiscard]] int count(ActorKind kind) const;

private:
    Entities() = default;

    friend void bindWorld();
    void onFrame(FrameEvent& event);

    bool readList();
    bool readActor(uintptr_t address, Actor& out) const;

    std::vector<Actor> actors_;
    int dropped_ = 0;
    bool available_ = false;
};

inline Entities& entities() { return Entities::get(); }

// Binds the camera and the entity list, and declares the signatures both need. Called
// from the same place as bindGame(), before anything is resolved.
void bindWorld();

}
