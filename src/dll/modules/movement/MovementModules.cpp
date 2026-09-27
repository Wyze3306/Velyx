#include "MovementModules.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "dll/hook/Turn.hpp"
#include "dll/hook/hooks/FovHook.hpp"
#include "dll/hook/hooks/PerspectiveHook.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

// Every module below that reads or writes a turn stands on the same thing: a door
// through which the game actually collects its mouse. Which door that is depends on
// the machine and on the pack, and whether any of them carries it is not known while
// the catalogue is being built, so they all ask rather than assume.
bool mouseHookReady() { return turn::carried(); }

// Judged from the speed where the pack cannot say: walking is four blocks a second and
// a bit, sprinting five and a half.
bool sprinting(const sdk::PlayerState& player) {
    return player.sprinting || sdk::game().horizontalSpeed() > 5.f;
}

class Zoom final : public Module {
public:
    Zoom()
        : Module("zoom", "Zoom", ModuleCategory::Movement,
                 "Narrows the field of view while the key is held.") {
        markWaitingUnless("the FOV hook", FovHook::live);
        settings.slider("amount", "Zoom factor", 3.f, 1.2f, 12.f, "", "x");
        settings.slider("smoothing", "Smoothness", 14.f, 1.f, 40.f,
                        "Lower is a slower transition.");
        settings.toggle("reduceSensitivity", "Reduce sensitivity", true,
                        "Keeps on-screen movement the same whatever the zoom.");
        settings.toggle("cinematic", "Cinematic zoom", false,
                        "Adds inertia to the camera while zoomed.");

        keybind() = Keybind{'C', false, false, false, Keybind::Mode::Hold};

        // Both outlive the key: released, the zoom eases back out over a few frames,
        // and the field of view has to keep being answered for until it has.
        always(&Zoom::onFrame);
        always(&Zoom::onFov);
        on(&Zoom::onTurn);
        addKeywords({"zoom", "fov", "magnify"});
    }

    void onEnable() override { progress_.to(1.f); }
    void onDisable() override { progress_.to(0.f); }

private:
    // The animation runs on the frame. The game asks for its field of view more than
    // once a frame, and an animation stepped on every ask runs twice as fast as set.
    void onFrame(FrameEvent& event) {
        progress_.speed = settings.value<float>("smoothing", 14.f);
        progress_.update(event.deltaSeconds);

        const float amount = settings.value<float>("amount", 3.f);
        currentFactor_ = lerp(1.f, amount, progress_.value);
    }

    void onFov(FovEvent& event) {
        if (progress_.value <= 0.001f) return;

        const float amount = settings.value<float>("amount", 3.f);
        event.fov = lerp(event.fov, event.fov / amount, progress_.value);
    }

    void onTurn(TurnDeltaEvent& event) {
        if (!settings.value<bool>("reduceSensitivity", true)) return;
        if (currentFactor_ <= 1.f) return;

        event.yaw /= currentFactor_;
        event.pitch /= currentFactor_;

        if (settings.value<bool>("cinematic", false)) {
            event.yaw = lerp(lastYaw_, event.yaw, 0.35f);
            event.pitch = lerp(lastPitch_, event.pitch, 0.35f);
            lastYaw_ = event.yaw;
            lastPitch_ = event.pitch;
        }
    }

    Animated progress_{0.f, 14.f};
    float currentFactor_ = 1.f;
    float lastYaw_ = 0.f;
    float lastPitch_ = 0.f;
};

class FovChanger final : public Module {
public:
    FovChanger()
        : Module("fov_changer", "Custom FOV", ModuleCategory::Movement,
                 "Allows a field of view past the game's own limits.") {
        markWaitingUnless("the FOV hook", FovHook::live);
        settings.slider("fov", "Field of view", 90.f, 30.f, 150.f, "", "°");
        settings.toggle("overrideAll", "Ignore the game's own effects", false,
                        "Also cancels the swings from sprinting and potions.");

        on(&FovChanger::onFov, EventPriority::High);
        addKeywords({"fov", "field of view"});
    }

private:
    void onFov(FovEvent& event) {
        const float target = settings.value<float>("fov", 90.f);
        event.fov = settings.value<bool>("overrideAll", false)
                        ? target
                        : target * (event.fov / 70.f);
    }
};

class JavaDynamicFov final : public Module {
public:
    JavaDynamicFov()
        : Module("java_dynamic_fov", "Dynamic FOV (Java)", ModuleCategory::Movement,
                 "Widens the field of view while sprinting, like Java Edition. Sprinting "
                 "is judged from your speed.") {
        markWaitingUnless("the FOV hook", FovHook::live);
        markNeedsGame();
        settings.slider("sprintBoost", "Sprint effect", 1.15f, 1.f, 1.5f, "", "x");
        settings.slider("speed", "Transition speed", 8.f, 1.f, 30.f);

        on(&JavaDynamicFov::onFrame);
        on(&JavaDynamicFov::onFov, EventPriority::Low);
        addKeywords({"fov", "sprint", "java", "dynamic"});
    }

    void onEnable() override { factor_.set(1.f); }

private:
    void onFrame(FrameEvent& event) {
        const auto& player = sdk::game().player();

        factor_.speed = settings.value<float>("speed", 8.f);
        factor_.to(player.valid && sprinting(player) ? settings.value<float>("sprintBoost", 1.15f)
                                                     : 1.f);
        factor_.update(event.deltaSeconds);
    }

    void onFov(FovEvent& event) { event.fov *= factor_.value; }

    Animated factor_{1.f, 8.f};
};

class SensMultiplier final : public Module {
public:
    SensMultiplier()
        : Module("sens_multiplier", "Sensitivity multiplier", ModuleCategory::Movement,
                 "Tunes sensitivity beyond the game's own slider.") {
        markWaitingUnless("the mouse hook", mouseHookReady);
        settings.slider("multiplier", "Multiplier", 1.f, 0.05f, 5.f, "", "x");
        settings.toggle("separateAxes", "Separate axes", false);
        settings.slider("horizontal", "Horizontal", 1.f, 0.05f, 5.f, "", "x");
        settings.slider("vertical", "Vertical", 1.f, 0.05f, 5.f, "", "x");

        const auto separate = [this] { return settings.value<bool>("separateAxes", false); };
        settings.find("horizontal")->visibleWhen = separate;
        settings.find("vertical")->visibleWhen = separate;
        settings.find("multiplier")->visibleWhen = [separate] { return !separate(); };

        on(&SensMultiplier::onTurn, EventPriority::High);
        addKeywords({"sensitivity", "dpi", "mouse"});
    }

private:
    void onTurn(TurnDeltaEvent& event) {
        if (settings.value<bool>("separateAxes", false)) {
            event.yaw *= settings.value<float>("horizontal", 1.f);
            event.pitch *= settings.value<float>("vertical", 1.f);
        } else {
            const float multiplier = settings.value<float>("multiplier", 1.f);
            event.yaw *= multiplier;
            event.pitch *= multiplier;
        }
    }
};

class HeldKeyToggle : public Module {
public:
    HeldKeyToggle(std::string id, std::string name, std::string description, int defaultKey)
        : Module(std::move(id), std::move(name), ModuleCategory::Movement, std::move(description)) {
        settings.dropdown("mode", "Mode", "Toggle", {"Toggle", "Always on"});
        settings.toggle("cancelOnGui", "Pause in menus", true);
        settings.intSlider("key", "Game key", defaultKey, 1, 255,
                           "The key the game actually uses for this action.")
            .advanced = true;

        mutablePermissions().inputSynthesis = true;

        on(&HeldKeyToggle::onFrame);
    }

protected:
    virtual bool shouldHold() const = 0;

private:
    void onFrame(FrameEvent& event) {
        const bool hold = shouldHold();
        if (hold == holding_) return;

        holding_ = hold;

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(settings.value<int>("key", 'W'));
        input.ki.dwFlags = hold ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(input));
    }

    bool holding_ = false;
};

class ToggleSprint final : public HeldKeyToggle {
public:
    ToggleSprint()
        : HeldKeyToggle("toggle_sprint", "Auto sprint",
                        "Keeps sprinting without holding the key.", VK_CONTROL) {
        markNeedsGame();
        addKeywords({"sprint", "running", "autosprint"});
    }

protected:
    bool shouldHold() const override {
        if (settings.value<std::string>("mode", "Toggle") == "Always on") return true;
        return sdk::game().player().valid && !sdk::game().inMenu();
    }
};

class ToggleSneak final : public HeldKeyToggle {
public:
    ToggleSneak()
        : HeldKeyToggle("toggle_sneak", "Toggle sneak",
                        "Stays sneaking without holding the key.", VK_SHIFT) {
        markNeedsGame();
        addKeywords({"sneak", "crouch", "shift"});
    }

protected:
    bool shouldHold() const override { return !sdk::game().inMenu(); }
};

class FreeLook final : public Module {
public:
    FreeLook()
        : Module("free_look", "FreeLook", ModuleCategory::Movement,
                 "Look around while the key is held; the view comes back to where it was "
                 "the moment you let go.") {
        // The way back is a turn of the exact opposite size. Through the game's own
        // door that is degrees; through an input door it is counts, which land just as
        // exactly because they go out and come back through the same constant.
        markWaitingUnless("the mouse hook", mouseHookReady);
        settings.toggle("thirdPerson", "Switch to third person", true,
                        "Needs the perspective hook; without it the view stays as it is.");
        settings.slider("maxYaw", "Horizontal amount", 180.f, 45.f, 180.f, "", "°");
        settings.toggle("returnSmoothly", "Ease back", true);
        settings.slider("returnDuration", "Return over", 0.15f, 0.02f, 0.6f, "", " s");

        settings.find("returnDuration")->visibleWhen = [this] {
            return settings.value<bool>("returnSmoothly", true);
        };

        keybind() = Keybind{VK_MENU, false, false, false, Keybind::Mode::Hold};

        // Last in line, so what is recorded is what the game is about to apply, after
        // every other module has had its say.
        on(&FreeLook::onTurn, EventPriority::Last);
        on(&FreeLook::onPerspective);

        // The way back runs after the key is up, which is after the module is off.
        always(&FreeLook::onFrame);
        addKeywords({"freelook", "perspective", "camera"});
    }

    void onEnable() override {
        // Pressed again while the view was still on its way back: what is left of the
        // way back is where the look-around now starts from.
        offsetYaw_ = -pendingYaw_;
        offsetPitch_ = -pendingPitch_;
        pendingYaw_ = pendingPitch_ = 0.f;
        elapsed_ = 0.f;
    }

    void onDisable() override {
        pendingYaw_ = -offsetYaw_;
        pendingPitch_ = -offsetPitch_;
        offsetYaw_ = offsetPitch_ = 0.f;
        elapsed_ = 0.f;

        if (!settings.value<bool>("returnSmoothly", true)) {
            turn::inject(pendingYaw_, pendingPitch_);
            pendingYaw_ = pendingPitch_ = 0.f;
        }
    }

private:
    void onTurn(TurnDeltaEvent& event) {
        if (inert() || event.cancelled) return;

        // Held to the amount set: what would go past it is taken off the turn rather
        // than remembered, so the way back never overshoots the amount either.
        const float maxYaw = settings.value<float>("maxYaw", 180.f);
        const float yaw = clamp(offsetYaw_ + event.yaw, -maxYaw, maxYaw) - offsetYaw_;
        event.yaw = yaw;
        offsetYaw_ += yaw;

        // The game clamps the pitch itself, at straight up and straight down, and says
        // nothing about what it threw away. Kept within a turn and a half so that a
        // look at the sky does not come back as a somersault.
        offsetPitch_ = clamp(offsetPitch_ + event.pitch, -178.f, 178.f);
    }

    void onPerspective(PerspectiveEvent& event) {
        if (settings.value<bool>("thirdPerson", true)) {
            event.perspective = Perspective::ThirdPersonBack;
        }
    }

    void onFrame(FrameEvent& event) {
        if (std::abs(pendingYaw_) < 0.001f && std::abs(pendingPitch_) < 0.001f) return;

        const float duration = std::max(0.02f, settings.value<float>("returnDuration", 0.15f));
        elapsed_ = std::min(elapsed_ + event.deltaSeconds, duration);

        float stepYaw = pendingYaw_ * (event.deltaSeconds / duration);
        float stepPitch = pendingPitch_ * (event.deltaSeconds / duration);
        pendingYaw_ -= stepYaw;
        pendingPitch_ -= stepPitch;

        // The last frame pays whatever the division left behind, rather than dropping
        // it: a view that comes back four degrees short has not come back.
        if (elapsed_ >= duration) {
            stepYaw += pendingYaw_;
            stepPitch += pendingPitch_;
            pendingYaw_ = pendingPitch_ = 0.f;
        }

        turn::inject(stepYaw, stepPitch);
    }

    float offsetYaw_ = 0.f;
    float offsetPitch_ = 0.f;
    float pendingYaw_ = 0.f;
    float pendingPitch_ = 0.f;
    float elapsed_ = 0.f;
};

class SnapLook final : public Module {
public:
    SnapLook()
        : Module("snap_look", "SnapLook", ModuleCategory::Movement,
                 "Turns the camera by a fixed angle in one press.") {
        markWaitingUnless("the mouse hook", mouseHookReady);
        mutablePermissions().inputSynthesis = true;

        settings.slider("angle", "Angle", 180.f, 45.f, 180.f, "", "°");
        settings.slider("duration", "Duration", 0.12f, 0.f, 0.6f,
                        "0 = instant.", " s");
        settings.toggle("resetOnSecondPress", "Return on the second press", true);
        settings.slider("calibration", "Calibration", 1.f, 0.1f, 5.f,
                        "The turn is asked for in mouse movement, and how far that goes "
                        "depends on the game's own sensitivity. Raise it if the turn "
                        "falls short, lower it if it overshoots.",
                        "x");

        // Through the game's own door a degree asked for is a degree turned, and there
        // is nothing to calibrate.
        settings.find("calibration")->visibleWhen = [] { return !turn::exact(); };

        keybind() = Keybind{'V', false, false, false, Keybind::Mode::Once};

        on(&SnapLook::onFrame);
        addKeywords({"snap", "180", "about-face"});
    }

    void onEnable() override {
        pending_ = settings.value<float>("angle", 180.f) * (flipped_ ? -1.f : 1.f);
        if (settings.value<bool>("resetOnSecondPress", true)) flipped_ = !flipped_;
        elapsed_ = 0.f;
    }

private:
    void onFrame(FrameEvent& event) {
        if (std::abs(pending_) < 0.01f) return;

        const float duration = settings.value<float>("duration", 0.12f);

        float step = 0.f;
        if (duration <= 0.f) {
            step = pending_;
            pending_ = 0.f;
        } else {
            elapsed_ = std::min(elapsed_ + event.deltaSeconds, duration);
            step = pending_ * (event.deltaSeconds / duration);
            pending_ -= step;

            // The last frame pays whatever the division left behind, rather than
            // dropping it: an about-face that stops four degrees short is not one.
            if (elapsed_ >= duration) {
                step += pending_;
                pending_ = 0.f;
            }
        }

        // Asked for rather than emitted. A turn the player did not make has no business
        // passing under their sensitivity multiplier on its way to the game.
        const float scale = turn::exact() ? 1.f : settings.value<float>("calibration", 1.f);
        turn::inject(step * scale, 0.f);
    }

    float pending_ = 0.f;
    float elapsed_ = 0.f;
    bool flipped_ = false;
};

class CinematicCamera final : public Module {
public:
    CinematicCamera()
        : Module("cinematic_camera", "Cinematic camera", ModuleCategory::Movement,
                 "Smooths camera movement for recording.") {
        markWaitingUnless("the mouse hook", mouseHookReady);
        settings.slider("smoothing", "Smoothing", 0.75f, 0.05f, 0.98f);
        settings.toggle("rollOnStrafe", "Tilt while strafing", false);
        settings.slider("rollAmount", "Tilt amount", 4.f, 0.5f, 15.f, "", "°");

        settings.find("rollAmount")->visibleWhen = [this] {
            return settings.value<bool>("rollOnStrafe", false);
        };

        on(&CinematicCamera::onTurn, EventPriority::Low);
        addKeywords({"cinematic", "smoothing", "camera"});
    }

private:
    void onTurn(TurnDeltaEvent& event) {
        const float smoothing = settings.value<float>("smoothing", 0.75f);

        smoothYaw_ = lerp(event.yaw, smoothYaw_, smoothing);
        smoothPitch_ = lerp(event.pitch, smoothPitch_, smoothing);

        event.yaw = smoothYaw_;
        event.pitch = smoothPitch_;
    }

    float smoothYaw_ = 0.f;
    float smoothPitch_ = 0.f;
};

class AutoPerspective final : public Module {
public:
    AutoPerspective()
        : Module("auto_perspective", "Automatic perspective", ModuleCategory::Movement,
                 "Switches view on its own depending on what you are doing.") {
        // The perspective itself is answered for through the hook. What the player is
        // doing — swimming, gliding, riding — is a flag no pack carries an offset for
        // yet, and without it there is nothing to switch on.
        markWaitingUnless("the perspective hook", PerspectiveHook::live);
        markWaitingFor("the player's state flags");
        markNeedsGame();
        settings.dropdown("onSwim", "While swimming", "None",
                          {"None", "First person", "Third person", "Third person front"});
        settings.dropdown("onElytra", "On elytra", "Third person",
                          {"None", "First person", "Third person", "Third person front"});
        settings.dropdown("onRide", "While riding", "None",
                          {"None", "First person", "Third person", "Third person front"});
        settings.toggle("restore", "Restore the previous view", true);

        on(&AutoPerspective::onPerspective);
        addKeywords({"perspective", "view", "camera"});
    }

private:
    static Perspective parse(const std::string& value, Perspective fallback) {
        if (value == "First person") return Perspective::FirstPerson;
        if (value == "Third person") return Perspective::ThirdPersonBack;
        if (value == "Third person front") return Perspective::ThirdPersonFront;
        return fallback;
    }

    void onPerspective(PerspectiveEvent& event) {
        const auto& player = sdk::game().player();
        if (!player.valid) return;

        if (player.inWater) {
            event.perspective =
                parse(settings.value<std::string>("onSwim", "None"), event.perspective);
        } else if (player.flying) {
            event.perspective =
                parse(settings.value<std::string>("onElytra", "Third person"), event.perspective);
        }
    }
};

class NullMovement final : public Module {
public:
    NullMovement()
        : Module("null_movement", "Null Movement", ModuleCategory::Movement,
                 "Gives priority to the last direction key pressed.") {
        settings.toggle("horizontal", "Left/right axes", true);
        settings.toggle("vertical", "Forward/back axes", true);

        mutablePermissions().inputSynthesis = true;

        on(&NullMovement::onKey, EventPriority::High);
        addKeywords({"null movement", "movement", "priority", "keys"});
    }

private:
    void onKey(KeyEvent& event) {

        const auto release = [](int key) {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = static_cast<WORD>(key);
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(input));
        };

        if (!event.down || event.repeat) return;

        if (settings.value<bool>("horizontal", true)) {
            if (event.key == 'Q' || event.key == 'A') release('D');
            if (event.key == 'D') { release('Q'); release('A'); }
        }
        if (settings.value<bool>("vertical", true)) {
            if (event.key == 'Z' || event.key == 'W') release('S');
            if (event.key == 'S') { release('Z'); release('W'); }
        }
    }
};

}

void registerMovementModules(ModuleManager& manager) {
    manager.add<Zoom>();
    manager.add<FovChanger>();
    manager.add<JavaDynamicFov>();
    manager.add<SensMultiplier>();
    manager.add<ToggleSprint>();
    manager.add<ToggleSneak>();
    manager.add<FreeLook>();
    manager.add<SnapLook>();
    manager.add<CinematicCamera>();
    manager.add<AutoPerspective>();
    manager.add<NullMovement>();
}

}
