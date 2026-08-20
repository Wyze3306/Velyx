#include "Game.hpp"

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
}

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string readStdString(uintptr_t address) {
    if (!address || !memory::readable(reinterpret_cast<const void*>(address), 32)) return {};

    const auto size = memory::read<uint64_t>(address + 16);
    const auto capacity = memory::read<uint64_t>(address + 24);

    if (size > 0x10000) return {};

    if (capacity > 15) {
        const auto data = memory::read<uintptr_t>(address);
        if (!memory::readable(reinterpret_cast<const void*>(data), size)) return {};
        return std::string(reinterpret_cast<const char*>(data), size);
    }

    return std::string(reinterpret_cast<const char*>(address), size);
}

}

Game& Game::get() {
    static Game instance;
    return instance;
}

void Game::requireSignatures() {
    Signatures& registry = Signatures::get();

    SignatureSpec clientInstance;
    clientInstance.name = names::kClientInstance;
    clientInstance.kind = SignatureKind::Relative;
    clientInstance.required = true;
    clientInstance.owner = "SDK";
    registry.require(clientInstance);

    SignatureSpec sendChat;
    sendChat.name = names::kSendChat;
    sendChat.owner = "SDK";
    registry.require(sendChat);

    SignatureSpec clientMessage;
    clientMessage.name = names::kClientMessage;
    clientMessage.owner = "SDK";
    registry.require(clientMessage);

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

void Game::refreshPlayer() {
    player_ = {};

    const int localPlayerOffset = sig::offset(names::kLocalPlayerOffset);
    if (clientInstance_ == 0 || localPlayerOffset < 0) return;

    const auto localPlayer = memory::read<uintptr_t>(
        clientInstance_ + static_cast<uintptr_t>(localPlayerOffset));
    if (!memory::readable(reinterpret_cast<const void*>(localPlayer), 8)) return;

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
        player_.name = readStdString(localPlayer + static_cast<uintptr_t>(nameOffset));
    }
}

void Game::refreshWorld() {
    const bool wasInGame = world_.inGame;

    world_.inGame = player_.valid;

    if (const int screenOffset = sig::offset(names::kScreenNameOffset);
        screenOffset >= 0 && clientInstance_ != 0) {
        screen_ = readStdString(clientInstance_ + static_cast<uintptr_t>(screenOffset));
    }

    if (const int addressOffset = sig::offset(names::kServerAddress);
        addressOffset >= 0 && clientInstance_ != 0) {
        world_.serverAddress = readStdString(clientInstance_ + static_cast<uintptr_t>(addressOffset));
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
    }
}

void Game::onFrame(FrameEvent& event) {
    const uintptr_t instanceAddress = sig::address(names::kClientInstance);
    clientInstance_ = instanceAddress != 0 ? memory::read<uintptr_t>(instanceAddress) : 0;
    available_ = memory::readable(reinterpret_cast<const void*>(clientInstance_), 8);

    if (!available_) {
        if (world_.inGame) {
            WorldLeaveEvent leave;
            leave.serverAddress = world_.serverAddress;
            events().emit(leave);
        }
        player_ = {};
        world_ = {};
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

bool Game::sendChat(const std::string& message) {
    if (!available_ || message.empty()) return false;

    const uintptr_t function = sig::address(names::kSendChat);
    if (!function) return false;

    ChatSendEvent event;
    event.message = message;
    events().emit(event);
    if (event.cancelled) return false;

    using SendFn = void(__fastcall*)(uintptr_t player, const std::string* message);
    const int localPlayerOffset = sig::offset(names::kLocalPlayerOffset);
    if (localPlayerOffset < 0) return false;

    const auto localPlayer =
        memory::read<uintptr_t>(clientInstance_ + static_cast<uintptr_t>(localPlayerOffset));
    if (!memory::readable(reinterpret_cast<const void*>(localPlayer), 8)) return false;

    reinterpret_cast<SendFn>(function)(localPlayer, &event.message);
    return true;
}

bool Game::showClientMessage(const std::string& message) {
    if (!available_ || message.empty()) return false;

    const uintptr_t function = sig::address(names::kClientMessage);
    if (!function) return false;

    using DisplayFn = void(__fastcall*)(uintptr_t instance, const std::string* message);
    reinterpret_cast<DisplayFn>(function)(clientInstance_, &message);
    return true;
}

const char* Game::compass(float yaw) {
    static const char* kNames[] = {"S", "SO", "O", "NO", "N", "NE", "E", "SE"};

    const float normalised = std::fmod(yaw + 180.f + 22.5f, 360.f);
    const int index = static_cast<int>((normalised < 0.f ? normalised + 360.f : normalised) / 45.f);
    return kNames[index % 8];
}

const char* Game::compassLong(float yaw) {
    static const char* kNames[] = {"Sud",   "Sud-Ouest", "Ouest", "Nord-Ouest",
                                   "Nord",  "Nord-Est",  "Est",   "Sud-Est"};

    const float normalised = std::fmod(yaw + 180.f + 22.5f, 360.f);
    const int index = static_cast<int>((normalised < 0.f ? normalised + 360.f : normalised) / 45.f);
    return kNames[index % 8];
}

void bindGame() {
    Game& instance = Game::get();
    instance.requireSignatures();
    events().on<FrameEvent>(&instance, &Game::onFrame, EventPriority::First);
    Log::debug(kLog, "façade SDK enregistrée");
}

}
