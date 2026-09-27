#include "GameOptions.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>

#include "core/Log.hpp"

namespace velyx::gameoptions {
namespace {

constexpr const char* kLog = "GameOptions";
constexpr const char* kBackupName = "options.velyx-backup.txt";

// An empty target means the preset has no opinion about that setting and leaves it
// where the player put it. Presets that guess at a value they are not sure of are
// worse than presets that do less, because the player cannot tell the difference
// until something looks wrong.
struct Knob {
    const char* label;

    // Bedrock has renamed settings across versions. Whichever of these the file
    // already has is the one that gets written; if it has none of them, the setting is
    // reported as not offered rather than invented.
    const char* keys[3];

    const char* performance;
    const char* balanced;

    // The one setting whose value cannot be written blind: see renderDistanceFor().
    bool distance = false;
};

constexpr Knob kKnobs[] = {
    {"Render distance", {"gfx_viewdistance", nullptr, nullptr}, "", "", true},

    {"Fancy graphics", {"gfx_fancygraphics", nullptr, nullptr}, "0", "0"},
    {"Fancy leaves", {"gfx_fancyleaves", "gfx_transparentleaves", nullptr}, "0", "0"},
    {"Smooth lighting", {"gfx_smoothlighting", nullptr, nullptr}, "0", "1"},
    {"Vertical sync", {"gfx_vsync", nullptr, nullptr}, "0", "0"},
    {"Multisampling", {"gfx_msaa", nullptr, nullptr}, "1", "1"},
    {"Texel anti-aliasing", {"gfx_texel_aa_enabled", nullptr, nullptr}, "0", "0"},
    {"Clouds", {"gfx_render_clouds", "gfx_renderclouds", nullptr}, "0", ""},
    {"Particles", {"gfx_particleviewdistance", nullptr, nullptr}, "0", ""},
    {"View bobbing", {"gfx_bobview", "gfx_viewbobbing", nullptr}, "0", ""},
    {"Screen animations", {"gfx_screenanimations", nullptr, nullptr}, "0", ""},
    {"Camera shake", {"gfx_camerashake", nullptr, nullptr}, "0", ""},
};

std::filesystem::path localAppData() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) return {};

    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

// The file is key:value a line, in the order the game wrote it. Keeping the lines
// themselves rather than a map means everything not touched comes back out byte for
// byte, including anything a newer version added that nothing here knows about.
struct Line {
    std::string raw;
    std::string key;
    std::string value;
    bool pair = false;
};

std::vector<Line> read(const std::filesystem::path& path, bool& found) {
    std::vector<Line> lines;
    found = false;

    std::ifstream file(path);
    if (!file) return lines;
    found = true;

    std::string raw;
    while (std::getline(file, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        Line line;
        line.raw = raw;

        const size_t colon = raw.find(':');
        if (colon != std::string::npos && colon > 0) {
            line.key = raw.substr(0, colon);
            line.value = raw.substr(colon + 1);
            line.pair = true;
        }

        lines.push_back(std::move(line));
    }

    return lines;
}

bool write(const std::filesystem::path& path, const std::vector<Line>& lines, std::string* error) {
    // Written beside the real one and moved over it, so a launcher that dies halfway
    // through leaves the game's own file intact rather than half of one.
    const std::filesystem::path staging = path.parent_path() / "options.velyx-staging.txt";

    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        if (!file) {
            if (error) *error = "could not write beside " + path.string();
            return false;
        }

        for (const Line& line : lines) file << line.raw << '\n';
    }

    std::error_code ec;
    std::filesystem::rename(staging, path, ec);
    if (!ec) return true;

    // Same folder, so a rename should not need a copy; when it does, the copy is
    // still better than leaving the staging file behind.
    std::filesystem::copy_file(staging, path, std::filesystem::copy_options::overwrite_existing,
                               ec);
    std::filesystem::remove(staging, ec);

    if (ec && error) *error = "could not replace " + path.string() + ": " + ec.message();
    return !ec;
}

Line* find(std::vector<Line>& lines, const Knob& knob, std::string& matched) {
    for (const char* key : knob.keys) {
        if (key == nullptr) break;

        for (Line& line : lines) {
            if (!line.pair || line.key != key) continue;
            matched = key;
            return &line;
        }
    }
    return nullptr;
}

// Whether the game counts a view distance in blocks or in chunks has moved around, and
// the two are a factor of sixteen apart: writing chunks where blocks were expected is
// a render distance of four, and writing blocks where chunks were expected is a
// hundred and twelve chunks, which is the opposite of a performance preset. So the
// unit is read off the value already there rather than assumed. Nobody plays at
// thirty-three chunks, and nobody plays at thirty-two blocks.
std::string renderDistanceFor(const std::string& current, Preset preset) {
    int value = 0;
    const char* first = current.data();
    const char* last = first + current.size();
    if (std::from_chars(first, last, value).ec != std::errc{} || value <= 0) return {};

    const bool blocks = value > 32;
    const int chunks = preset == Preset::Performance ? 6 : 10;

    return std::to_string(blocks ? chunks * 16 : chunks);
}

Result run(const std::string& packageFamilyName, Preset preset, bool commit) {
    Result result;

    const std::filesystem::path path = optionsPath(packageFamilyName);
    if (path.empty()) {
        result.error = "this instance has no package identity yet";
        return result;
    }

    bool found = false;
    std::vector<Line> lines = read(path, found);
    if (!found) {
        result.noOptionsYet = true;
        result.error = "the game has not written its settings yet — launch this instance once";
        return result;
    }

    for (const Knob& knob : kKnobs) {
        std::string key;
        Line* line = find(lines, knob, key);
        if (line == nullptr) {
            result.notOffered.emplace_back(knob.label);
            continue;
        }

        const std::string target =
            knob.distance
                ? renderDistanceFor(line->value, preset)
                : std::string(preset == Preset::Performance ? knob.performance : knob.balanced);

        if (target.empty() || target == line->value) continue;

        result.changes.push_back(Change{knob.label, key, line->value, target});

        if (!commit) continue;

        line->value = target;
        line->raw = key + ":" + target;
    }

    if (!commit) {
        result.ok = true;
        return result;
    }

    if (result.changes.empty()) {
        result.ok = true;
        return result;
    }

    // Taken once and never overwritten: the point of it is the file as the player had
    // it, not the file as the last preset left it.
    const std::filesystem::path backup = path.parent_path() / kBackupName;
    std::error_code ec;
    if (!std::filesystem::exists(backup, ec)) {
        std::filesystem::copy_file(path, backup, ec);
        if (ec) {
            result.error = "could not keep a copy of the original: " + ec.message();
            return result;
        }
    }

    result.ok = write(path, lines, &result.error);
    if (result.ok) {
        Log::info(kLog, "{} preset: {} setting(s) changed in {}", presetName(preset),
                  result.changes.size(), path.string());
    }
    return result;
}

}

const char* presetName(Preset preset) {
    return preset == Preset::Performance ? "Performance" : "Balanced";
}

std::filesystem::path optionsPath(const std::string& packageFamilyName) {
    if (packageFamilyName.empty()) return {};

    const std::filesystem::path local = localAppData();
    if (local.empty()) return {};

    return local / "Packages" / packageFamilyName / "LocalState" / "games" / "com.mojang" /
           "minecraftpe" / "options.txt";
}

std::string Result::summary() const {
    if (!error.empty()) return error;
    if (changes.empty()) return "nothing to change; the settings already match";

    std::string text = std::format("{} setting{} changed", changes.size(),
                                   changes.size() == 1 ? "" : "s");
    if (!notOffered.empty()) {
        text += std::format(", {} not offered by this version", notOffered.size());
    }
    return text;
}

Result apply(const std::string& packageFamilyName, Preset preset) {
    return run(packageFamilyName, preset, true);
}

Result preview(const std::string& packageFamilyName, Preset preset) {
    return run(packageFamilyName, preset, false);
}

bool hasBackup(const std::string& packageFamilyName) {
    const std::filesystem::path path = optionsPath(packageFamilyName);
    if (path.empty()) return false;

    std::error_code ec;
    return std::filesystem::exists(path.parent_path() / kBackupName, ec);
}

bool restore(const std::string& packageFamilyName, std::string* error) {
    const std::filesystem::path path = optionsPath(packageFamilyName);
    if (path.empty()) {
        if (error) *error = "this instance has no package identity yet";
        return false;
    }

    const std::filesystem::path backup = path.parent_path() / kBackupName;

    std::error_code ec;
    if (!std::filesystem::exists(backup, ec)) {
        if (error) *error = "there is no copy of the original to go back to";
        return false;
    }

    std::filesystem::copy_file(backup, path, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        if (error) *error = "could not put the original back: " + ec.message();
        return false;
    }

    // Gone, so that the next preset takes its own copy of what is now the original.
    std::filesystem::remove(backup, ec);

    Log::info(kLog, "restored the original settings in {}", path.string());
    return true;
}

}
