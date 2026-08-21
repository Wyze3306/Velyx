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

struct TurnDeltaEvent : Cancellable {
    float yaw = 0.f;
    float pitch = 0.f;
};

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

struct AttackEvent : Cancellable {
    Actor* target = nullptr;
};

struct HurtEvent : Event {
    float damage = 0.f;
    Color tint = Color::rgb8(255, 0, 0, 76);
};

struct ActorHurtEvent : Event {
    Actor* actor = nullptr;
    Color tint = Color::rgb8(255, 0, 0, 76);
};

struct DeathEvent : Event {
    std::string cause;
    Vec3 position;
};

struct RespawnEvent : Event {};

enum class Perspective { FirstPerson = 0, ThirdPersonBack = 1, ThirdPersonFront = 2 };

struct PerspectiveEvent : Cancellable {
    Perspective perspective = Perspective::FirstPerson;
};

struct FovEvent : Event {
    float fov = 70.f;
};

struct SoundEvent : Cancellable {
    std::string name;
    Vec3 position;
    float volume = 1.f;
    float pitch = 1.f;
};

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
