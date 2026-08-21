#include "Theme.hpp"

#include <algorithm>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/event/Events.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Theme";
constexpr const char* kExtension = ".velyxtheme";

const char* kBuiltInNames[] = {"Velyx", "Velyx Light", "Night", "Contrast"};

nlohmann::json colorToJson(const Color& color) { return color.toHex(true); }

Color colorFromJson(const nlohmann::json& node, const Color& fallback) {
    if (node.is_string()) return Color::fromHex(node.get<std::string>(), fallback);
    return fallback;
}

const char* easingName(Easing easing) {
    switch (easing) {
        case Easing::Linear:     return "linear";
        case Easing::OutQuad:    return "outQuad";
        case Easing::OutCubic:   return "outCubic";
        case Easing::OutQuart:   return "outQuart";
        case Easing::OutExpo:    return "outExpo";
        case Easing::InOutQuad:  return "inOutQuad";
        case Easing::InOutCubic: return "inOutCubic";
        case Easing::OutBack:    return "outBack";
        case Easing::OutElastic: return "outElastic";
    }
    return "outCubic";
}

Easing easingFromName(const std::string& name) {
    if (name == "linear")     return Easing::Linear;
    if (name == "outQuad")    return Easing::OutQuad;
    if (name == "outQuart")   return Easing::OutQuart;
    if (name == "outExpo")    return Easing::OutExpo;
    if (name == "inOutQuad")  return Easing::InOutQuad;
    if (name == "inOutCubic") return Easing::InOutCubic;
    if (name == "outBack")    return Easing::OutBack;
    if (name == "outElastic") return Easing::OutElastic;
    return Easing::OutCubic;
}

nlohmann::json serialize(const Theme& theme) {
    nlohmann::json json;

    json["name"] = theme.name;
    json["author"] = theme.author;
    json["description"] = theme.description;
    json["formatVersion"] = 1;

    auto& colors = json["colors"];
    colors["background"] = colorToJson(theme.background);
    colors["backgroundDeep"] = colorToJson(theme.backgroundDeep);
    colors["surface"] = colorToJson(theme.surface);
    colors["surfaceHover"] = colorToJson(theme.surfaceHover);
    colors["border"] = colorToJson(theme.border);
    colors["accent"] = colorToJson(theme.accent);
    colors["accentDeep"] = colorToJson(theme.accentDeep);
    colors["accentGlow"] = colorToJson(theme.accentGlow);
    colors["text"] = colorToJson(theme.text);
    colors["textMuted"] = colorToJson(theme.textMuted);
    colors["textDim"] = colorToJson(theme.textDim);
    colors["danger"] = colorToJson(theme.danger);
    colors["warning"] = colorToJson(theme.warning);
    colors["success"] = colorToJson(theme.success);
    colors["shadow"] = colorToJson(theme.shadowColor);
    colors["rgbAccent"] = theme.rgbAccent;
    colors["rgbSpeed"] = theme.rgbSpeed;

    auto& shape = json["shape"];
    shape["radius"] = theme.radius;
    shape["panelRadius"] = theme.panelRadius;
    shape["borderWidth"] = theme.borderWidth;
    shape["spacing"] = theme.spacing;

    auto& type = json["typography"];
    type["family"] = theme.fontFamily;
    type["mono"] = theme.monoFamily;
    type["scale"] = theme.fontScale;
    type["letterSpacing"] = theme.letterSpacing;

    auto& effects = json["effects"];
    effects["blur"] = theme.blur;
    effects["blurSigma"] = theme.blurSigma;
    effects["shadows"] = theme.shadows;
    effects["shadowSpread"] = theme.shadowSpread;
    effects["panelOpacity"] = theme.panelOpacity;

    auto& motion = json["motion"];
    motion["speed"] = theme.animationSpeed;
    motion["easing"] = easingName(theme.easing);

    return json;
}

Theme deserialize(const nlohmann::json& json) {
    Theme theme;

    theme.name = json.value("name", theme.name);
    theme.author = json.value("author", theme.author);
    theme.description = json.value("description", std::string{});

    if (json.contains("colors")) {
        const auto& c = json["colors"];
        theme.background = colorFromJson(c.value("background", nlohmann::json{}), theme.background);
        theme.backgroundDeep = colorFromJson(c.value("backgroundDeep", nlohmann::json{}), theme.backgroundDeep);
        theme.surface = colorFromJson(c.value("surface", nlohmann::json{}), theme.surface);
        theme.surfaceHover = colorFromJson(c.value("surfaceHover", nlohmann::json{}), theme.surfaceHover);
        theme.border = colorFromJson(c.value("border", nlohmann::json{}), theme.border);
        theme.accent = colorFromJson(c.value("accent", nlohmann::json{}), theme.accent);
        theme.accentDeep = colorFromJson(c.value("accentDeep", nlohmann::json{}), theme.accentDeep);
        theme.accentGlow = colorFromJson(c.value("accentGlow", nlohmann::json{}), theme.accentGlow);
        theme.text = colorFromJson(c.value("text", nlohmann::json{}), theme.text);
        theme.textMuted = colorFromJson(c.value("textMuted", nlohmann::json{}), theme.textMuted);
        theme.textDim = colorFromJson(c.value("textDim", nlohmann::json{}), theme.textDim);
        theme.danger = colorFromJson(c.value("danger", nlohmann::json{}), theme.danger);
        theme.warning = colorFromJson(c.value("warning", nlohmann::json{}), theme.warning);
        theme.success = colorFromJson(c.value("success", nlohmann::json{}), theme.success);
        theme.shadowColor = colorFromJson(c.value("shadow", nlohmann::json{}), theme.shadowColor);
        theme.rgbAccent = c.value("rgbAccent", theme.rgbAccent);
        theme.rgbSpeed = c.value("rgbSpeed", theme.rgbSpeed);
    }

    if (json.contains("shape")) {
        const auto& s = json["shape"];
        theme.radius = s.value("radius", theme.radius);
        theme.panelRadius = s.value("panelRadius", theme.panelRadius);
        theme.borderWidth = s.value("borderWidth", theme.borderWidth);
        theme.spacing = s.value("spacing", theme.spacing);
    }

    if (json.contains("typography")) {
        const auto& t = json["typography"];
        theme.fontFamily = t.value("family", theme.fontFamily);
        theme.monoFamily = t.value("mono", theme.monoFamily);
        theme.fontScale = t.value("scale", theme.fontScale);
        theme.letterSpacing = t.value("letterSpacing", theme.letterSpacing);
    }

    if (json.contains("effects")) {
        const auto& e = json["effects"];
        theme.blur = e.value("blur", theme.blur);
        theme.blurSigma = e.value("blurSigma", theme.blurSigma);
        theme.shadows = e.value("shadows", theme.shadows);
        theme.shadowSpread = e.value("shadowSpread", theme.shadowSpread);
        theme.panelOpacity = e.value("panelOpacity", theme.panelOpacity);
    }

    if (json.contains("motion")) {
        const auto& m = json["motion"];
        theme.animationSpeed = m.value("speed", theme.animationSpeed);
        theme.easing = easingFromName(m.value("easing", std::string("outCubic")));
    }

    return theme;
}

}

Color Theme::liveAccent() const {
    if (!rgbAccent) return accent;

    float h, s, v;
    accent.toHsv(h, s, v);

    return rainbow(rgbSpeed, s, v);
}

Color Theme::liveAccentDeep() const {
    if (!rgbAccent) return accentDeep;
    return liveAccent().darken(0.18f);
}

float Theme::motion(float base) const {
    if (animationSpeed <= 0.001f) return 1e6f;
    return base * animationSpeed;
}

ThemeManager& ThemeManager::get() {
    static ThemeManager instance;
    return instance;
}

bool ThemeManager::isBuiltIn(const std::string& name) {
    return std::ranges::any_of(kBuiltInNames,
                               [&](const char* builtIn) { return name == builtIn; });
}

void ThemeManager::registerBuiltIns() {
    Theme velyx;
    velyx.description = "Graphite surfaces, mint accent. The default theme.";
    themes_.push_back(velyx);

    Theme light = velyx;
    light.name = "Velyx Light";
    light.description = "The same grammar, on a light background.";
    light.background = Color::rgb8(247, 248, 249);
    light.backgroundDeep = Color::rgb8(233, 236, 238);
    light.surface = Color::rgb8(255, 255, 255);
    light.surfaceHover = Color::rgb8(240, 242, 244);
    light.border = Color::rgb8(219, 223, 227);
    light.accent = Color::rgb8(26, 152, 88);
    light.accentDeep = Color::rgb8(17, 120, 68);
    light.accentGlow = Color::rgb8(61, 220, 132);
    light.text = Color::rgb8(24, 27, 31);
    light.textMuted = Color::rgb8(96, 105, 115);
    light.textDim = Color::rgb8(140, 149, 158);
    light.shadowColor = Color::rgb8(20, 24, 30, 38);
    light.panelOpacity = 0.98f;
    themes_.push_back(light);

    Theme contrast = velyx;
    contrast.name = "Night";
    contrast.description = "Darker and tighter, for PvP.";
    contrast.background = Color::rgb8(10, 11, 13);
    contrast.backgroundDeep = Color::rgb8(0, 0, 0);
    contrast.surface = Color::rgb8(18, 20, 23);
    contrast.surfaceHover = Color::rgb8(30, 33, 38);
    contrast.border = Color::rgb8(38, 42, 48);
    contrast.accent = Color::rgb8(0, 240, 132);
    contrast.accentDeep = Color::rgb8(0, 180, 100);
    contrast.radius = 6.f;
    contrast.panelRadius = 10.f;
    contrast.blur = false;
    contrast.shadowSpread = 10.f;
    themes_.push_back(contrast);

    Theme access = velyx;
    access.name = "Contrast";
    access.description = "No animation, no blur, maximum contrast.";
    access.background = Color::rgb8(0, 0, 0);
    access.backgroundDeep = Color::rgb8(0, 0, 0);
    access.surface = Color::rgb8(20, 20, 20);
    access.surfaceHover = Color::rgb8(44, 44, 44);
    access.border = Color::rgb8(255, 255, 255);
    access.accent = Color::rgb8(0, 255, 140);
    access.accentDeep = Color::rgb8(0, 200, 110);
    access.text = Color::rgb8(255, 255, 255);
    access.textMuted = Color::rgb8(224, 224, 224);
    access.textDim = Color::rgb8(190, 190, 190);
    access.borderWidth = 2.f;
    access.fontScale = 1.15f;
    access.blur = false;
    access.shadows = false;
    access.animationSpeed = 0.f;
    themes_.push_back(access);
}

void ThemeManager::load() {
    themes_.clear();
    registerBuiltIns();

    std::error_code ec;
    std::filesystem::create_directories(Paths::themes(), ec);

    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(Paths::themes(), ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != kExtension) continue;

        std::ifstream stream(entry.path());
        nlohmann::json json;
        try {
            stream >> json;
        } catch (const std::exception& e) {
            Log::warn(kLog, "{} is not valid JSON: {}", entry.path().filename().string(), e.what());
            continue;
        }

        Theme theme = deserialize(json);
        if (theme.name.empty()) theme.name = entry.path().stem().string();

        if (isBuiltIn(theme.name)) theme.name += " (copy)";

        themes_.push_back(std::move(theme));
        ++loaded;
    }

    current_ = themes_.front();
    Log::info(kLog, "{} built-in + {} user theme(s)", std::size(kBuiltInNames), loaded);
}

const Theme* ThemeManager::find(const std::string& name) const {
    const auto it = std::ranges::find_if(themes_, [&](const Theme& t) { return t.name == name; });
    return it == themes_.end() ? nullptr : &*it;
}

bool ThemeManager::apply(const std::string& name) {
    const Theme* found = find(name);
    if (!found) {
        Log::warn(kLog, "no theme named '{}'", name);
        return false;
    }

    current_ = *found;

    ThemeChangeEvent event;
    event.name = name;
    events().emit(event);

    Log::info(kLog, "theme -> {}", name);
    return true;
}

bool ThemeManager::save(const Theme& theme) {
    Theme copy = theme;
    if (isBuiltIn(copy.name)) copy.name += " (copy)";

    std::error_code ec;
    std::filesystem::create_directories(Paths::themes(), ec);

    const auto path = Paths::themes() / (Paths::sanitize(copy.name) + kExtension);

    std::ofstream stream(path);
    if (!stream) {
        Log::error(kLog, "could not write {}", path.string());
        return false;
    }
    stream << serialize(copy).dump(2);

    if (auto* existing = const_cast<Theme*>(find(copy.name))) {
        *existing = copy;
    } else {
        themes_.push_back(copy);
    }

    current_ = copy;

    ThemeChangeEvent event;
    event.name = copy.name;
    events().emit(event);

    Log::info(kLog, "saved theme '{}'", copy.name);
    return true;
}

bool ThemeManager::remove(const std::string& name) {
    if (isBuiltIn(name)) return false;

    std::error_code ec;
    std::filesystem::remove(Paths::themes() / (Paths::sanitize(name) + kExtension), ec);

    std::erase_if(themes_, [&](const Theme& t) { return t.name == name; });

    if (current_.name == name) apply(kBuiltInNames[0]);
    return true;
}

bool ThemeManager::exportTo(const std::string& name, const std::filesystem::path& destination) const {
    const Theme* found = find(name);
    if (!found) return false;

    std::ofstream stream(destination);
    if (!stream) return false;

    stream << serialize(*found).dump(2);
    return true;
}

bool ThemeManager::importFrom(const std::filesystem::path& source, std::string* importedName) {
    std::ifstream stream(source);
    if (!stream) return false;

    nlohmann::json json;
    try {
        stream >> json;
    } catch (const std::exception& e) {
        Log::warn(kLog, "import failed: {}", e.what());
        return false;
    }

    Theme theme = deserialize(json);
    if (theme.name.empty()) theme.name = source.stem().string();

    if (!save(theme)) return false;
    if (importedName) *importedName = theme.name;
    return true;
}

std::string ThemeManager::exportCode() const {
    return "VELYXTHEME1:" + strings::base64Encode(serialize(current_).dump());
}

bool ThemeManager::importCode(std::string_view code, std::string* importedName) {
    constexpr std::string_view kPrefix = "VELYXTHEME1:";

    std::string_view body = strings::trim(code);
    if (!strings::startsWith(body, kPrefix)) {
        Log::warn(kLog, "unrecognised theme code");
        return false;
    }
    body.remove_prefix(kPrefix.size());

    const auto decoded = strings::base64Decode(body);
    if (!decoded) {
        Log::warn(kLog, "theme code is corrupt");
        return false;
    }

    Theme theme;
    try {
        theme = deserialize(nlohmann::json::parse(*decoded));
    } catch (const std::exception& e) {
        Log::warn(kLog, "theme code could not be parsed: {}", e.what());
        return false;
    }

    if (theme.name.empty()) theme.name = "Theme imported";
    if (!save(theme)) return false;

    if (importedName) *importedName = theme.name;
    return true;
}

std::vector<std::string> ThemeManager::names() const {
    std::vector<std::string> result;
    result.reserve(themes_.size());
    for (const Theme& t : themes_) result.push_back(t.name);
    return result;
}

}
