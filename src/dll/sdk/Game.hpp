#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

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

    // Whether the pack carries any route to the client instance at all. False means
    // nothing that reads the game can ever work, whatever else resolved.
    [[nodiscard]] static bool reachable();

    // Whether the pack can get from the client instance to the player. Kept apart from
    // reachable: without this one offset every reading of the player is empty and every
    // other Actor offset in the pack is dead weight, because there is no object to
    // apply it to — and a HUD full of zeroes says none of that on its own.
    //
    // There is a second road to it. Every turn the game applies is applied to the
    // LocalPlayer, so a pack that carries the turn hook's signature names the player
    // without an offset at all; see adoptLocalPlayer.
    [[nodiscard]] static bool packSeesPlayer();

    // The player, announced by the turn hook rather than read through an offset. Called
    // on the game's own thread, once per turn; it only takes note when the pointer is
    // new. Kept for as long as the object still starts with the vtable it had, and used
    // whenever the pack has no localPlayer offset of its own — or the offset reads
    // nothing.
    void adoptLocalPlayer(uintptr_t player, const char* through);

    // Whether an object is the local player rather than another player: the one whose
    // vtable the pack names when it names it, the one already adopted otherwise. A hook
    // that runs for every player — the integrated server runs GameMode::attack a second
    // time for its own copy of the player — asks before it takes anything as ours.
    [[nodiscard]] bool isLocalPlayer(uintptr_t player) const;

    // Said by the turn hook when it stands, so that the menu tells the player to move
    // rather than to find an offset.
    void expectPlayerFromHook(bool live);

    // Stops the finder thread. Before the modules go, on the way out.
    void shutdown();

    [[nodiscard]] const PlayerState& player() const { return player_; }
    [[nodiscard]] const WorldState& world() const { return world_; }

    [[nodiscard]] const std::string& screen() const { return screen_; }
    [[nodiscard]] bool inMenu() const;

    // The two things about the screen that anything off the frame thread may ask. The
    // name itself is a std::string rewritten every scan, so it cannot be handed across
    // threads; what it means is published here instead. Known is false when the pack
    // carries no offset to read the screen with, and then neither answer says anything.
    [[nodiscard]] bool screenKnown() const {
        return screenKnown_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool chatOnScreen() const {
        return chatScreen_.load(std::memory_order_acquire);
    }

    bool sendChat(const std::string& message);

    bool showClientMessage(const std::string& message);

    [[nodiscard]] static const char* compass(float yaw);
    [[nodiscard]] static const char* compassLong(float yaw);

    [[nodiscard]] float horizontalSpeed() const { return horizontalSpeed_; }

    // The two anchors everything else in the SDK hangs off. Handed out as addresses
    // rather than as objects on purpose: they are only valid for the frame that read
    // them, and every read through them still goes past memory::readable().
    [[nodiscard]] uintptr_t clientInstance() const { return clientInstance_; }
    [[nodiscard]] uintptr_t localPlayer() const { return localPlayer_; }

    [[nodiscard]] uintptr_t level() const;

private:
    Game() = default;

    void onFrame(FrameEvent& event);

    // How the client instance is reached on a build that keeps no global for it.
    void locateClientInstance(float deltaSeconds);
    static constexpr float kScanInterval = 1.f;
    static constexpr float kMaximumScanInterval = 30.f;
    void refreshPlayer();
    void refreshWorld();
    void publishScreen();

    // The adopted player, or 0 once it no longer looks like the object that was adopted.
    [[nodiscard]] uintptr_t adoptedPlayer() const;

    // The third road to the player: the object that starts with LocalPlayer's own
    // vtable, when the pack names it. Bedrock allocates one, so a walk of the heap
    // finds it — but a walk of the heap is seconds of reading, and the frame is no
    // place for that. A thread of its own walks while the player is wanted and unknown,
    // and hands what it finds to adoptLocalPlayer like the turn hook does.
    void wantPlayerFromVtable();
    void finderLoop(const std::stop_token& stop);

    friend void bindGame();

    PlayerState player_;
    WorldState world_;
    std::string screen_;
    std::atomic<bool> screenKnown_{false};
    std::atomic<bool> chatScreen_{false};

    uintptr_t clientInstance_ = 0;

    // Written by the turn hook on the game's thread, read here on the render thread:
    // the vtable first, then the pointer, so that a pointer seen is a pointer whose
    // vtable is known.
    std::atomic<uintptr_t> adopted_{0};
    std::atomic<uintptr_t> adoptedVtable_{0};
    std::atomic<bool> playerHook_{false};

    std::jthread finder_;
    std::atomic<bool> finderWanted_{false};

    float sinceScan_ = kScanInterval;
    float scanInterval_ = kScanInterval;
    uintptr_t announced_ = 0;
    uintptr_t localPlayer_ = 0;
    bool available_ = false;

    Vec3 previousPosition_;
    float horizontalSpeed_ = 0.f;
    bool wasInGame_ = false;
    long long joinedAtMs_ = 0;
};

inline Game& game() { return Game::get(); }

void bindGame();

}
