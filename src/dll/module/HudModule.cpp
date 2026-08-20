#include "HudModule.hpp"

#include <array>

#include "dll/Velyx.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr std::array<const char*, 9> kAnchorLabels{
    "Haut gauche", "Haut centre", "Haut droite",
    "Milieu gauche", "Centre", "Milieu droite",
    "Bas gauche", "Bas centre", "Bas droite",
};

Vec2 anchorFraction(HudAnchor anchor) {
    switch (anchor) {
        case HudAnchor::TopLeft:      return {0.f, 0.f};
        case HudAnchor::TopCenter:    return {0.5f, 0.f};
        case HudAnchor::TopRight:     return {1.f, 0.f};
        case HudAnchor::MiddleLeft:   return {0.f, 0.5f};
        case HudAnchor::Center:       return {0.5f, 0.5f};
        case HudAnchor::MiddleRight:  return {1.f, 0.5f};
        case HudAnchor::BottomLeft:   return {0.f, 1.f};
        case HudAnchor::BottomCenter: return {0.5f, 1.f};
        case HudAnchor::BottomRight:  return {1.f, 1.f};
    }
    return {0.f, 0.f};
}

}

const char* anchorLabel(HudAnchor anchor) {
    const auto index = static_cast<size_t>(anchor);
    return index < kAnchorLabels.size() ? kAnchorLabels[index] : kAnchorLabels[0];
}

HudAnchor anchorFromLabel(std::string_view label) {
    for (size_t i = 0; i < kAnchorLabels.size(); ++i) {
        if (label == kAnchorLabels[i]) return static_cast<HudAnchor>(i);
    }
    return HudAnchor::TopLeft;
}

HudModule::HudModule(std::string id, std::string name, std::string description,
                     Vec2 defaultPosition, HudAnchor defaultAnchor)
    : Module(std::move(id), std::move(name), ModuleCategory::Hud, std::move(description)) {
    addLayoutSettings(defaultPosition, defaultAnchor);
    on(&HudModule::onRender);
}

void HudModule::addLayoutSettings(Vec2 defaultPosition, HudAnchor defaultAnchor) {
    settings.header("Position");
    settings.position("position", "Position", defaultPosition,
                      "Déplaçable directement dans l'éditeur de HUD.");

    std::vector<std::string> anchors(kAnchorLabels.begin(), kAnchorLabels.end());
    settings.dropdown("anchor", "Ancrage", anchorLabel(defaultAnchor), std::move(anchors),
                      "Le coin d'écran auquel l'élément reste accroché.");

    settings.slider("scale", "Échelle", 1.f, 0.4f, 3.f, "", "x");
    settings.slider("rotation", "Rotation", 0.f, -180.f, 180.f, "", "°").advanced = true;
    settings.slider("opacity", "Opacité", 1.f, 0.f, 1.f);

    settings.header("Apparence");
    settings.toggle("background", "Fond", true);
    settings.color("backgroundColor", "Couleur du fond", Color::rgb8(11, 31, 23, 160));
    settings.slider("radius", "Arrondi", 8.f, 0.f, 24.f, "", "px");
    settings.slider("padding", "Marge intérieure", 6.f, 0.f, 24.f, "", "px");
    settings.toggle("border", "Bordure", false);
    settings.color("borderColor", "Couleur de la bordure", palette::kMint.withAlpha(0.6f));
    settings.toggle("shadow", "Ombre portée", true);

    settings.header("Texte");
    settings.toggle("useThemeColor", "Utiliser la couleur du thème", true);
    settings.color("textColor", "Couleur du texte", palette::kSnow);
    settings.slider("fontSize", "Taille du texte", 15.f, 8.f, 40.f, "", "px");
    settings.text("fontFamily", "Police", "",
                  "Vide = police du thème. Sinon, nom exact d'une police installée.")
        .advanced = true;
    settings.toggle("textShadow", "Ombre du texte", false);

    settings.header("Organisation");
    settings.text("group", "Groupe", "",
                  "Les éléments d'un même groupe se déplacent ensemble dans l'éditeur.");
    settings.toggle("hideInScreenshots", "Masquer en mode capture", false);

    if (Setting* backgroundColor = settings.find("backgroundColor")) {
        backgroundColor->visibleWhen = [this] { return settings.value<bool>("background", true); };
    }
    if (Setting* radius = settings.find("radius")) {
        radius->visibleWhen = [this] { return settings.value<bool>("background", true); };
    }
    if (Setting* borderColor = settings.find("borderColor")) {
        borderColor->visibleWhen = [this] { return settings.value<bool>("border", false); };
    }
    if (Setting* textColor = settings.find("textColor")) {
        textColor->visibleWhen = [this] { return !settings.value<bool>("useThemeColor", true); };
    }
}

Vec2 HudModule::normalisedPosition() const {
    return settings.value<Vec2>("position", {0.02f, 0.02f});
}

HudAnchor HudModule::anchor() const {
    return anchorFromLabel(settings.value<std::string>("anchor", "Haut gauche"));
}

float HudModule::scale() const { return settings.value<float>("scale", 1.f); }
float HudModule::rotation() const { return settings.value<float>("rotation", 0.f); }
float HudModule::elementOpacity() const { return settings.value<float>("opacity", 1.f); }
float HudModule::padding() const { return settings.value<float>("padding", 6.f) * scale(); }
std::string HudModule::group() const { return settings.value<std::string>("group", ""); }

bool HudModule::hiddenInScreenshots() const {
    return settings.value<bool>("hideInScreenshots", false);
}

Color HudModule::textColor() const {
    if (settings.value<bool>("useThemeColor", true)) return theme().text;
    return settings.value<Color>("textColor", palette::kSnow);
}

FontSpec HudModule::fontFor(float sizeMultiplier, FontWeight weight) const {
    const auto& active = theme();

    FontSpec spec;
    const std::string family = settings.value<std::string>("fontFamily", "");
    spec.family = family.empty() ? active.fontFamily : family;
    spec.size = settings.value<float>("fontSize", 15.f) * sizeMultiplier * scale() *
                active.fontScale;
    spec.weight = weight;
    spec.letterSpacing = active.letterSpacing;
    spec.align = TextAlign::Left;
    spec.valign = TextVAlign::Top;
    return spec;
}

Rect HudModule::computeBounds(Vec2 size, Vec2 screenSize) const {
    const Vec2 fraction = normalisedPosition();
    const Vec2 anchorPoint{fraction.x * screenSize.x, fraction.y * screenSize.y};
    const Vec2 origin = anchorFraction(anchor());

    const Vec2 topLeft{anchorPoint.x - size.x * origin.x, anchorPoint.y - size.y * origin.y};
    return Rect::fromSize(topLeft.x, topLeft.y, size.x, size.y);
}

void HudModule::moveTo(Vec2 screenPosition, Vec2 screenSize) {
    if (screenSize.x <= 0.f || screenSize.y <= 0.f) return;

    settings.set("position", SettingValue{Vec2{clamp(screenPosition.x / screenSize.x, 0.f, 1.f),
                                               clamp(screenPosition.y / screenSize.y, 0.f, 1.f)}});
}

// Re-anchoring must not move the element: the stored fraction is recomputed so
// the rectangle stays exactly where it was dropped.
void HudModule::reanchorToNearestEdge(Vec2 screenSize) {
    if (screenSize.x <= 0.f || screenSize.y <= 0.f) return;

    const Vec2 center = lastBounds_.center();
    const float x = center.x / screenSize.x;
    const float y = center.y / screenSize.y;

    const int column = x < 0.33f ? 0 : (x < 0.67f ? 1 : 2);
    const int row = y < 0.33f ? 0 : (y < 0.67f ? 1 : 2);
    const auto newAnchor = static_cast<HudAnchor>(row * 3 + column);

    const Vec2 origin = anchorFraction(newAnchor);
    const Vec2 anchorPoint{lastBounds_.left + lastBounds_.width() * origin.x,
                           lastBounds_.top + lastBounds_.height() * origin.y};

    settings.set("anchor", SettingValue{std::string(anchorLabel(newAnchor))});
    settings.set("position", SettingValue{Vec2{anchorPoint.x / screenSize.x,
                                               anchorPoint.y / screenSize.y}});
}

void HudModule::onRender(RenderEvent& event) {
    Renderer& renderer = *event.renderer;
    const auto& active = theme();

    const crash::Breadcrumb breadcrumb(id().c_str());

    const bool shouldShow =
        relevantNow() && !(Velyx::get().screenshotMode() && hiddenInScreenshots());

    fade_.speed = active.motion(14.f);
    fade_.to(shouldShow ? 1.f : 0.f);
    fade_.update(event.deltaSeconds);

    if (fade_.value <= 0.005f) return;

    const Vec2 content = contentSize(renderer);
    if (content.x <= 0.f || content.y <= 0.f) return;

    const float pad = padding();
    const Vec2 total{content.x + pad * 2.f, content.y + pad * 2.f};

    lastBounds_ = computeBounds(total, event.screenSize);

    const float angle = rotation();
    const bool rotated = std::abs(angle) > 0.01f;
    if (rotated) renderer.pushRotation(angle, lastBounds_.center());

    renderer.pushOpacity(elementOpacity() * fade_.value);

    const float radius = settings.value<float>("radius", 8.f);

    if (settings.value<bool>("shadow", true) && active.shadows) {
        renderer.dropShadow(lastBounds_, active.shadowColor, radius, active.shadowSpread * 0.4f,
                            {0.f, 2.f});
    }

    if (settings.value<bool>("background", true)) {
        renderer.fillRounded(lastBounds_, settings.value<Color>("backgroundColor",
                                                               Color::rgb8(11, 31, 23, 160)),
                             radius);
    }

    if (settings.value<bool>("border", false)) {
        renderer.strokeRounded(lastBounds_,
                               settings.value<Color>("borderColor", active.accent), radius,
                               active.borderWidth);
    }

    const Rect content_rect = lastBounds_.inflated(-pad);
    renderer.pushClip(lastBounds_);
    drawContent(renderer, content_rect);
    renderer.popClip();

    renderer.popOpacity();
    if (rotated) renderer.popTransform();
}

}
