#include "UtilityModules.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/feature/Clipboard.hpp"
#include "dll/feature/Services.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/render/ColorMatrix.hpp"
#include "dll/sdk/Camera.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Utility";

// Brightness the game never offered, done to the picture rather than to the game. No
// signature, no hook, no gamma register: the overlay already owns a colour matrix over
// the whole frame, and lifting the shadows through it is the same result by an honest
// route. It cannot see through a wall — it only stops the wall from being black.
class Fullbright final : public Module {
public:
    Fullbright()
        : Module("fullbright", "Fullbright", ModuleCategory::Render,
                 "Lifts the dark of a cave or a night without touching the game.") {
        settings.header("Brightness");
        settings.slider("lift", "Lift the shadows", 0.35f, 0.f, 0.8f);
        settings.slider("gain", "Overall brightness", 1.05f, 0.8f, 1.6f, "", "x");
        settings.slider("saturation", "Colour back", 1.12f, 0.6f, 1.8f,
                        "Lifting shadows washes colour out; this puts some back.", "x");

        settings.header("Where");
        settings.toggle("skipMenus", "Leave the client's own menus alone", true);
        settings.slider("fade", "Ease in over", 0.35f, 0.f, 2.f, "", " s");

        on(&Fullbright::onRender, EventPriority::Low);
        addKeywords({"fullbright", "brightness", "gamma", "night", "cave", "light"});
    }

    void onEnable() override { strength_.to(1.f); }
    void onDisable() override { strength_.set(0.f); }

private:
    void onRender(RenderEvent& event) {
        const float fade = settings.value<float>("fade", 0.35f);
        strength_.speed = fade > 0.01f ? 1.f / fade : 1000.f;
        strength_.to(1.f);
        strength_.update(event.deltaSeconds);

        if (strength_.value <= 0.01f) return;
        if (event.guiOpen && settings.value<bool>("skipMenus", true)) return;

        const float amount = strength_.value;
        const float lift = settings.value<float>("lift", 0.35f) * amount;
        const float gain = 1.f + (settings.value<float>("gain", 1.05f) - 1.f) * amount;
        const float saturation = 1.f + (settings.value<float>("saturation", 1.12f) - 1.f) * amount;

        if (lift <= 0.001f && std::abs(gain - 1.f) <= 0.001f &&
            std::abs(saturation - 1.f) <= 0.001f) {
            return;
        }

        ColorMatrix matrix = kIdentityMatrix;
        if (lift > 0.001f) matrix = multiply(matrix, liftMatrix(lift));
        if (std::abs(gain - 1.f) > 0.001f) matrix = multiply(matrix, gainMatrix(gain));
        if (std::abs(saturation - 1.f) > 0.001f) {
            matrix = multiply(matrix, saturationMatrix(saturation));
        }

        event.renderer->colorMatrix(
            Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y), matrix.data());
    }

    Animated strength_{0.f, 3.f};
};

struct Waypoint {
    std::string name;
    Vec3 position;
    Color color = palette::kMint;
};

// One file for every world, keyed by the server or the world it was dropped in, so a
// base marked on one server does not follow you onto another.
class WaypointStore {
public:
    static WaypointStore& get() {
        static WaypointStore instance;
        return instance;
    }

    // Sanitising a server address is cheap, and doing it on every frame that draws a
    // marker is still work for an answer that changes when you join somewhere.
    [[nodiscard]] const std::string& worldKey() {
        const sdk::WorldState& world = sdk::game().world();
        const std::string& source = !world.serverAddress.empty()
                                        ? world.serverAddress
                                        : (!world.worldName.empty() ? world.worldName : local());

        if (source != keySource_) {
            keySource_ = source;
            key_ = Paths::sanitize(source);
        }
        return key_;
    }

    [[nodiscard]] std::vector<Waypoint>& current() {
        load();
        return worlds_[worldKey()];
    }

    void add(Waypoint point) {
        current().push_back(std::move(point));
        save();
    }

    bool removeNearest(const Vec3& position, float within) {
        std::vector<Waypoint>& list = current();
        if (list.empty()) return false;

        auto nearest = list.end();
        float best = within;

        for (auto it = list.begin(); it != list.end(); ++it) {
            const float distance = it->position.distanceTo(position);
            if (distance > best) continue;
            best = distance;
            nearest = it;
        }

        if (nearest == list.end()) return false;

        list.erase(nearest);
        save();
        return true;
    }

    void clear() {
        current().clear();
        save();
    }

    void save() const {
        nlohmann::json document = nlohmann::json::object();

        for (const auto& [key, list] : worlds_) {
            nlohmann::json array = nlohmann::json::array();
            for (const Waypoint& point : list) {
                array.push_back({{"name", point.name},
                                 {"x", point.position.x},
                                 {"y", point.position.y},
                                 {"z", point.position.z},
                                 {"color", point.color.toHex(true)}});
            }
            document[key] = std::move(array);
        }

        try {
            std::ofstream file(path());
            if (file) file << document.dump(2);
        } catch (const std::exception& error) {
            Log::warn(kLog, "waypoints could not be written: {}", error.what());
        }
    }

private:
    WaypointStore() = default;

    [[nodiscard]] static std::filesystem::path path() {
        return Paths::waypoints() / "waypoints.json";
    }

    void load() {
        if (loaded_) return;
        loaded_ = true;

        try {
            std::ifstream file(path());
            if (!file) return;

            nlohmann::json document;
            file >> document;
            if (!document.is_object()) return;

            for (const auto& [key, array] : document.items()) {
                if (!array.is_array()) continue;

                std::vector<Waypoint>& list = worlds_[key];
                for (const auto& entry : array) {
                    Waypoint point;
                    point.name = entry.value("name", std::string("Point"));
                    point.position = {entry.value("x", 0.f), entry.value("y", 0.f),
                                      entry.value("z", 0.f)};
                    point.color = Color::fromHex(entry.value("color", std::string("#3DDC84")),
                                                 palette::kMint);
                    list.push_back(std::move(point));
                }
            }
        } catch (const std::exception& error) {
            Log::warn(kLog, "waypoints could not be read: {}", error.what());
        }
    }

    static const std::string& local() {
        static const std::string name = "local";
        return name;
    }

    std::unordered_map<std::string, std::vector<Waypoint>> worlds_;
    std::string keySource_;
    std::string key_ = "local";
    bool loaded_ = false;
};

// A marker in the world, drawn where it is and pinned to the edge when it is behind
// you. It needs nothing the coordinates readout does not already read, which is why it
// works on a pack that knows almost nothing.
class Waypoints final : public Module {
public:
    Waypoints()
        : Module("waypoints", "Waypoints", ModuleCategory::Render,
                 "Marks a place and keeps pointing at it, on screen or off it.") {
        mutablePermissions().files = true;

        settings.header("Dropping one");
        settings.text("nextName", "Name for the next one", "",
                      "Empty numbers them.");
        settings.keybind("dropKey", "Drop here",
                         Keybind{VK_F7, false, false, false, Keybind::Mode::Once});
        settings.keybind("removeKey", "Remove the nearest",
                         Keybind{VK_F7, false, true, false, Keybind::Mode::Once});
        settings.color("color", "Colour", palette::kMint);
        settings.button("clear", "Remove them all", [] {
            WaypointStore::get().clear();
            Notifications::info("Waypoints", "All of this world's markers are gone.");
        });

        settings.header("On screen");
        settings.slider("maxDistance", "Show up to", 512.f, 16.f, 4096.f, "", " m");
        settings.toggle("showDistance", "Distance", true);
        settings.toggle("showHeight", "How far above or below", true);
        settings.toggle("edgeMarkers", "Pin to the edge when off screen", true);
        settings.slider("size", "Text size", 13.f, 8.f, 26.f, "", "px");
        settings.slider("edgeInset", "Edge margin", 40.f, 10.f, 200.f, "", "px");
        settings.toggle("hideInMenus", "Hide in menus", true);

        on(&Waypoints::onKey);
        on(&Waypoints::onRender);

        addKeywords({"waypoint", "marker", "navigation", "base", "coordinates"});
    }

private:
    static bool matches(const Keybind& bind, const KeyEvent& event) {
        if (!bind.bound() || event.key != bind.key) return false;
        return bind.ctrl == event.ctrl && bind.shift == event.shift && bind.alt == event.alt;
    }

    // Keybinds arrive on the message thread; the file is written on the render one.
    void onKey(KeyEvent& event) {
        if (!event.down || event.repeat) return;

        if (matches(settings.value<Keybind>("dropKey", Keybind{}), event)) {
            pendingDrop_.store(true, std::memory_order_release);
        } else if (matches(settings.value<Keybind>("removeKey", Keybind{}), event)) {
            pendingRemove_.store(true, std::memory_order_release);
        }
    }

    void applyPending() {
        const bool drop = pendingDrop_.exchange(false, std::memory_order_acq_rel);
        const bool remove = pendingRemove_.exchange(false, std::memory_order_acq_rel);
        if (!drop && !remove) return;

        const sdk::PlayerState& player = sdk::game().player();
        if (!player.valid) return;

        if (drop) {
            Waypoint point;
            point.name = settings.value<std::string>("nextName", "");
            if (point.name.empty()) {
                point.name = std::format("Point {}", WaypointStore::get().current().size() + 1);
            }
            point.position = player.position;
            point.color = settings.value<Color>("color", palette::kMint);

            WaypointStore::get().add(point);
            settings.set("nextName", SettingValue{std::string{}});

            Notifications::success("Waypoint dropped", point.name);
        }

        if (remove) {
            if (WaypointStore::get().removeNearest(player.position, 24.f)) {
                Notifications::info("Waypoint removed");
            } else {
                Notifications::warning("Waypoints", "Nothing within twenty-four blocks.");
            }
        }
    }

    void onRender(RenderEvent& event) {
        applyPending();

        if (event.guiOpen) return;
        if (settings.value<bool>("hideInMenus", true) && sdk::game().inMenu()) return;
        if (!sdk::camera().valid()) return;

        const std::vector<Waypoint>& list = WaypointStore::get().current();
        if (list.empty()) return;

        Renderer& renderer = *event.renderer;
        const Theme& active = theme();
        const sdk::Camera& view = sdk::camera();
        const Vec3 eye = sdk::game().player().position;

        const float range = settings.value<float>("maxDistance", 512.f);
        const float inset = settings.value<float>("edgeInset", 40.f);
        const bool edges = settings.value<bool>("edgeMarkers", true);

        FontSpec spec;
        spec.family = active.fontFamily;
        spec.size = settings.value<float>("size", 13.f) * active.fontScale;
        spec.weight = FontWeight::SemiBold;
        spec.align = TextAlign::Center;
        spec.valign = TextVAlign::Middle;

        const Rect screen = Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y);
        const Rect safe = screen.inflated(-inset);

        for (const Waypoint& point : list) {
            const float distance = point.position.distanceTo(eye);
            if (distance > range) continue;

            std::string label = point.name;
            if (settings.value<bool>("showDistance", true)) {
                label += std::format("  {} m", static_cast<int>(distance));
            }
            if (settings.value<bool>("showHeight", true)) {
                const int height = static_cast<int>(std::round(point.position.y - eye.y));
                if (std::abs(height) >= 2) label += std::format("  {}{}", height > 0 ? "↑" : "↓",
                                                                std::abs(height));
            }

            Vec2 anchor;
            const bool inFront = view.project({point.position.x, point.position.y + 1.f,
                                               point.position.z}, anchor);

            if (inFront && safe.contains(anchor)) {
                drawMarker(renderer, anchor, label, point.color, spec, false);
                continue;
            }
            if (!edges) continue;

            drawMarker(renderer, edgePosition(point.position, eye, safe, anchor, inFront), label,
                       point.color.fade(0.8f), spec, true);
        }
    }

    // Where a marker sits once it has left the picture. In front, the projection still
    // says which way; behind, only the bearing does — and a bearing is what you want
    // anyway, because it says which way to turn.
    static Vec2 edgePosition(const Vec3& target, const Vec3& eye, const Rect& safe, Vec2 projected,
                             bool inFront) {
        const Vec2 centre = safe.center();

        Vec2 direction;
        if (inFront) {
            direction = projected - centre;
        } else {
            const Vec3 delta = target - eye;
            direction = {delta.dot(sdk::camera().right()), -delta.dot(sdk::camera().forward())};
        }

        const float length = direction.length();
        if (length < 1e-3f) return centre;
        direction = direction / length;

        // The largest step along the ray that still lands inside the box.
        const float horizontal = std::abs(direction.x) > 1e-4f
                                     ? safe.width() * 0.5f / std::abs(direction.x)
                                     : 1e9f;
        const float vertical = std::abs(direction.y) > 1e-4f
                                   ? safe.height() * 0.5f / std::abs(direction.y)
                                   : 1e9f;
        const float step = std::min(horizontal, vertical);

        return {centre.x + direction.x * step, centre.y + direction.y * step};
    }

    static void drawMarker(Renderer& renderer, Vec2 at, const std::string& label,
                           const Color& colour, const FontSpec& spec, bool onEdge) {
        const Theme& active = theme();

        const float radius = onEdge ? 5.f : 4.f;
        renderer.fillCircle(at, radius + 1.5f, Color{0.f, 0.f, 0.f, colour.a * 0.6f});
        renderer.fillCircle(at, radius, colour);

        if (onEdge) renderer.strokeCircle(at, radius + 4.f, colour.fade(0.5f), 1.4f);

        const Vec2 size = renderer.measure(label, spec);
        const Rect text = Rect::fromSize(at.x - size.x * 0.5f, at.y - size.y - radius - 6.f,
                                         size.x, size.y);

        renderer.fillRounded(text.inflated(4.f), active.backgroundDeep.withAlpha(0.6f * colour.a),
                             4.f);
        renderer.textShadowed(label, text, active.text.fade(colour.a), spec);
    }

    std::atomic<bool> pendingDrop_{false};
    std::atomic<bool> pendingRemove_{false};
};

// Four keys, four messages. On a server whose whole loop is a queue command and a
// "gg", typing them is the part of the evening you do not remember fondly.
class ChatMacros final : public Module {
public:
    static constexpr int kSlots = 4;

    ChatMacros()
        : Module("chat_macros", "Chat macros", ModuleCategory::Utility,
                 "A key sends a message, so a queue command is one press.") {
        for (int slot = 0; slot < kSlots; ++slot) {
            settings.header(std::format("Macro {}", slot + 1));
            settings.text(messageId(slot), "Message", slot == 0 ? "gg" : "");
            settings.keybind(keyId(slot), "Key", Keybind{});
        }

        settings.header("Rules");
        settings.toggle("onlyInGame", "Only while in a world", true);
        settings.toggle("skipInMenus", "Not while a screen is open", true);
        settings.slider("cooldown", "Cooldown", 1.f, 0.1f, 10.f,
                        "The same macro will not fire again before this.", " s");
        settings.toggle("notify", "Confirm what was sent", false);

        on(&ChatMacros::onKey);
        on(&ChatMacros::onFrame);

        addKeywords({"macro", "chat", "message", "bind", "queue", "gg"});
    }

private:
    [[nodiscard]] static std::string messageId(int slot) {
        return std::format("message{}", slot + 1);
    }
    [[nodiscard]] static std::string keyId(int slot) { return std::format("key{}", slot + 1); }

    void onKey(KeyEvent& event) {
        if (!event.down || event.repeat) return;

        for (int slot = 0; slot < kSlots; ++slot) {
            const Keybind bind = settings.value<Keybind>(keyId(slot), Keybind{});
            if (!bind.bound() || event.key != bind.key) continue;
            if (bind.ctrl != event.ctrl || bind.shift != event.shift || bind.alt != event.alt) {
                continue;
            }

            // Queued, never sent here: this is the game's message thread, and the chat
            // call goes through the game's own code on the render one.
            pending_.store(slot, std::memory_order_release);
            return;
        }
    }

    void onFrame(FrameEvent& event) {
        for (float& elapsed : sinceSent_) elapsed += event.deltaSeconds;

        const int slot = pending_.exchange(-1, std::memory_order_acq_rel);
        if (slot < 0 || slot >= kSlots) return;

        if (sinceSent_[static_cast<size_t>(slot)] < settings.value<float>("cooldown", 1.f)) return;

        if (settings.value<bool>("onlyInGame", true) && !sdk::game().world().inGame) return;
        if (settings.value<bool>("skipInMenus", true) && sdk::game().inMenu()) return;

        const std::string message = settings.value<std::string>(messageId(slot), "");
        if (message.empty()) return;

        if (!sdk::game().sendChat(message)) {
            Notifications::warning("Chat macros",
                                   "The chat is not reachable on this build of the game.");
            sinceSent_[static_cast<size_t>(slot)] = 0.f;
            return;
        }

        sinceSent_[static_cast<size_t>(slot)] = 0.f;

        if (settings.value<bool>("notify", false)) Notifications::info("Sent", message);
    }

    std::atomic<int> pending_{-1};
    std::array<float, kSlots> sinceSent_{100.f, 100.f, 100.f, 100.f};
};

// Where you are, in a form someone else can use. Reading coordinates off a HUD into a
// chat box is the small daily friction this removes.
class CoordinateTools final : public Module {
public:
    CoordinateTools()
        : Module("coord_tools", "Coordinate tools", ModuleCategory::Utility,
                 "Copies or announces where you are, in one press.") {
        mutablePermissions().clipboard = true;

        settings.header("Keys");
        settings.keybind("copyKey", "Copy to the clipboard",
                         Keybind{'C', true, true, false, Keybind::Mode::Once});
        settings.keybind("sendKey", "Send to the chat", Keybind{});

        settings.header("Format");
        settings.dropdown("format", "Layout", "Plain",
                          {"Plain", "With the dimension", "Slash command", "Just the numbers"});
        settings.intSlider("decimals", "Decimals", 0, 0, 3);
        settings.toggle("includeName", "Prefix with your name", false);
        settings.toggle("notify", "Confirm with a notification", true);

        settings.find("includeName")->visibleWhen = [this] {
            return settings.value<std::string>("format", "Plain") != "Slash command";
        };

        on(&CoordinateTools::onKey);
        on(&CoordinateTools::onFrame);

        addKeywords({"coordinates", "copy", "clipboard", "share", "position"});
    }

private:
    [[nodiscard]] std::string format() const {
        const sdk::PlayerState& player = sdk::game().player();
        const int decimals = settings.value<int>("decimals", 0);

        const auto number = [decimals](float value) {
            return strings::formatFloat(static_cast<double>(value), decimals);
        };

        const std::string style = settings.value<std::string>("format", "Plain");

        if (style == "Slash command") {
            return std::format("/tp {} {} {}", number(player.position.x), number(player.position.y),
                               number(player.position.z));
        }

        std::string body;
        if (style == "Just the numbers") {
            body = std::format("{} {} {}", number(player.position.x), number(player.position.y),
                               number(player.position.z));
        } else {
            body = std::format("X {}  Y {}  Z {}", number(player.position.x),
                               number(player.position.y), number(player.position.z));
        }

        if (style == "With the dimension") {
            body += std::format("  ({})", dimensionName(sdk::game().world().dimension));
        }

        if (settings.value<bool>("includeName", false) && !player.name.empty()) {
            body = player.name + ": " + body;
        }

        return body;
    }

    static const char* dimensionName(int dimension) {
        switch (dimension) {
            case 1: return "Nether";
            case 2: return "The End";
            default: break;
        }
        return "Overworld";
    }

    static bool matches(const Keybind& bind, const KeyEvent& event) {
        if (!bind.bound() || event.key != bind.key) return false;
        return bind.ctrl == event.ctrl && bind.shift == event.shift && bind.alt == event.alt;
    }

    void onKey(KeyEvent& event) {
        if (!event.down || event.repeat) return;

        if (matches(settings.value<Keybind>("copyKey", Keybind{}), event)) {
            pendingCopy_.store(true, std::memory_order_release);
        } else if (matches(settings.value<Keybind>("sendKey", Keybind{}), event)) {
            pendingSend_.store(true, std::memory_order_release);
        }
    }

    void onFrame(FrameEvent&) {
        const bool copy = pendingCopy_.exchange(false, std::memory_order_acq_rel);
        const bool send = pendingSend_.exchange(false, std::memory_order_acq_rel);
        if (!copy && !send) return;

        if (!sdk::game().player().valid) {
            Notifications::warning("Coordinates", "Not in a world.");
            return;
        }

        const std::string text = format();
        const bool notify = settings.value<bool>("notify", true);

        if (copy) {
            if (clipboard::copy(text)) {
                if (notify) Notifications::success("Copied", text);
            } else if (notify) {
                Notifications::error("Copy failed", "Windows refused the clipboard.");
            }
        }

        if (send && !sdk::game().sendChat(text) && notify) {
            Notifications::warning("Coordinates",
                                   "The chat is not reachable on this build of the game.");
        }
    }

    std::atomic<bool> pendingCopy_{false};
    std::atomic<bool> pendingSend_{false};
};

} // namespace

void registerUtilityModules(ModuleManager& manager) {
    manager.add<Fullbright>();
    manager.add<Waypoints>();
    manager.add<ChatMacros>();
    manager.add<CoordinateTools>();
}

} // namespace velyx
