#pragma once

#include <cstdint>
#include <string>

#include "core/Math.hpp"
#include "dll/event/Events.hpp"

namespace velyx::sdk {

struct PlayerState {
    bool valid = false;

    Vec3 position;
    Vec3 velocity;
    float yaw = 0.f;
    float pitch = 0.f;

    float health = 0.f;
    float maxHealth = 20.f;
    float absorption = 0.f;
    float hunger = 20.f;
    float saturation = 0.f;
    int armourPoints = 0;

    int experienceLevel = 0;
    float experienceProgress = 0.f;

    std::string name;

    bool onGround = true;
    bool sneaking = false;
    bool sprinting = false;
    bool inWater = false;
    bool flying = false;
};

struct WorldState {
    bool inGame = false;
    bool multiplayer = false;

    std::string serverAddress;
    uint16_t serverPort = 0;
    std::string worldName;

    int dimension = 0;
    long long timeOfDay = 0;

    int entityCount = 0;
    int playerCount = 0;

    float ping = -1.f;

    float tps = -1.f;

    float packetLoss = 0.f;
};

class Game {
public:
    static Game& get();

    void requireSignatures();

    [[nodiscard]] bool available() const { return available_; }

    [[nodiscard]] const PlayerState& player() const { return player_; }
    [[nodiscard]] const WorldState& world() const { return world_; }

    [[nodiscard]] const std::string& screen() const { return screen_; }
    [[nodiscard]] bool inMenu() const;

    bool sendChat(const std::string& message);

    bool showClientMessage(const std::string& message);

    [[nodiscard]] static const char* compass(float yaw);
    [[nodiscard]] static const char* compassLong(float yaw);

    [[nodiscard]] float horizontalSpeed() const { return horizontalSpeed_; }

private:
    Game() = default;

    void onFrame(FrameEvent& event);
    void refreshPlayer();
    void refreshWorld();

    friend void bindGame();

    PlayerState player_;
    WorldState world_;
    std::string screen_;

    uintptr_t clientInstance_ = 0;
    bool available_ = false;

    Vec3 previousPosition_;
    float horizontalSpeed_ = 0.f;
    bool wasInGame_ = false;
    long long joinedAtMs_ = 0;
};

inline Game& game() { return Game::get(); }

void bindGame();

}
