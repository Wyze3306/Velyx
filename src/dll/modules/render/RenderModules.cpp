#include "RenderModules.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "dll/Velyx.hpp"
#include "dll/module/HudModule.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

class Crosshair final : public HudModule {
public:
    Crosshair()
        : HudModule("crosshair", "Viseur", "Viseur personnalisé, forme, couleur et écartement.",
                    {0.5f, 0.5f}, HudAnchor::Center) {
        settings.set("background", SettingValue{false});
        settings.set("shadow", SettingValue{false});
        settings.set("padding", SettingValue{0.f});

        settings.header("Forme");
        settings.dropdown("style", "Style", "Croix",
                          {"Croix", "Croix + point", "Point", "Cercle", "T inversé", "Crochets"});
        settings.slider("length", "Longueur des branches", 7.f, 1.f, 30.f, "", "px");
        settings.slider("gap", "Écart central", 4.f, 0.f, 24.f, "", "px");
        settings.slider("thickness", "Épaisseur", 2.f, 1.f, 8.f, "", "px");
        settings.slider("dotSize", "Taille du point", 2.f, 0.5f, 10.f, "", "px");

        settings.header("Couleurs");
        settings.toggle("useThemeAccent", "Couleur du thème", false);
        settings.color("color", "Couleur", Color::rgb8(255, 255, 255, 230));
        settings.toggle("outline", "Contour", true);
        settings.color("outlineColor", "Couleur du contour", Color::rgb8(0, 0, 0, 160));
        settings.toggle("hitFlash", "Éclair sur touche", true);
        settings.color("hitColor", "Couleur de touche", palette::kMint);
        settings.slider("hitDuration", "Durée de l'éclair", 0.25f, 0.05f, 1.f, "", " s");

        settings.header("Dynamique");
        settings.toggle("dynamic", "Écartement dynamique", false,
                        "Le viseur s'ouvre en mouvement et se referme à l'arrêt.");
        settings.slider("spread", "Amplitude", 6.f, 1.f, 24.f, "", "px");
        settings.toggle("hideInMenus", "Masquer dans les menus", true);

        settings.find("color")->visibleWhen = [this] {
            return !settings.value<bool>("useThemeAccent", false);
        };
        settings.find("outlineColor")->visibleWhen = [this] {
            return settings.value<bool>("outline", true);
        };
        settings.find("hitColor")->visibleWhen = [this] {
            return settings.value<bool>("hitFlash", true);
        };
        settings.find("hitDuration")->visibleWhen = [this] {
            return settings.value<bool>("hitFlash", true);
        };
        settings.find("spread")->visibleWhen = [this] {
            return settings.value<bool>("dynamic", false);
        };
        settings.find("dotSize")->visibleWhen = [this] {
            const std::string style = settings.value<std::string>("style", "Croix");
            return style == "Point" || style == "Croix + point";
        };

        on(&Crosshair::onActorHurt);
        addKeywords({"viseur", "crosshair", "réticule", "mire"});
    }

    bool relevantNow() const override {
        if (settings.value<bool>("hideInMenus", true) && sdk::game().inMenu()) return false;
        return true;
    }

    Vec2 contentSize(Renderer& renderer) override {
        const float reach = currentGap() + settings.value<float>("length", 7.f) * scale();
        const float thickness = settings.value<float>("thickness", 2.f) * scale();
        const float extent = std::max(reach, settings.value<float>("dotSize", 2.f) * scale()) * 2.f;
        return {extent + thickness, extent + thickness};
    }

    void drawContent(Renderer& renderer, const Rect& content) override {
        const Vec2 centre = content.center();
        const float length = settings.value<float>("length", 7.f) * scale();
        const float gap = currentGap();
        const float thickness = settings.value<float>("thickness", 2.f) * scale();
        const std::string style = settings.value<std::string>("style", "Croix");

        hit_.speed = 8.f / std::max(0.05f, settings.value<float>("hitDuration", 0.25f));
        hit_.to(0.f);
        hit_.update(renderer.delta());

        if (settings.value<bool>("dynamic", false)) {
            const float speed = sdk::game().horizontalSpeed();
            spread_.speed = theme().motion(10.f);
            spread_.to(clamp(speed / 6.f, 0.f, 1.f));
            spread_.update(renderer.delta());
        } else {
            spread_.set(0.f);
        }

        Color colour = settings.value<bool>("useThemeAccent", false)
                           ? theme().liveAccent()
                           : settings.value<Color>("color", Color::rgb8(255, 255, 255, 230));

        if (settings.value<bool>("hitFlash", true) && hit_.value > 0.001f) {
            colour = lerp(colour, settings.value<Color>("hitColor", palette::kMint), hit_.value);
        }

        const bool outline = settings.value<bool>("outline", true);
        const Color outlineColour = settings.value<Color>("outlineColor", Color::rgb8(0, 0, 0, 160));
        const float outlineWidth = thickness + 2.f;

        std::vector<Rect> bars;

        const auto addBar = [&](float x0, float y0, float x1, float y1) {
            bars.push_back(Rect{x0, y0, x1, y1});
        };

        const bool wantsArms = style == "Croix" || style == "Croix + point" ||
                               style == "T inversé" || style == "Crochets";

        if (wantsArms) {
            const float half = thickness * 0.5f;

            if (style != "T inversé") {
                addBar(centre.x - gap - length, centre.y - half, centre.x - gap, centre.y + half);
                addBar(centre.x + gap, centre.y - half, centre.x + gap + length, centre.y + half);
                addBar(centre.x - half, centre.y - gap - length, centre.x + half, centre.y - gap);
            } else {
                addBar(centre.x - gap - length, centre.y - half, centre.x - gap, centre.y + half);
                addBar(centre.x + gap, centre.y - half, centre.x + gap + length, centre.y + half);
            }

            addBar(centre.x - half, centre.y + gap, centre.x + half, centre.y + gap + length);
        }

        if (style == "Crochets") {
            const float tip = length * 0.5f;
            const float half = thickness * 0.5f;
            addBar(centre.x - gap - length, centre.y - gap - length,
                   centre.x - gap - length + tip, centre.y - gap - length + thickness);
            addBar(centre.x + gap + length - tip, centre.y - gap - length,
                   centre.x + gap + length, centre.y - gap - length + thickness);
            addBar(centre.x - gap - length, centre.y - gap - length,
                   centre.x - gap - length + thickness, centre.y - gap - length + tip);
            addBar(centre.x + gap + length - thickness, centre.y - gap - length,
                   centre.x + gap + length, centre.y - gap - length + tip);
            (void)half;
        }

        if (outline) {
            for (const Rect& bar : bars) {
                renderer.fillRounded(bar.inflated(1.f), outlineColour, 1.f);
            }
        }
        for (const Rect& bar : bars) {
            renderer.fillRounded(bar, colour, thickness * 0.35f);
        }

        if (style == "Cercle") {
            const float radius = gap + length * 0.5f;
            if (outline) renderer.strokeCircle(centre, radius, outlineColour, outlineWidth);
            renderer.strokeCircle(centre, radius, colour, thickness);
        }

        if (style == "Point" || style == "Croix + point") {
            const float dot = settings.value<float>("dotSize", 2.f) * scale();
            if (outline) renderer.fillCircle(centre, dot + 1.f, outlineColour);
            renderer.fillCircle(centre, dot, colour);
        }
    }

private:
    [[nodiscard]] float currentGap() const {
        const float base = settings.value<float>("gap", 4.f) * scale();
        if (!settings.value<bool>("dynamic", false)) return base;
        return base + spread_.value * settings.value<float>("spread", 6.f) * scale();
    }

    void onActorHurt(ActorHurtEvent& event) { hit_.set(1.f); }

    Animated hit_{0.f, 12.f};
    Animated spread_{0.f, 10.f};
};

} // namespace

void registerRenderModules(ModuleManager& manager) {
    manager.add<Crosshair>();
}

} // namespace velyx
