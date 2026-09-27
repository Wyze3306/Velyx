#pragma once

#include <cstdint>
#include <string>

#include "core/Color.hpp"
#include "core/Math.hpp"
#include "dll/event/EventBus.hpp"

namespace velyx {

class Renderer;
class Actor;
class Module;

struct FrameEvent : Event {
    float deltaSeconds = 0.f;
    uint64_t frameIndex = 0;
    Vec2 screenSize;
};

struct RenderEvent : Event {
    Renderer* renderer = nullptr;
    Vec2 screenSize;
    float deltaSeconds = 0.f;

    bool guiOpen = false;
};

struct RenderTopEvent : Event {
    Renderer* renderer = nullptr;
    Vec2 screenSize;
    float deltaSeconds = 0.f;
};

struct Render3DEvent : Event {
    float deltaSeconds = 0.f;
};

struct SwapchainResizeEvent : Event {
    uint32_t width = 0;
    uint32_t height = 0;

    // True only for the window telling us it changed. The render thread emits this
    // event again once it has rebuilt for the new size, so that everything can lay
    // itself out — and a rebuild must not be mistaken for another window change, or
    // the two feed each other and the overlay never stops tearing itself down.
    bool fromWindow = false;
};

// The bookends of an interactive border drag. Between them the window size is in
// motion and nothing may be rebuilt from the swapchain.
struct WindowDragEvent : Event {
    bool dragging = false;
};

struct DeviceLostEvent : Event {};

// The window closing is the only warning a normal exit gives: at DLL_PROCESS_DETACH
// the process is already going and Velyx deliberately does nothing there, so without
// this every clean exit would be counted as a crash.
struct GameClosingEvent : Event {};

struct KeyEvent : Cancellable {
    int key = 0;
    bool down = false;
    bool repeat = false;
    bool alt = false;
    bool ctrl = false;
    bool shift = false;
};

struct CharEvent : Cancellable {
    unsigned int codepoint = 0;
};

enum class MouseButton { Left, Right, Middle, X1, X2, None };
enum class MouseAction { Press, Release, Move, Wheel };

struct MouseEvent : Cancellable {
    MouseButton button = MouseButton::None;
    MouseAction action = MouseAction::Move;
    Vec2 position;
    float wheelDelta = 0.f;
};

// One movement of the mouse, in degrees, before the game applies it. Handlers scale,
// smooth or cancel it; velyx::turn hands the result on. Exact means the degrees are
// the game's own — the movement came through LocalPlayer::applyTurnDelta rather than
// through an input door, where a count is only nominally worth a fixed fraction of a
// degree.
struct TurnDeltaEvent : Cancellable {
    float yaw = 0.f;
    float pitch = 0.f;
    bool exact = false;
};

// Twenty times a second while there is a world, from the frame clock rather than from
// the game's own tick: a steady beat for anything that should not run once per frame.
struct TickEvent : Event {
    uint64_t tick = 0;
};

struct WorldJoinEvent : Event {
    std::string serverAddress;
    uint16_t serverPort = 0;
    std::string worldName;
    bool multiplayer = false;
};

struct WorldLeaveEvent : Event {
    std::string serverAddress;
    long long sessionSeconds = 0;
};

struct ChatReceiveEvent : Cancellable {
    std::string sender;
    std::string message;
    std::string rawMessage;
    int type = 0;
};

struct ChatSendEvent : Cancellable {
    std::string message;
};

// Another Velyx client, seen here for the first time. Whoever heard it — the chat
// hook on the game's thread, the roster watcher on its own — hands it to the render
// thread first, so a handler is free to notify, to draw, or to answer in chat.
struct PeerFoundEvent : Event {
    std::string name;
    std::string version;
    bool local = false;
};

// A hit of the player's that the game registered — GameMode::attack ran with a target.
// Heard on the game's thread and handed to the render thread, so the target is an
// entry of this frame's snapshot, or a copy read off the address the game named when
// the pack cannot walk the list. Valid for the frame, like every Actor.
struct AttackEvent : Cancellable {
    Actor* target = nullptr;
};

// The player took damage. Read off the health rather than heard from the game: it is
// emitted the frame the number drops, with the drop as the damage.
struct HurtEvent : Event {
    float damage = 0.f;
    float health = 0.f;
    Color tint = Color::rgb8(255, 0, 0, 76);
};

// An entity was hurt. By the player when it follows one of their own hits, and then
// it is emitted the moment the hit registers; otherwise read off the snapshot, the
// frame the entity's health drops, for as long as the pack can read one.
struct ActorHurtEvent : Event {
    Actor* actor = nullptr;
    float damage = 0.f;
    bool byPlayer = false;
    Color tint = Color::rgb8(255, 0, 0, 76);
};

// The player's health reached zero, and came back from it.
struct DeathEvent : Event {
    std::string cause;
    Vec3 position;
};

struct RespawnEvent : Event {};

enum class Perspective { FirstPerson = 0, ThirdPersonBack = 1, ThirdPersonFront = 2 };

// The game asking which perspective to draw, answered by the game and then by the
// handlers. Emitted from inside the game's own getter, as often as it asks.
struct PerspectiveEvent : Cancellable {
    Perspective perspective = Perspective::FirstPerson;
};

// The game asking what field of view to draw with, in degrees whatever it keeps
// inside. Emitted from inside the game's own getter, which runs more than once a
// frame: a handler that animates should do so on the frame, and only apply here.
struct FovEvent : Event {
    float fov = 70.f;
};

struct SoundEvent : Cancellable {
    std::string name;
    Vec3 position;
    float volume = 1.f;
    float pitch = 1.f;
};

// The game's own screen name changed — the pack has to carry the offset to read it.
struct ScreenChangeEvent : Event {
    std::string previous;
    std::string current;
};

struct PacketEvent : Cancellable {
    int packetId = 0;
    void* packet = nullptr;
    bool outgoing = false;
};

struct ModuleToggleEvent : Event {
    Module* module = nullptr;
    bool enabled = false;

    bool byUser = true;
};

struct ProfileChangeEvent : Event {
    std::string previous;
    std::string current;
    bool automatic = false;
};

struct ThemeChangeEvent : Event {
    std::string name;
};

struct SettingChangeEvent : Event {
    Module* module = nullptr;
    std::string setting;
};

}
