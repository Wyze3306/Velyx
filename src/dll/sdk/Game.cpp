#include "Game.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/Log.hpp"
#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"

namespace velyx::sdk {
namespace {

constexpr const char* kLog = "Game";

namespace names {
constexpr const char* kClientInstance = "ClientInstance::instance";
constexpr const char* kClientVtable = "ClientInstance::vtable";
constexpr const char* kPlayerVtable = "LocalPlayer::vtable";
constexpr const char* kLocalPlayerOffset = "ClientInstance::localPlayer";
constexpr const char* kLevelOffset = "ClientInstance::level";
constexpr const char* kScreenNameOffset = "ClientInstance::screenName";
constexpr const char* kPositionOffset = "Actor::position";
constexpr const char* kVelocityOffset = "Actor::velocity";
constexpr const char* kRotationOffset = "Actor::rotation";
constexpr const char* kOnGroundOffset = "Actor::onGround";
constexpr const char* kHealthOffset = "Player::health";
constexpr const char* kMaxHealthOffset = "Player::maxHealth";
constexpr const char* kAbsorptionOffset = "Player::absorption";
constexpr const char* kHungerOffset = "Player::hunger";
constexpr const char* kArmourOffset = "Player::armourPoints";
constexpr const char* kXpLevelOffset = "Player::experienceLevel";
constexpr const char* kXpProgressOffset = "Player::experienceProgress";
constexpr const char* kNameOffset = "Actor::nameTag";
constexpr const char* kServerAddress = "Connection::serverAddress";
constexpr const char* kPing = "RakNetConnector::ping";
constexpr const char* kEntityList = "Level::runtimeActorList";
constexpr const char* kSendChat = "LocalPlayer::sendChatMessage";
constexpr const char* kClientMessage = "GuiData::displayClientMessage";
constexpr const char* kChatMessage = "GuiData::displayChatMessage";
}

// How much of a candidate is examined, and how many object-shaped fields it has to
// carry. The live instance has ninety-nine in its first eight kilobytes, so eight in
// the first kilobyte is a low bar that no loose copy has cleared.
constexpr uintptr_t kProbeBytes = 0x400;
constexpr int kMinimumObjectFields = 6;

// A vtable's value turns up in freed blocks and in copies of the pointer, so matching
// it is not enough. What separates the real object from the litter is what follows: a
// ClientInstance, and a player for that matter, is dense with pointers to other
// objects, and those objects start with a vtable of their own inside the game's image.
// A stale block does not.
bool looksLikeClientInstance(uintptr_t candidate) {
    const memory::ModuleRange& module = memory::gameModule();
    if (!module.valid()) return false;

    if (!memory::readable(reinterpret_cast<const void*>(candidate), kProbeBytes)) return false;

    int objects = 0;
    for (uintptr_t offset = 8; offset < kProbeBytes; offset += 8) {
        const auto field = memory::read<uintptr_t>(candidate + offset);
        if (field < 0x10000) continue;
        if (!memory::readable(reinterpret_cast<const void*>(field), 8)) continue;

        const auto head = memory::read<uintptr_t>(field);
        if (module.contains(head)) ++objects;
    }

    return objects >= kMinimumObjectFields;
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}

Game& Game::get() {
    static Game instance;
    return instance;
}

void Game::requireSignatures() {
    Signatures& registry = Signatures::get();

    // Two ways to the same object, and a pack only needs whichever its build has.
    // Neither is required on its own: reduced mode is decided on having found the
    // instance, not on having found a particular route to it.
    SignatureSpec clientInstance;
    clientInstance.name = names::kClientInstance;
    clientInstance.kind = SignatureKind::Relative;
    clientInstance.owner = "SDK";
    registry.require(clientInstance);

    SignatureSpec clientVtable;
    clientVtable.name = names::kClientVtable;
    clientVtable.kind = SignatureKind::Relative;
    clientVtable.owner = "SDK";
    registry.require(clientVtable);

    // The player the same way, for a pack that has no offset to it and no turn hook.
    SignatureSpec playerVtable;
    playerVtable.name = names::kPlayerVtable;
    playerVtable.kind = SignatureKind::Relative;
    playerVtable.owner = "SDK";
    registry.require(playerVtable);

    SignatureSpec sendChat;
    sendChat.name = names::kSendChat;
    sendChat.owner = "SDK";
    registry.require(sendChat);

    SignatureSpec clientMessage;
    clientMessage.name = names::kClientMessage;
    clientMessage.owner = "SDK";
    registry.require(clientMessage);

    // Not read here: ChatHook detours it, and the SDK is where the pack's entries are
    // declared so the Diagnostics page lists them all in one place.
    SignatureSpec chatMessage;
    chatMessage.name = names::kChatMessage;
    chatMessage.owner = "SDK";
    registry.require(chatMessage);

    for (const char* offset : {names::kLocalPlayerOffset, names::kLevelOffset,
                               names::kScreenNameOffset, names::kPositionOffset,
                               names::kVelocityOffset, names::kRotationOffset,
                               names::kOnGroundOffset, names::kHealthOffset,
                               names::kMaxHealthOffset, names::kAbsorptionOffset,
                               names::kHungerOffset, names::kArmourOffset,
                               names::kXpLevelOffset, names::kXpProgressOffset,
                               names::kNameOffset, names::kServerAddress, names::kPing,
                               names::kEntityList}) {
        registry.requireOffset(offset, "SDK");
    }
}

uintptr_t Game::adoptedPlayer() const {
    const uintptr_t player = adopted_.load(std::memory_order_acquire);
    if (player == 0) return 0;

    // The vtable is the object's signature: a destructor rewrites it on the way out,
    // and a block reused for something else starts with something else.
    if (!memory::readable(reinterpret_cast<const void*>(player), 8)) return 0;
    if (memory::read<uintptr_t>(player) != adoptedVtable_.load(std::memory_order_acquire)) {
        return 0;
    }
    return player;
}

void Game::adoptLocalPlayer(uintptr_t player, const char* through) {
    if (player == 0 || player == adopted_.load(std::memory_order_relaxed)) return;
    if (!memory::readable(reinterpret_cast<const void*>(player), 8)) return;

    const auto vtable = memory::read<uintptr_t>(player);
    if (!memory::gameModule().contains(vtable)) return;

    // Where the pack names the LocalPlayer's vtable, nothing else is the player.
    if (const uintptr_t expected = sig::address(names::kPlayerVtable);
        expected != 0 && vtable != expected) {
        return;
    }

    adoptedVtable_.store(vtable, std::memory_order_release);
    adopted_.store(player, std::memory_order_release);
    Log::info(kLog, "the player announced itself through {} at {:#x}", through, player);
}

bool Game::isLocalPlayer(uintptr_t player) const {
    if (player == 0) return false;

    if (const uintptr_t expected = sig::address(names::kPlayerVtable); expected != 0) {
        return memory::read<uintptr_t>(player) == expected;
    }

    // No vtable to go by: the first one heard is taken, and after that only it.
    const uintptr_t known = adopted_.load(std::memory_order_acquire);
    return known == 0 || known == player;
}

void Game::expectPlayerFromHook(bool live) {
    playerHook_.store(live, std::memory_order_release);
    if (!live) adopted_.store(0, std::memory_order_release);
}

void Game::wantPlayerFromVtable() {
    if (sig::address(names::kPlayerVtable) == 0) return;

    finderWanted_.store(true, std::memory_order_release);
    if (!finder_.joinable()) {
        finder_ = std::jthread([this](const std::stop_token& stop) { finderLoop(stop); });
    }
}

// Walks while wanted, rests while not. One walk is a read of everything committed,
// so the rest between two is long enough that the game never sees two in a row —
// and the moment something is found, the walking stops until the object is gone.
void Game::finderLoop(const std::stop_token& stop) {
    constexpr int kRestMs = 2500;

    while (!stop.stop_requested()) {
        if (!finderWanted_.load(std::memory_order_acquire)) {
            Sleep(200);
            continue;
        }

        const uintptr_t vtable = sig::address(names::kPlayerVtable);
        const uintptr_t found = vtable == 0 ? 0 : memory::findByVtable(vtable, [](uintptr_t candidate) {
            return looksLikeClientInstance(candidate);
        });

        if (found != 0) {
            adoptLocalPlayer(found, "its vtable");
            finderWanted_.store(false, std::memory_order_release);
        }

        for (int waited = 0; waited < kRestMs && !stop.stop_requested(); waited += 100) Sleep(100);
    }
}

void Game::shutdown() {
    finderWanted_.store(false, std::memory_order_release);
    if (finder_.joinable()) {
        finder_.request_stop();
        finder_.join();
    }
}

void Game::refreshPlayer() {
    player_ = {};
    localPlayer_ = 0;

    if (clientInstance_ == 0) return;

    // The offset first, where the pack has one; the object the turn hook named
    // otherwise. Both go past the same check.
    uintptr_t localPlayer = 0;
    if (const int offset = sig::offset(names::kLocalPlayerOffset); offset >= 0) {
        localPlayer = memory::read<uintptr_t>(clientInstance_ + static_cast<uintptr_t>(offset));
    }
    if (!memory::readable(reinterpret_cast<const void*>(localPlayer), 8)) {
        localPlayer = adoptedPlayer();
    }
    if (!memory::readable(reinterpret_cast<const void*>(localPlayer), 8)) {
        // Nothing names the player. The finder looks for it by its vtable when the
        // pack carries one, off the frame, and hands it to adoptLocalPlayer — unless the
        // turn hook stands, which names it on the first frame in a world. Walking gigabytes
        // every few seconds on a menu, where there is no player to find, costs for nothing.
        if (!playerHook_.load(std::memory_order_acquire)) wantPlayerFromVtable();
        return;
    }

    localPlayer_ = localPlayer;

    player_.valid = true;

    const auto readVec3 = [&](const char* name, Vec3 fallback) {
        const int offset = sig::offset(name);
        if (offset < 0) return fallback;
        return memory::read<Vec3>(localPlayer + static_cast<uintptr_t>(offset), fallback);
    };
    const auto readFloat = [&](const char* name, float fallback) {
        const int offset = sig::offset(name);
        if (offset < 0) return fallback;
        return memory::read<float>(localPlayer + static_cast<uintptr_t>(offset), fallback);
    };
    const auto readInt = [&](const char* name, int fallback) {
        const int offset = sig::offset(name);
        if (offset < 0) return fallback;
        return memory::read<int>(localPlayer + static_cast<uintptr_t>(offset), fallback);
    };

    player_.position = readVec3(names::kPositionOffset, {});
    player_.velocity = readVec3(names::kVelocityOffset, {});

    // Coordinates outside the world are how a wrong offset announces itself, and a
    // HUD that draws them as though they were the answer says nothing about that.
    const Vec3& at = player_.position;
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) ||
        std::abs(at.x) > 4.0e7f || std::abs(at.z) > 4.0e7f || std::abs(at.y) > 1.0e4f) {
        static bool said = false;
        if (!said) {
            said = true;
            Log::warn(kLog, "the player's position reads as ({}, {}, {}); the Actor::position "
                            "offset does not fit this build",
                      at.x, at.y, at.z);
        }
        player_.position = {};
        player_.velocity = {};
    }

    if (const int rotation = sig::offset(names::kRotationOffset); rotation >= 0) {

        player_.pitch = memory::read<float>(localPlayer + static_cast<uintptr_t>(rotation));
        player_.yaw = memory::read<float>(localPlayer + static_cast<uintptr_t>(rotation) + 4);
    }

    player_.health = readFloat(names::kHealthOffset, 0.f);
    player_.maxHealth = readFloat(names::kMaxHealthOffset, 20.f);
    player_.absorption = readFloat(names::kAbsorptionOffset, 0.f);
    player_.hunger = readFloat(names::kHungerOffset, 20.f);
    player_.armourPoints = readInt(names::kArmourOffset, 0);
    player_.experienceLevel = readInt(names::kXpLevelOffset, 0);
    player_.experienceProgress = readFloat(names::kXpProgressOffset, 0.f);

    if (const int onGround = sig::offset(names::kOnGroundOffset); onGround >= 0) {
        player_.onGround =
            memory::read<uint8_t>(localPlayer + static_cast<uintptr_t>(onGround)) != 0;
    }

    if (const int nameOffset = sig::offset(names::kNameOffset); nameOffset >= 0) {
        player_.name = memory::readString(localPlayer + static_cast<uintptr_t>(nameOffset));
    }
}

// Everything the rest of the client is allowed to know about the screen from another
// thread. Bedrock names its chat screen for what it is, so the name carries the answer.
void Game::publishScreen() {
    chatScreen_.store(screen_.find("chat") != std::string::npos, std::memory_order_release);
    screenKnown_.store(!screen_.empty(), std::memory_order_release);
}

void Game::refreshWorld() {
    const bool wasInGame = world_.inGame;

    world_.inGame = player_.valid;

    if (const int screenOffset = sig::offset(names::kScreenNameOffset);
        screenOffset >= 0 && clientInstance_ != 0) {
        std::string screen =
            memory::readString(clientInstance_ + static_cast<uintptr_t>(screenOffset));

        // The name changing is the one thing about the screen worth telling anyone,
        // and it is told after the facade already answers for the new one.
        if (screen != screen_) {
            ScreenChangeEvent change;
            change.previous = screen_;
            change.current = screen;
            screen_ = std::move(screen);
            publishScreen();
            events().emit(change);
        }
    }

    if (const int addressOffset = sig::offset(names::kServerAddress);
        addressOffset >= 0 && clientInstance_ != 0) {
        world_.serverAddress = memory::readString(clientInstance_ + static_cast<uintptr_t>(addressOffset));
        world_.multiplayer = !world_.serverAddress.empty();
    }

    if (const int pingOffset = sig::offset(names::kPing); pingOffset >= 0 && clientInstance_ != 0) {
        world_.ping = static_cast<float>(
            memory::read<int>(clientInstance_ + static_cast<uintptr_t>(pingOffset), -1));
    }

    if (world_.inGame && !wasInGame) {
        joinedAtMs_ = nowMs();

        WorldJoinEvent event;
        event.serverAddress = world_.serverAddress;
        event.serverPort = world_.serverPort;
        event.worldName = world_.worldName;
        event.multiplayer = world_.multiplayer;
        events().emit(event);
    } else if (!world_.inGame && wasInGame) {
        WorldLeaveEvent event;
        event.serverAddress = world_.serverAddress;
        event.sessionSeconds = joinedAtMs_ > 0 ? (nowMs() - joinedAtMs_) / 1000 : 0;
        events().emit(event);

        world_ = {};
        screen_.clear();
        publishScreen();
    }
}

// Bedrock keeps no global for this. What it does have is a vtable whose address the
// constructor loads, so the object is found once by looking for it, then kept as long
// as it still starts with that vtable — one read a frame instead of a walk.
//
// The walk is expensive: gigabytes, once. It is on a timer that nothing is allowed to
// short-circuit, because the first version of this rescanned on every frame that had
// not settled, and a heap walk per frame does not leave a game much to run on.
void Game::locateClientInstance(float deltaSeconds) {
    if (const uintptr_t global = sig::address(names::kClientInstance); global != 0) {
        clientInstance_ = memory::read<uintptr_t>(global);
        return;
    }

    const uintptr_t vtable = sig::address(names::kClientVtable);
    if (vtable == 0) {
        clientInstance_ = 0;
        return;
    }

    if (clientInstance_ != 0 && memory::read<uintptr_t>(clientInstance_) == vtable) return;

    clientInstance_ = 0;

    sinceScan_ += deltaSeconds;
    if (sinceScan_ < scanInterval_) return;
    sinceScan_ = 0.f;

    clientInstance_ = memory::findByVtable(vtable, [](uintptr_t candidate) {
        return looksLikeClientInstance(candidate);
    });

    // Backing off after a miss bounds what a wrong signature can cost: sitting on a
    // menu with nothing to find, this settles at one walk every half minute instead of
    // one a second, for ever.
    if (clientInstance_ == 0) {
        scanInterval_ = std::min(scanInterval_ * 2.f, kMaximumScanInterval);
        return;
    }
    scanInterval_ = kScanInterval;

    // Only when it changes: found once is news, found again on the next frame is a bug
    // reporting itself five hundred thousand times.
    if (clientInstance_ != 0 && clientInstance_ != announced_) {
        announced_ = clientInstance_;
        Log::info(kLog, "ClientInstance at {:#x}", clientInstance_);
    }
}

void Game::onFrame(FrameEvent& event) {
    locateClientInstance(event.deltaSeconds);
    available_ = memory::readable(reinterpret_cast<const void*>(clientInstance_), 8);

    if (!available_) {
        if (world_.inGame) {
            WorldLeaveEvent leave;
            leave.serverAddress = world_.serverAddress;
            events().emit(leave);
        }
        player_ = {};
        world_ = {};
        localPlayer_ = 0;
        return;
    }

    refreshPlayer();
    refreshWorld();

    if (player_.valid && event.deltaSeconds > 0.f) {
        const float travelled = player_.position.flatDistanceTo(previousPosition_);
        const float instant = travelled / event.deltaSeconds;

        if (instant < 100.f) {
            horizontalSpeed_ = approach(horizontalSpeed_, instant, event.deltaSeconds, 8.f);
        }
        previousPosition_ = player_.position;
    }
}

bool Game::inMenu() const {
    return !screen_.empty() && screen_ != "hud_screen";
}

uintptr_t Game::level() const {
    const int offset = sig::offset(names::kLevelOffset);
    if (clientInstance_ == 0 || offset < 0) return 0;

    const auto level = memory::read<uintptr_t>(clientInstance_ + static_cast<uintptr_t>(offset));
    return memory::readable(reinterpret_cast<const void*>(level), 8) ? level : 0;
}

bool Game::sendChat(const std::string& message) {
    if (!available_ || message.empty()) return false;

    const uintptr_t function = sig::address(names::kSendChat);
    if (!function) return false;

    ChatSendEvent event;
    event.message = message;
    events().emit(event);
    if (event.cancelled) return false;

    using SendFn = void(__fastcall*)(uintptr_t player, const void* message);

    // The same object the frame reads through, whichever road named it.
    const uintptr_t localPlayer = localPlayer_;
    if (!memory::readable(reinterpret_cast<const void*>(localPlayer), 8)) return false;

    const memory::GameString text(event.message);
    reinterpret_cast<SendFn>(function)(localPlayer, text.as());
    return true;
}

bool Game::showClientMessage(const std::string& message) {
    if (!available_ || message.empty()) return false;

    const uintptr_t function = sig::address(names::kClientMessage);
    if (!function) return false;

    using DisplayFn = void(__fastcall*)(uintptr_t instance, const void* message);

    const memory::GameString text(message);
    reinterpret_cast<DisplayFn>(function)(clientInstance_, text.as());
    return true;
}

const char* Game::compass(float yaw) {
    static const char* kNames[] = {"S", "SW", "W", "NW", "N", "NE", "E", "SE"};

    const float normalised = std::fmod(yaw + 180.f + 22.5f, 360.f);
    const int index = static_cast<int>((normalised < 0.f ? normalised + 360.f : normalised) / 45.f);
    return kNames[index % 8];
}

const char* Game::compassLong(float yaw) {
    static const char* kNames[] = {"South", "South-west", "West", "North-west",
                                   "North", "North-east", "East", "South-east"};

    const float normalised = std::fmod(yaw + 180.f + 22.5f, 360.f);
    const int index = static_cast<int>((normalised < 0.f ? normalised + 360.f : normalised) / 45.f);
    return kNames[index % 8];
}

// Neither route to the client instance is required on its own — a pack only carries
// whichever its build has — so Signatures::healthy() cannot answer this, and the one
// thing worth saying out loud at startup is whether *some* route exists.
bool Game::reachable() {
    return sig::has(names::kClientInstance) || sig::has(names::kClientVtable);
}

bool Game::packSeesPlayer() {
    return sig::offset(names::kLocalPlayerOffset) >= 0 || sig::has(names::kPlayerVtable) ||
           get().playerHook_.load(std::memory_order_acquire);
}

void bindGame() {
    Game& instance = Game::get();
    instance.requireSignatures();
    events().on<FrameEvent>(&instance, &Game::onFrame, EventPriority::First);

    Log::debug(kLog, "SDK facade bound");
}

}
