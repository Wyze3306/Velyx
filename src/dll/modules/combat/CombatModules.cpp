#include "CombatModules.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/module/HudModule.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/modules/hud/TextHud.hpp"
#include "dll/sdk/Camera.hpp"
#include "dll/sdk/Entities.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

// Everything that draws over an entity answers the same two questions — who, and in
// what colour. Asking them the same way is what makes four modules feel like one
// feature rather than four takes on it.

void addAudienceSettings(Settings& settings, bool hostileByDefault, float defaultRange,
                         bool withRange = true) {
    settings.header("Who is drawn");
    settings.toggle("showPlayers", "Players", true);
    settings.toggle("showHostile", "Hostile mobs", hostileByDefault);
    settings.toggle("showPassive", "Passive mobs", false);
    settings.toggle("showItems", "Dropped items", false);
    settings.toggle("showProjectiles", "Projectiles", false);
    settings.toggle("showOther", "Anything else", false);

    if (withRange) {
        settings.slider("maxDistance", "Range", defaultRange, 8.f, 256.f,
                        "Past this, nothing is drawn at all.", " m");
    }
}

bool accepts(const Settings& settings, const Actor& actor) {
    if (actor.self) return false;
    if (actor.distance > settings.value<float>("maxDistance", 48.f)) return false;

    switch (actor.kind) {
        case ActorKind::Player:     return settings.value<bool>("showPlayers", true);
        case ActorKind::Hostile:    return settings.value<bool>("showHostile", false);
        case ActorKind::Passive:    return settings.value<bool>("showPassive", false);
        case ActorKind::Item:       return settings.value<bool>("showItems", false);
        case ActorKind::Projectile: return settings.value<bool>("showProjectiles", false);
        case ActorKind::Vehicle:
        case ActorKind::Unknown:    return settings.value<bool>("showOther", false);
    }
    return false;
}

Color kindColour(ActorKind kind, const Theme& active) {
    switch (kind) {
        case ActorKind::Player:     return active.liveAccent();
        case ActorKind::Hostile:    return active.danger;
        case ActorKind::Passive:    return active.success.darken(0.15f);
        case ActorKind::Item:       return active.textMuted;
        case ActorKind::Projectile: return active.warning;
        case ActorKind::Vehicle:
        case ActorKind::Unknown:    break;
    }
    return active.textDim;
}

void addColourSettings(Settings& settings, const char* defaultSource) {
    settings.header("Colour");
    settings.dropdown("colorSource", "Colour", defaultSource,
                      {"By type", "By health", "Theme accent", "Single colour"});
    settings.color("color", "Colour", palette::kMint);
    settings.toggle("fadeWithDistance", "Fade with distance", true,
                    "What is far away draws fainter, so the near target reads first.");

    settings.find("color")->visibleWhen = [&settings] {
        return settings.value<std::string>("colorSource", "By type") == "Single colour";
    };
}

Color colourFor(const Settings& settings, const Actor& actor, const Theme& active) {
    const std::string source = settings.value<std::string>("colorSource", "By type");

    if (source == "Theme accent") return active.liveAccent();
    if (source == "Single colour") return settings.value<Color>("color", palette::kMint);
    if (source == "By health" && actor.living()) {
        return lerp(active.danger, active.success, actor.healthFraction());
    }
    return kindColour(actor.kind, active);
}

// Full strength up to half the range, then down to a quarter at the edge. Anything
// steeper makes the far half of the list look like a fault rather than a choice.
float distanceFade(const Settings& settings, const Actor& actor) {
    if (!settings.value<bool>("fadeWithDistance", true)) return 1.f;

    const float range = settings.value<float>("maxDistance", 48.f);
    return remap(actor.distance, range * 0.5f, range, 1.f, 0.25f);
}

enum class Origin { Feet, Centre, Eye };

Origin originFrom(const Settings& settings) {
    const std::string value = settings.value<std::string>("origin", "Feet");
    if (value == "Centre") return Origin::Centre;
    if (value == "Eye") return Origin::Eye;
    return Origin::Feet;
}

// A signature pack decides what `Actor::position` points at, and packs disagree. The
// box is built from whichever answer is right for the one installed, which is a
// dropdown rather than a guess.
void boxFor(const Actor& actor, Origin origin, float expand, Vec3& minimum, Vec3& maximum) {
    const float half = actor.width * 0.5f + expand;

    float feet = actor.position.y;
    if (origin == Origin::Centre) feet -= actor.height * 0.5f;
    if (origin == Origin::Eye) feet -= actor.height * 0.9f;

    minimum = {actor.position.x - half, feet - expand, actor.position.z - half};
    maximum = {actor.position.x + half, feet + actor.height + expand, actor.position.z + half};
}

// Bit 0 is x, bit 1 is y, bit 2 is z, so an edge is a pair differing in one bit.
constexpr std::array<std::pair<int, int>, 12> kBoxEdges{{
    {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
    {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
}};

bool projectCorners(const Vec3& minimum, const Vec3& maximum, std::array<Vec2, 8>& out) {
    const sdk::Camera& view = sdk::camera();

    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point{(corner & 1) ? maximum.x : minimum.x,
                         (corner & 2) ? maximum.y : minimum.y,
                         (corner & 4) ? maximum.z : minimum.z};
        if (!view.project(point, out[static_cast<size_t>(corner)])) return false;
    }
    return true;
}

void drawBracketCorners(Renderer& renderer, const Rect& box, const Color& colour,
                        float thickness) {
    const float arm = std::min(box.width(), box.height()) * 0.28f;
    if (arm <= 1.f) return;

    const auto bracket = [&](Vec2 corner, float dx, float dy) {
        renderer.line(corner, {corner.x + arm * dx, corner.y}, colour, thickness);
        renderer.line(corner, {corner.x, corner.y + arm * dy}, colour, thickness);
    };

    bracket({box.left, box.top}, 1.f, 1.f);
    bracket({box.right, box.top}, -1.f, 1.f);
    bracket({box.left, box.bottom}, 1.f, -1.f);
    bracket({box.right, box.bottom}, -1.f, -1.f);
}

std::string displayName(const Actor& actor) {
    if (!actor.name.empty()) return actor.name;
    return actorKindLabel(actor.kind);
}

// The box the game hits against, drawn where the game would draw it. Nothing here
// changes what the server sees: it reads positions and puts lines on the screen.
class Hitboxes final : public Module {
public:
    Hitboxes()
        : Module("hitboxes", "Hitboxes", ModuleCategory::Combat,
                 "Draws the box an entity is actually hit against, over players and mobs.") {
        settings.header("Shape");
        settings.dropdown("style", "Style", "Corners",
                          {"Corners", "Box", "3D outline", "Filled", "Feet"});
        settings.slider("thickness", "Line thickness", 1.6f, 0.5f, 6.f, "", "px");
        settings.toggle("outline", "Dark outline", true,
                        "Keeps the lines readable against a bright sky.");
        settings.slider("expand", "Grow the box", 0.f, 0.f, 0.6f,
                        "Widens the drawing only, never the reach.", " m");
        settings.dropdown("origin", "Box origin", "Feet", {"Feet", "Centre", "Eye"},
                          "Where the signature pack puts an entity's position.");

        addAudienceSettings(settings, true, 48.f);
        addColourSettings(settings, "By type");

        settings.toggle("flashOnHit", "Flash when hit", true);
        settings.color("hitColor", "Flash colour", Color::rgb8(255, 255, 255, 235));

        settings.header("Health");
        settings.toggle("healthBar", "Health bar beside the box", true);
        settings.slider("barWidth", "Bar width", 3.f, 1.f, 10.f, "", "px");

        settings.header("Limits");
        settings.intSlider("limit", "Boxes at once", 32, 4, 256,
                           "The nearest are kept; the rest cost nothing.");
        settings.toggle("hideInMenus", "Hide in menus", true);

        // Only ever read when the camera had to be derived. The moment a pack knows
        // the game's own matrix, these two stop mattering and stop being shown.
        settings.header("Projection");
        settings.slider("fovCalibration", "Assumed field of view", 70.f, 30.f, 150.f,
                        "Raise it until the boxes sit on the entities.", "°").advanced = true;
        settings.slider("eyeHeight", "Eye height", 1.62f, 0.f, 2.5f, "", " m").advanced = true;

        const auto derived = [] { return !sdk::camera().exact(); };
        settings.find("fovCalibration")->visibleWhen = derived;
        settings.find("eyeHeight")->visibleWhen = derived;
        settings.find("hitColor")->visibleWhen = [this] {
            return settings.value<bool>("flashOnHit", true);
        };
        settings.find("barWidth")->visibleWhen = [this] {
            return settings.value<bool>("healthBar", true);
        };

        on(&Hitboxes::onRender, EventPriority::High);
        on(&Hitboxes::onActorHurt);
        addKeywords({"hitbox", "esp", "box", "entities", "players", "pvp"});
    }

private:
    void onActorHurt(ActorHurtEvent& event) {
        if (!event.actor) return;
        flashed_ = event.actor->address;
        flash_.set(1.f);
    }

    void onRender(RenderEvent& event) {
        if (event.guiOpen) return;
        if (settings.value<bool>("hideInMenus", true) && sdk::game().inMenu()) return;

        if (!sdk::camera().exact()) {
            sdk::camera().setCalibration(settings.value<float>("fovCalibration", 70.f),
                                         settings.value<float>("eyeHeight", 1.62f));
        }

        flash_.to(0.f);
        flash_.update(event.deltaSeconds);

        if (!sdk::camera().valid() || !sdk::entities().available()) return;

        Renderer& renderer = *event.renderer;
        const Theme& active = theme();

        const std::string style = settings.value<std::string>("style", "Corners");
        const Origin origin = originFrom(settings);
        const float expand = settings.value<float>("expand", 0.f);
        const float thickness = settings.value<float>("thickness", 1.6f);
        const bool outline = settings.value<bool>("outline", true);
        const int limit = settings.value<int>("limit", 32);

        int drawn = 0;

        // The list arrives sorted near to far, which is what makes the limit mean "the
        // ones you care about" rather than "whichever came back first".
        for (const Actor& actor : sdk::entities().list()) {
            if (drawn >= limit) break;
            if (!accepts(settings, actor)) continue;

            Vec3 minimum;
            Vec3 maximum;
            boxFor(actor, origin, expand, minimum, maximum);

            std::array<Vec2, 8> corners{};
            if (!projectCorners(minimum, maximum, corners)) continue;

            Rect box{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
            for (const Vec2& corner : corners) {
                box.left = std::min(box.left, corner.x);
                box.top = std::min(box.top, corner.y);
                box.right = std::max(box.right, corner.x);
                box.bottom = std::max(box.bottom, corner.y);
            }

            if (box.width() < 1.f || box.height() < 1.f) continue;
            if (!box.intersects(Rect::fromSize(0.f, 0.f, event.screenSize.x, event.screenSize.y))) {
                continue;
            }

            ++drawn;

            Color colour = colourFor(settings, actor, active).fade(distanceFade(settings, actor));

            if (settings.value<bool>("flashOnHit", true) && actor.address == flashed_ &&
                flash_.value > 0.01f) {
                colour = lerp(colour, settings.value<Color>("hitColor", palette::kSnow),
                              flash_.value);
            }

            const Color shadow = Color{0.f, 0.f, 0.f, colour.a * 0.7f};

            if (style == "Feet") {
                const float radius = std::max(3.f, box.width() * 0.45f);
                const Vec2 feet{box.center().x, box.bottom};
                if (outline) renderer.strokeCircle(feet, radius, shadow, thickness + 2.f);
                renderer.strokeCircle(feet, radius, colour, thickness);
            } else if (style == "3D outline") {
                for (const auto& [from, to] : kBoxEdges) {
                    const Vec2 a = corners[static_cast<size_t>(from)];
                    const Vec2 b = corners[static_cast<size_t>(to)];
                    if (outline) renderer.line(a, b, shadow, thickness + 2.f);
                    renderer.line(a, b, colour, thickness);
                }
            } else if (style == "Corners") {
                if (outline) drawBracketCorners(renderer, box, shadow, thickness + 2.f);
                drawBracketCorners(renderer, box, colour, thickness);
            } else {
                if (style == "Filled") renderer.fillRect(box, colour.fade(0.18f));
                if (outline) renderer.strokeRect(box.inflated(1.f), shadow, thickness + 1.f);
                renderer.strokeRect(box, colour, thickness);
            }

            if (settings.value<bool>("healthBar", true) && actor.living()) {
                drawHealthBar(renderer, box, actor, active, colour.a);
            }
        }
    }

    void drawHealthBar(Renderer& renderer, const Rect& box, const Actor& actor,
                       const Theme& active, float alpha) {
        const float width = settings.value<float>("barWidth", 3.f);
        const Rect track{box.left - 4.f - width, box.top, box.left - 4.f, box.bottom};
        if (track.height() < 4.f) return;

        renderer.fillRounded(track, Color{0.f, 0.f, 0.f, 0.55f * alpha}, width * 0.5f);

        const float fraction = actor.healthFraction();
        const Rect fill{track.left, lerp(track.bottom, track.top, fraction), track.right,
                        track.bottom};

        renderer.fillRounded(fill, lerp(active.danger, active.success, fraction).fade(alpha),
                             width * 0.5f);
    }

    uintptr_t flashed_ = 0;
    Animated flash_{0.f, 6.f};
};

// A name over a head is only useful if it stays readable at forty blocks and does not
// bury the fight at four. Both come down to how it scales, which is a setting.
class Nametags final : public Module {
public:
    Nametags()
        : Module("nametags", "Nametags", ModuleCategory::Combat,
                 "Names, health and distance above the entities you choose.") {
        settings.header("Content");
        settings.toggle("showHealth", "Health", true);
        settings.toggle("showDistance", "Distance", true);
        settings.toggle("healthBar", "Health bar under the name", true);
        settings.toggle("showKind", "What it is", false);

        settings.header("Size");
        settings.slider("size", "Text size", 13.f, 8.f, 28.f, "", "px");
        settings.toggle("scaleWithDistance", "Shrink with distance", true);
        settings.slider("minimumScale", "Smallest", 0.55f, 0.2f, 1.f, "", "x");
        settings.slider("lift", "Height above the head", 6.f, 0.f, 40.f, "", "px");

        settings.header("Background");
        settings.toggle("background", "Background", true);
        settings.color("backgroundColor", "Background colour", Color::rgb8(8, 10, 12, 175));
        settings.toggle("shadow", "Text shadow", true);

        addAudienceSettings(settings, false, 64.f);
        addColourSettings(settings, "By type");

        settings.header("Limits");
        settings.intSlider("limit", "Tags at once", 24, 4, 128);
        settings.toggle("hideInMenus", "Hide in menus", true);

        settings.find("minimumScale")->visibleWhen = [this] {
            return settings.value<bool>("scaleWithDistance", true);
        };
        settings.find("backgroundColor")->visibleWhen = [this] {
            return settings.value<bool>("background", true);
        };

        on(&Nametags::onRender, EventPriority::High);
        addKeywords({"nametag", "name", "tag", "esp", "health", "pvp"});
    }

private:
    void onRender(RenderEvent& event) {
        if (event.guiOpen) return;
        if (settings.value<bool>("hideInMenus", true) && sdk::game().inMenu()) return;
        if (!sdk::camera().valid() || !sdk::entities().available()) return;

        Renderer& renderer = *event.renderer;
        const Theme& active = theme();
        const auto limit = static_cast<size_t>(settings.value<int>("limit", 24));

        // Chosen near to far so the limit keeps the ones that matter, then drawn far to
        // near so a close tag lands on top of the ones behind it rather than under them.
        chosen_.clear();
        for (const Actor& actor : sdk::entities().list()) {
            if (chosen_.size() >= limit) break;
            if (!accepts(settings, actor)) continue;
            chosen_.push_back(&actor);
        }

        for (auto it = chosen_.rbegin(); it != chosen_.rend(); ++it) {
            Vec2 anchor;
            if (!sdk::camera().project((*it)->head(), anchor)) continue;
            draw(renderer, active, **it, anchor);
        }
    }

    void draw(Renderer& renderer, const Theme& active, const Actor& actor, Vec2 anchor) {
        const float alpha = distanceFade(settings, actor);

        float scale = 1.f;
        if (settings.value<bool>("scaleWithDistance", true)) {
            scale = clamp(12.f / std::max(1.f, actor.distance), 
                          settings.value<float>("minimumScale", 0.55f), 1.f);
        }

        FontSpec spec;
        spec.family = active.fontFamily;
        spec.size = settings.value<float>("size", 13.f) * scale * active.fontScale;
        spec.weight = FontWeight::SemiBold;
        spec.align = TextAlign::Center;
        spec.valign = TextVAlign::Middle;

        std::string label = displayName(actor);
        if (settings.value<bool>("showKind", false) && !actor.name.empty()) {
            label += std::format("  [{}]", actorKindLabel(actor.kind));
        }
        if (settings.value<bool>("showHealth", true) && actor.living()) {
            label += std::format("  {}", static_cast<int>(std::ceil(actor.health)));
        }
        if (settings.value<bool>("showDistance", true)) {
            label += std::format("  {}m", static_cast<int>(actor.distance));
        }

        const Vec2 size = renderer.measure(label, spec);
        const float padding = 5.f * scale;
        const float lift = settings.value<float>("lift", 6.f);

        const Rect pill = Rect::fromSize(anchor.x - size.x * 0.5f - padding,
                                         anchor.y - size.y - padding * 2.f - lift,
                                         size.x + padding * 2.f, size.y + padding * 2.f);

        if (settings.value<bool>("background", true)) {
            renderer.fillRounded(
                pill, settings.value<Color>("backgroundColor", Color::rgb8(8, 10, 12, 175))
                          .fade(alpha),
                4.f * scale);
        }

        const Color colour = colourFor(settings, actor, active).fade(alpha);

        if (settings.value<bool>("shadow", true)) {
            renderer.textShadowed(label, pill, colour, spec);
        } else {
            renderer.text(label, pill, colour, spec);
        }

        if (!settings.value<bool>("healthBar", true) || !actor.living()) return;

        const float height = std::max(2.f, 3.f * scale);
        const Rect track{pill.left, pill.bottom + 2.f, pill.right, pill.bottom + 2.f + height};

        renderer.fillRounded(track, Color{0.f, 0.f, 0.f, 0.55f * alpha}, height * 0.5f);

        const float fraction = actor.healthFraction();
        renderer.fillRounded(
            Rect{track.left, track.top, lerp(track.left, track.right, fraction), track.bottom},
            lerp(active.danger, active.success, fraction).fade(alpha), height * 0.5f);
    }

    // Rebuilt every frame, kept as a member so the drawing does not allocate once per
    // frame for a list whose size barely changes.
    std::vector<const Actor*> chosen_;
};

// A line from somewhere fixed to where something is. Cheap, and the one overlay that
// still tells you where a target went once it left the screen edge.
class Tracers final : public Module {
public:
    Tracers()
        : Module("tracers", "Tracers", ModuleCategory::Combat,
                 "A line to each target, from the edge of the screen or from the crosshair.") {
        settings.header("Line");
        settings.dropdown("from", "Starts at", "Bottom of the screen",
                          {"Bottom of the screen", "Crosshair", "Top of the screen"});
        settings.dropdown("to", "Ends at", "Feet", {"Feet", "Centre", "Head"});
        settings.slider("thickness", "Thickness", 1.4f, 0.5f, 5.f, "", "px");
        settings.toggle("outline", "Dark outline", true);

        addAudienceSettings(settings, false, 96.f);
        addColourSettings(settings, "By type");

        settings.header("Limits");
        settings.intSlider("limit", "Lines at once", 16, 2, 64);
        settings.toggle("hideInMenus", "Hide in menus", true);

        on(&Tracers::onRender, EventPriority::High);
        addKeywords({"tracer", "line", "esp", "target", "pvp"});
    }

private:
    void onRender(RenderEvent& event) {
        if (event.guiOpen) return;
        if (settings.value<bool>("hideInMenus", true) && sdk::game().inMenu()) return;
        if (!sdk::camera().valid() || !sdk::entities().available()) return;

        Renderer& renderer = *event.renderer;
        const Theme& active = theme();

        const std::string from = settings.value<std::string>("from", "Bottom of the screen");
        const Vec2 start = from == "Crosshair"
                               ? Vec2{event.screenSize.x * 0.5f, event.screenSize.y * 0.5f}
                               : Vec2{event.screenSize.x * 0.5f,
                                      from == "Top of the screen" ? 0.f : event.screenSize.y};

        const std::string to = settings.value<std::string>("to", "Feet");
        const float thickness = settings.value<float>("thickness", 1.4f);
        const bool outline = settings.value<bool>("outline", true);
        const int limit = settings.value<int>("limit", 16);

        int drawn = 0;

        for (const Actor& actor : sdk::entities().list()) {
            if (drawn >= limit) break;
            if (!accepts(settings, actor)) continue;

            const Vec3 target = to == "Head" ? actor.head()
                                             : (to == "Centre" ? actor.centre() : actor.position);

            Vec2 end;
            if (!sdk::camera().project(target, end)) continue;

            ++drawn;

            const Color colour = colourFor(settings, actor, active).fade(distanceFade(settings, actor));
            if (outline) {
                renderer.line(start, end, Color{0.f, 0.f, 0.f, colour.a * 0.6f}, thickness + 1.6f);
            }
            renderer.line(start, end, colour, thickness);
        }
    }
};

// Who you are fighting, in one card, with the bar that trails behind the real health
// so a burst of damage is something you see rather than something you missed.
class TargetHud final : public HudModule {
public:
    TargetHud()
        : HudModule("target_hud", "Target", "The player you are fighting, and their health.",
                    {0.5f, 0.22f}, HudAnchor::Center) {
        settings.header("Target");
        settings.dropdown("pick", "Chosen by", "Last hit, then aim",
                          {"Last hit, then aim", "Last hit only", "What you aim at",
                           "Nearest player"});
        settings.slider("hold", "Kept on screen for", 3.f, 0.5f, 15.f,
                        "After the last hit or the last time you looked at them.", " s");
        settings.slider("cone", "Aim tolerance", 12.f, 2.f, 45.f, "", "°");
        settings.slider("range", "Range", 24.f, 4.f, 128.f, "", " m");
        settings.toggle("playersOnly", "Players only", true);

        settings.header("Card");
        settings.slider("cardWidth", "Width", 190.f, 120.f, 400.f, "", "px");
        settings.toggle("showDistance", "Distance", true);
        settings.toggle("showKind", "What it is", false);
        settings.toggle("trailingBar", "Trailing damage bar", true);
        settings.slider("trailSpeed", "Trail speed", 2.5f, 0.5f, 12.f);

        settings.find("cone")->visibleWhen = [this] {
            const std::string pick = settings.value<std::string>("pick", "Last hit, then aim");
            return pick == "Last hit, then aim" || pick == "What you aim at";
        };
        settings.find("trailSpeed")->visibleWhen = [this] {
            return settings.value<bool>("trailingBar", true);
        };

        on(&TargetHud::onFrame);
        on(&TargetHud::onAttack);
        on(&TargetHud::onActorHurt);
        addKeywords({"target", "cible", "health", "combat", "pvp"});
    }

    bool relevantNow() const override { return hasTarget_; }

    Vec2 contentSize(Renderer& renderer) override {
        if (!hasTarget_) return {};

        const FontSpec name = fontFor(1.f, FontWeight::SemiBold);
        return {settings.value<float>("cardWidth", 190.f) * scale(), name.size * 3.4f};
    }

    void drawContent(Renderer& renderer, const Rect& content) override {
        if (!hasTarget_) return;

        const Theme& active = theme();
        const Actor& actor = resolved_;

        const FontSpec name = fontFor(1.f, FontWeight::SemiBold);
        FontSpec detail = fontFor(0.82f, FontWeight::Medium);
        detail.align = TextAlign::Right;

        const Rect header{content.left, content.top, content.right, content.top + name.size * 1.4f};

        renderer.text(displayName(actor), header, textColor(), name);

        std::string right;
        if (settings.value<bool>("showKind", false)) right += actorKindLabel(actor.kind);
        if (settings.value<bool>("showDistance", true)) {
            if (!right.empty()) right += "  ·  ";
            right += std::format("{:.1f} m", actor.distance);
        }
        if (!right.empty()) renderer.text(right, header, textColor().fade(0.65f), detail);

        const float fraction = actor.living() ? actor.healthFraction() : 1.f;

        trail_.speed = settings.value<float>("trailSpeed", 2.5f);
        if (fraction > trail_.value) trail_.set(fraction);
        trail_.to(fraction);
        trail_.update(renderer.delta());

        const Rect track{content.left, header.bottom + 4.f, content.right,
                         header.bottom + 4.f + name.size * 0.55f};
        const float radius = track.height() * 0.5f;

        renderer.fillRounded(track, Color{0.f, 0.f, 0.f, 0.45f}, radius);

        if (settings.value<bool>("trailingBar", true) && trail_.value > fraction) {
            renderer.fillRounded(
                Rect{track.left, track.top, lerp(track.left, track.right, trail_.value),
                     track.bottom},
                active.warning.fade(0.55f), radius);
        }

        renderer.fillRounded(
            Rect{track.left, track.top, lerp(track.left, track.right, fraction), track.bottom},
            lerp(active.danger, active.success, fraction), radius);

        if (!actor.living()) return;

        FontSpec value = fontFor(0.78f, FontWeight::Bold);
        value.align = TextAlign::Center;
        value.valign = TextVAlign::Middle;

        renderer.textShadowed(std::format("{} / {}", static_cast<int>(std::ceil(actor.health)),
                                          static_cast<int>(std::ceil(actor.maxHealth))),
                              track, palette::kSnow, value);
    }

private:
    // On the frame rather than in contentSize(): the element is only measured while it
    // is on screen, and something that decides whether it should be must run whether it
    // is or not.
    void onFrame(FrameEvent& event) { resolve(event.deltaSeconds); }

    void onAttack(AttackEvent& event) { remember(event.target); }
    void onActorHurt(ActorHurtEvent& event) { remember(event.actor); }

    void remember(const Actor* actor) {
        if (!actor) return;
        remembered_ = actor->address;
        sinceHit_ = 0.f;
    }

    // Resolved against this frame's snapshot every time: the address is an identity,
    // and the health beside it has to be the one the game holds now. What is kept is a
    // copy — the snapshot is rebuilt under us between frames, and a card that outlives
    // its entity by one frame must not be the way that is discovered.
    void resolve(float deltaSeconds) {
        hasTarget_ = false;
        sinceHit_ += deltaSeconds;

        if (!sdk::entities().available()) return;

        const std::string pick = settings.value<std::string>("pick", "Last hit, then aim");
        const float hold = settings.value<float>("hold", 3.f);
        const float range = settings.value<float>("range", 24.f);

        if (pick != "What you aim at" && pick != "Nearest player" && sinceHit_ <= hold) {
            if (const Actor* hit = sdk::entities().find(remembered_)) {
                resolved_ = *hit;
                hasTarget_ = true;
                return;
            }
        }

        if (pick == "Last hit only") return;

        const Actor* candidate =
            pick == "Nearest player"
                ? sdk::entities().nearestPlayer(range)
                : sdk::entities().underCrosshair(range, settings.value<float>("cone", 12.f));

        if (!candidate) return;
        if (settings.value<bool>("playersOnly", true) && !candidate->isPlayer()) return;

        resolved_ = *candidate;
        hasTarget_ = true;
        remembered_ = candidate->address;
        sinceHit_ = 0.f;
    }

    Actor resolved_;
    bool hasTarget_ = false;
    uintptr_t remembered_ = 0;
    float sinceHit_ = 1e6f;
    Animated trail_{1.f, 2.5f};
};

// A radar is the one overlay that tells you about what you cannot see, which in a
// fight is most of it. It turns with you, so a dot on the left is on your left.
class CombatRadar final : public HudModule {
public:
    CombatRadar()
        : HudModule("radar", "Radar", "Nearby entities, on a dial that turns with you.",
                    {0.985f, 0.42f}, HudAnchor::MiddleRight) {
        settings.header("Dial");
        settings.slider("size", "Diameter", 130.f, 60.f, 320.f, "", "px");
        settings.slider("range", "Range", 48.f, 8.f, 256.f, "", " m");
        settings.toggle("rings", "Distance rings", true);
        settings.toggle("cone", "Field of view cone", true);
        settings.toggle("cardinals", "North marker", true);
        settings.toggle("rotate", "Turn with the camera", true,
                        "Off keeps north up, like a map.");

        settings.header("Dots");
        settings.slider("dotSize", "Dot size", 3.2f, 1.f, 10.f, "", "px");
        settings.toggle("heightTint", "Dim what is below you", true);
        settings.toggle("ringEdge", "Pin what is out of range to the edge", false);

        addAudienceSettings(settings, true, 48.f, false);
        addColourSettings(settings, "By type");

        settings.set("background", SettingValue{false});
        settings.set("padding", SettingValue{0.f});

        addKeywords({"radar", "minimap", "entities", "pvp", "combat"});
    }

    bool relevantNow() const override {
        return sdk::game().player().valid && !sdk::game().inMenu();
    }

    Vec2 contentSize(Renderer& renderer) override {
        const float size = settings.value<float>("size", 130.f) * scale();
        return {size, size};
    }

    void drawContent(Renderer& renderer, const Rect& content) override {
        const Theme& active = theme();
        const Vec2 centre = content.center();
        const float radius = std::min(content.width(), content.height()) * 0.5f;

        renderer.fillCircle(centre, radius, active.backgroundDeep.withAlpha(0.55f));
        renderer.strokeCircle(centre, radius, active.border.fade(0.9f), active.borderWidth);

        if (settings.value<bool>("rings", true)) {
            renderer.strokeCircle(centre, radius * 0.66f, active.border.fade(0.5f), 1.f);
            renderer.strokeCircle(centre, radius * 0.33f, active.border.fade(0.35f), 1.f);
        }

        // The dial is drawn in the camera's frame. Holding north up is the same frame
        // with the camera pinned looking south, which is what 180 means here.
        const bool rotate = settings.value<bool>("rotate", true);
        const float yaw = rotate ? sdk::game().player().yaw : 180.f;
        const float sin = std::sin(toRadians(yaw));
        const float cos = std::cos(toRadians(yaw));

        if (settings.value<bool>("cone", true)) {
            drawCone(renderer, active, centre, radius, sin, cos);
        }

        if (settings.value<bool>("cardinals", true)) {
            // North is -z; on a dial that turns with you it has to turn with it.
            const Vec3 north{0.f, 0.f, -1.f};
            const Vec2 offset = project(north, sin, cos);

            FontSpec spec = fontFor(0.7f, FontWeight::Bold);
            spec.align = TextAlign::Center;
            spec.valign = TextVAlign::Middle;

            const Vec2 marker{centre.x + offset.x * radius * 0.86f,
                              centre.y + offset.y * radius * 0.86f};
            renderer.text("N", Rect::fromSize(marker.x - 8.f, marker.y - 8.f, 16.f, 16.f),
                          active.textMuted, spec);
        }

        renderer.fillCircle(centre, 2.4f * scale(), active.text.fade(0.85f));

        if (!sdk::entities().available()) return;

        const float range = std::max(1.f, settings.value<float>("range", 48.f));
        const float dot = settings.value<float>("dotSize", 3.2f) * scale();
        const bool pin = settings.value<bool>("ringEdge", false);
        const bool heightTint = settings.value<bool>("heightTint", true);
        const float eye = sdk::game().player().position.y;

        for (const Actor& actor : sdk::entities().list()) {
            if (actor.self) continue;
            if (!kindShown(actor.kind)) continue;

            const float flat = actor.position.flatDistanceTo(sdk::game().player().position);
            if (flat > range && !pin) continue;

            const Vec3 delta = actor.position - sdk::game().player().position;
            const Vec2 direction = project(delta, sin, cos);

            const float reach = std::min(flat / range, 1.f) * (radius - dot - 1.f);
            const Vec2 point{centre.x + direction.x * reach, centre.y + direction.y * reach};

            Color colour = colourFor(settings, actor, active);
            if (heightTint) {
                colour = colour.fade(remap(actor.position.y - eye, -12.f, 0.f, 0.45f, 1.f));
            }

            renderer.fillCircle(point, dot + 1.f, Color{0.f, 0.f, 0.f, colour.a * 0.6f});
            renderer.fillCircle(point, dot, colour);
        }
    }

private:
    // The radar has its own range, so the shared audience filter's own distance test
    // would be a second, disagreeing one. Only the checkboxes are borrowed.
    [[nodiscard]] bool kindShown(ActorKind kind) const {
        switch (kind) {
            case ActorKind::Player:     return settings.value<bool>("showPlayers", true);
            case ActorKind::Hostile:    return settings.value<bool>("showHostile", true);
            case ActorKind::Passive:    return settings.value<bool>("showPassive", false);
            case ActorKind::Item:       return settings.value<bool>("showItems", false);
            case ActorKind::Projectile: return settings.value<bool>("showProjectiles", false);
            case ActorKind::Vehicle:
            case ActorKind::Unknown:    return settings.value<bool>("showOther", false);
        }
        return false;
    }

    // World delta to a unit direction on the dial: forward is up, right is right.
    static Vec2 project(const Vec3& delta, float sin, float cos) {
        const float forward = -delta.x * sin + delta.z * cos;
        const float right = -delta.x * cos - delta.z * sin;

        const float length = std::sqrt(forward * forward + right * right);
        if (length < 1e-4f) return {};
        return {right / length, -forward / length};
    }

    // Where the wedge points is taken from the same projection the dots go through,
    // rather than worked out again: two derivations of "up" is one too many.
    void drawCone(Renderer& renderer, const Theme& active, Vec2 centre, float radius, float sin,
                  float cos) {
        const float playerYaw = toRadians(sdk::game().player().yaw);
        const Vec3 look{-std::sin(playerYaw), 0.f, std::cos(playerYaw)};

        const Vec2 heading = project(look, sin, cos);
        if (heading.x == 0.f && heading.y == 0.f) return;

        const float facing = toDegrees(std::atan2(heading.y, heading.x));
        const float half = sdk::camera().fieldOfView() * 0.5f;

        std::vector<Vec2> wedge{centre};
        for (float step = -half; step <= half; step += 5.f) {
            const float angle = toRadians(facing + step);
            wedge.push_back({centre.x + std::cos(angle) * radius,
                             centre.y + std::sin(angle) * radius});
        }

        renderer.fillPolygon(wedge, active.liveAccent().fade(0.1f));
    }
};

// The confirmation that a hit landed, at the crosshair, where you were already
// looking. It is the cheapest module here and the one you notice missing.
class HitMarker final : public Module {
public:
    HitMarker()
        : Module("hit_marker", "Hit marker", ModuleCategory::Combat,
                 "Marks the crosshair the moment a hit registers.") {
        settings.header("Mark");
        settings.dropdown("style", "Style", "Cross", {"Cross", "Ring", "Dot", "Corners"});
        settings.slider("size", "Size", 9.f, 3.f, 30.f, "", "px");
        settings.slider("thickness", "Thickness", 2.f, 1.f, 6.f, "", "px");
        settings.slider("duration", "Fades over", 0.35f, 0.08f, 1.5f, "", " s");
        settings.toggle("expand", "Expand as it fades", true);

        settings.header("Colours");
        settings.color("color", "Colour", Color::rgb8(255, 255, 255, 235));
        settings.toggle("markKills", "A different mark on a kill", true);
        settings.color("killColor", "Kill colour", palette::kEmber);

        settings.header("Counter");
        settings.toggle("showCombo", "Show the hit streak", false);
        settings.slider("comboWindow", "Streak resets after", 2.5f, 0.5f, 10.f, "", " s");

        settings.find("killColor")->visibleWhen = [this] {
            return settings.value<bool>("markKills", true);
        };
        settings.find("comboWindow")->visibleWhen = [this] {
            return settings.value<bool>("showCombo", false);
        };

        on(&HitMarker::onActorHurt);
        on(&HitMarker::onRender, EventPriority::Low);
        addKeywords({"hit marker", "hitmarker", "combo", "pvp", "combat"});
    }

    void onEnable() override {
        life_.set(0.f);
        combo_ = 0;
    }

private:
    void onActorHurt(ActorHurtEvent& event) {
        life_.set(1.f);
        kill_ = event.actor != nullptr && event.actor->living() && event.actor->health <= 0.f;

        if (sinceHit_ > settings.value<float>("comboWindow", 2.5f)) combo_ = 0;
        ++combo_;
        sinceHit_ = 0.f;
    }

    void onRender(RenderEvent& event) {
        sinceHit_ += event.deltaSeconds;

        life_.speed = 4.f / std::max(0.05f, settings.value<float>("duration", 0.35f));
        life_.to(0.f);
        life_.update(event.deltaSeconds);

        if (life_.value <= 0.01f) return;
        if (event.guiOpen || sdk::game().inMenu()) return;

        Renderer& renderer = *event.renderer;
        const Vec2 centre{event.screenSize.x * 0.5f, event.screenSize.y * 0.5f};

        const float grow = settings.value<bool>("expand", true) ? 1.f + (1.f - life_.value) * 0.6f
                                                                : 1.f;
        const float size = settings.value<float>("size", 9.f) * grow;
        const float thickness = settings.value<float>("thickness", 2.f);

        const Color base = kill_ && settings.value<bool>("markKills", true)
                               ? settings.value<Color>("killColor", palette::kEmber)
                               : settings.value<Color>("color", palette::kSnow);
        const Color colour = base.fade(life_.value);
        const Color shadow = Color{0.f, 0.f, 0.f, colour.a * 0.55f};

        const std::string style = settings.value<std::string>("style", "Cross");

        if (style == "Dot") {
            renderer.fillCircle(centre, size * 0.35f + 1.f, shadow);
            renderer.fillCircle(centre, size * 0.35f, colour);
        } else if (style == "Ring") {
            renderer.strokeCircle(centre, size, shadow, thickness + 2.f);
            renderer.strokeCircle(centre, size, colour, thickness);
        } else if (style == "Corners") {
            const Rect box{centre.x - size, centre.y - size, centre.x + size, centre.y + size};
            drawBracketCorners(renderer, box, shadow, thickness + 2.f);
            drawBracketCorners(renderer, box, colour, thickness);
        } else {
            const float inner = size * 0.35f;
            const auto stroke = [&](float dx, float dy, const Color& tint, float width) {
                renderer.line({centre.x + inner * dx, centre.y + inner * dy},
                              {centre.x + size * dx, centre.y + size * dy}, tint, width);
            };
            for (const auto& [dx, dy] : {std::pair{-1.f, -1.f}, std::pair{1.f, -1.f},
                                         std::pair{-1.f, 1.f}, std::pair{1.f, 1.f}}) {
                stroke(dx, dy, shadow, thickness + 2.f);
                stroke(dx, dy, colour, thickness);
            }
        }

        if (!settings.value<bool>("showCombo", false) || combo_ < 2) return;

        const Theme& active = theme();
        FontSpec spec;
        spec.family = active.fontFamily;
        spec.size = 15.f * active.fontScale;
        spec.weight = FontWeight::Bold;
        spec.align = TextAlign::Center;
        spec.valign = TextVAlign::Middle;

        renderer.textShadowed(std::format("x{}", combo_),
                              Rect::fromSize(centre.x - 40.f, centre.y + size + 8.f, 80.f, 20.f),
                              active.liveAccent().fade(life_.value), spec);
    }

    Animated life_{0.f, 12.f};
    float sinceHit_ = 1e6f;
    int combo_ = 0;
    bool kill_ = false;
};

// How far away the thing you hit was. Not an advantage — the game decides what lands.
// It is the only way to find out that your reach on this server is 2.9 and not 3.
class ReachDisplay final : public TextHud {
public:
    ReachDisplay()
        : TextHud("reach", "Reach", "The distance of your hits, measured as the game does.",
                  {0.5f, 0.62f}, HudAnchor::Center) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showLast", "Last hit", true);
        settings.toggle("showAverage", "Average", true);
        settings.toggle("showBest", "Longest", false);
        settings.slider("hold", "Kept on screen for", 4.f, 0.5f, 30.f,
                        "After the last hit. Zero keeps it up.", " s");
        settings.button("reset", "Reset the numbers", [this] {
            samples_ = 0;
            total_ = 0.f;
            best_ = 0.f;
            last_ = 0.f;
        });

        on(&ReachDisplay::onAttack);
        on(&ReachDisplay::onFrame);
        addKeywords({"reach", "distance", "hit", "pvp", "combat"});
    }

    bool relevantNow() const override {
        const float hold = settings.value<float>("hold", 4.f);
        return samples_ > 0 && (hold <= 0.f || since_ <= hold);
    }

    std::vector<Row> rows() override {
        std::vector<Row> result;
        if (samples_ == 0) return result;

        if (settings.value<bool>("showLast", true)) {
            result.push_back(Row{"Reach", std::format("{:.2f} m", last_), {}});
        }
        if (settings.value<bool>("showAverage", true)) {
            result.push_back(
                Row{"Avg", std::format("{:.2f} m", total_ / static_cast<float>(samples_)), {}});
        }
        if (settings.value<bool>("showBest", false)) {
            result.push_back(Row{"Best", std::format("{:.2f} m", best_), {}});
        }

        return result;
    }

private:
    void onFrame(FrameEvent& event) { since_ += event.deltaSeconds; }

    void onAttack(AttackEvent& event) {
        if (!event.target) return;

        // The game measures to the nearest point of the box, not to its centre, which
        // is why a hit on the shoulder reads shorter than one straight on.
        const Vec3 eye = sdk::camera().origin();
        const Vec3 minimum = event.target->minimum();
        const Vec3 maximum = event.target->maximum();

        const Vec3 nearest{clamp(eye.x, minimum.x, maximum.x), clamp(eye.y, minimum.y, maximum.y),
                           clamp(eye.z, minimum.z, maximum.z)};

        last_ = eye.distanceTo(nearest);
        best_ = std::max(best_, last_);
        total_ += last_;
        ++samples_;
        since_ = 0.f;
    }

    float last_ = 0.f;
    float best_ = 0.f;
    float total_ = 0.f;
    int samples_ = 0;
    float since_ = 1e6f;
};

// Health is a number in a corner until it matters, and then it is the only thing that
// does. This puts it where the eye already is: around the edge of the screen.
class LowHealthAlert final : public Module {
public:
    LowHealthAlert()
        : Module("low_health", "Low health alert", ModuleCategory::Combat,
                 "Pulses the edge of the screen when your health drops.") {
        settings.header("Trigger");
        settings.slider("threshold", "Below", 7.f, 1.f, 20.f, "", " hp");
        settings.toggle("useFraction", "As a share of the maximum", false);
        settings.slider("fraction", "Below", 0.35f, 0.05f, 0.9f, "", "x");
        settings.toggle("notify", "Also send a notification", false);

        settings.header("Look");
        settings.color("color", "Colour", Color::rgb8(232, 96, 78, 200));
        settings.slider("width", "Edge width", 90.f, 20.f, 320.f, "", "px");
        settings.slider("pulseSpeed", "Pulse speed", 2.2f, 0.4f, 6.f, "", " Hz");
        settings.slider("intensity", "Strength", 0.75f, 0.1f, 1.f);
        settings.toggle("scaleWithDanger", "Stronger the lower it gets", true);

        settings.find("threshold")->visibleWhen = [this] {
            return !settings.value<bool>("useFraction", false);
        };
        settings.find("fraction")->visibleWhen = [this] {
            return settings.value<bool>("useFraction", false);
        };

        on(&LowHealthAlert::onRender, EventPriority::Low);
        addKeywords({"health", "low", "alert", "warning", "pvp"});
    }

    void onEnable() override { warned_ = false; }

private:
    void onRender(RenderEvent& event) {
        const sdk::PlayerState& player = sdk::game().player();

        pulse_ += event.deltaSeconds * settings.value<float>("pulseSpeed", 2.2f);

        float danger = 0.f;
        if (player.valid && player.health > 0.f) {
            if (settings.value<bool>("useFraction", false)) {
                const float limit = settings.value<float>("fraction", 0.35f) *
                                    std::max(1.f, player.maxHealth);
                danger = remap(player.health, limit, limit * 0.25f, 0.f, 1.f);
            } else {
                const float limit = settings.value<float>("threshold", 7.f);
                danger = remap(player.health, limit, 1.f, 0.f, 1.f);
            }
        }

        if (danger > 0.05f && !warned_) {
            warned_ = true;
            if (settings.value<bool>("notify", false)) {
                Notifications::warning("Low health",
                                       std::format("{} hp left",
                                                   static_cast<int>(std::ceil(player.health))));
            }
        } else if (danger <= 0.01f) {
            warned_ = false;
        }

        level_.speed = 6.f;
        level_.to(danger);
        level_.update(event.deltaSeconds);

        if (level_.value <= 0.01f || event.guiOpen) return;

        const float wave = 0.55f + 0.45f * std::sin(pulse_ * kPi);
        float strength = level_.value * settings.value<float>("intensity", 0.75f) * wave;
        if (!settings.value<bool>("scaleWithDanger", true)) {
            strength = settings.value<float>("intensity", 0.75f) * wave;
        }

        const Color colour =
            settings.value<Color>("color", palette::kEmber).fade(clamp(strength, 0.f, 1.f));
        const Color clear = colour.withAlpha(0.f);

        Renderer& renderer = *event.renderer;
        const float width = settings.value<float>("width", 90.f);
        const Vec2 screen = event.screenSize;

        renderer.fillGradient(Rect{0.f, 0.f, width, screen.y}, colour, clear, 0.f);
        renderer.fillGradient(Rect{screen.x - width, 0.f, screen.x, screen.y}, clear, colour, 0.f);
        renderer.fillGradient(Rect{0.f, 0.f, screen.x, width}, colour, clear, 90.f);
        renderer.fillGradient(Rect{0.f, screen.y - width, screen.x, screen.y}, clear, colour, 90.f);
    }

    Animated level_{0.f, 6.f};
    float pulse_ = 0.f;
    bool warned_ = false;
};

} // namespace

void registerCombatModules(ModuleManager& manager) {
    manager.add<Hitboxes>();
    manager.add<Nametags>();
    manager.add<Tracers>();
    manager.add<TargetHud>();
    manager.add<CombatRadar>();
    manager.add<HitMarker>();
    manager.add<ReachDisplay>();
    manager.add<LowHealthAlert>();
}

} // namespace velyx
