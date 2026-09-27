#include "Signatures.hpp"

#include <windows.h>

#include <chrono>
#include <format>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/memory/Memory.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Signatures";

std::filesystem::path moduleDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDirectory), &self);

    wchar_t buffer[MAX_PATH]{};
    if (self && GetModuleFileNameW(self, buffer, MAX_PATH)) {
        return std::filesystem::path(buffer).parent_path();
    }
    return {};
}

std::string readFileVersion() {
    wchar_t buffer[MAX_PATH]{};
    if (!GetModuleFileNameW(GetModuleHandleW(nullptr), buffer, MAX_PATH)) return "unknown";

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(buffer, &ignored);
    if (size == 0) return "unknown";

    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(buffer, 0, size, data.data())) return "unknown";

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoSize) || !info) {
        return "unknown";
    }

    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

SignatureKind parseKind(const std::string& text) {
    if (text == "relative") return SignatureKind::Relative;
    if (text == "anchor" || text == "string") return SignatureKind::Anchor;
    if (text == "vtable" || text == "slot") return SignatureKind::VtableSlot;
    return SignatureKind::Direct;
}

const char* kindName(SignatureKind kind) {
    switch (kind) {
        case SignatureKind::Direct:     return "direct";
        case SignatureKind::Relative:   return "relative";
        case SignatureKind::Anchor:     return "anchor";
        case SignatureKind::VtableSlot: return "vtable";
    }
    return "direct";
}

}

std::string SignatureSpec::describe() const {
    switch (kind) {
        case SignatureKind::Anchor:
            return std::format("anchor '{}'", text);
        case SignatureKind::VtableSlot:
            return std::format("{}[{}]", vtable, slot);
        default:
            break;
    }
    if (patterns.empty()) return "no pattern";
    return std::format("{} {}{}", kindName(kind), patterns.front(),
                       patterns.size() > 1 ? std::format(" (+{} more)", patterns.size() - 1)
                                           : "");
}

Signatures& Signatures::get() {
    static Signatures instance;
    return instance;
}

const std::string& Signatures::gameVersion() const {
    if (!gameVersion_) gameVersion_ = readFileVersion();
    return *gameVersion_;
}

std::string Signatures::gameVersionKey() const {
    const auto parts = strings::split(gameVersion(), '.');
    if (parts.size() < 2) return "unknown";
    return parts[0] + "." + parts[1];
}

void Signatures::require(SignatureSpec spec) {
    const std::string name = spec.name;

    auto it = signatures_.find(name);
    if (it != signatures_.end()) {

        it->second.spec.required = it->second.spec.required || spec.required;
        if (!spec.owner.empty() && it->second.spec.owner.find(spec.owner) == std::string::npos) {
            it->second.spec.owner += ", " + spec.owner;
        }
        return;
    }

    SignatureResult result;
    result.spec = std::move(spec);
    signatures_.emplace(name, std::move(result));
}

void Signatures::requireOffset(std::string name, std::string owner, int fallback) {
    offsetOwners_.emplace(name, std::move(owner));
    offsets_.try_emplace(std::move(name), fallback);
}

namespace {

// One level of a pack: its signatures, then its offsets, read over whatever an earlier
// level already said about the same names.
void readSection(const nlohmann::json& document,
                 std::unordered_map<std::string, SignatureResult>& signatures,
                 std::unordered_map<std::string, int>& offsets, int& patternCount,
                 int& offsetCount) {
    if (document.contains("signatures") && document["signatures"].is_object()) {
        for (const auto& [name, entry] : document["signatures"].items()) {
            // A key here is read as a signature whatever it is called, so a note
            // left in the object would be scanned as a pattern — and counted as one
            // that failed. Names starting with an underscore are notes, the same
            // convention the offsets already use.
            if (name.empty() || name.front() == '_') continue;

            auto it = signatures.find(name);
            if (it == signatures.end()) {

                SignatureResult placeholder;
                placeholder.spec.name = name;
                placeholder.spec.owner = "external";
                it = signatures.emplace(name, std::move(placeholder)).first;
            }

            auto& spec = it->second.spec;
            const auto readPatterns = [&spec](const nlohmann::json& value) {
                spec.patterns.clear();
                if (value.is_string()) {
                    if (!value.get<std::string>().empty()) spec.patterns.push_back(value);
                } else if (value.is_array()) {
                    for (const auto& item : value) {
                        if (item.is_string() && !item.get<std::string>().empty()) {
                            spec.patterns.push_back(item);
                        }
                    }
                }
            };

            if (entry.is_string()) {
                readPatterns(entry);
            } else if (entry.is_object()) {
                if (entry.contains("pattern")) readPatterns(entry["pattern"]);
                spec.kind = parseKind(entry.value("kind", std::string("direct")));
                spec.operandOffset = entry.value("operand", spec.operandOffset);
                spec.instructionLength = entry.value("length", spec.instructionLength);
                spec.addend = entry.value("addend", spec.addend);
                spec.functionStart = entry.value("function", spec.functionStart);
                spec.text = entry.value("text", spec.text);
                spec.vtable = entry.value("vtable", spec.vtable);
                spec.slot = entry.value("slot", spec.slot);
            }

            // An empty entry is the template's way of listing a name; it is not a
            // pattern that failed.
            const bool empty = spec.patterns.empty() && spec.text.empty() && spec.vtable.empty();
            if (!empty) ++patternCount;
        }
    }

    if (document.contains("offsets") && document["offsets"].is_object()) {
        for (const auto& [name, entry] : document["offsets"].items()) {
            if (!entry.is_number_integer()) continue;
            offsets[name] = entry.get<int>();
            ++offsetCount;
        }
    }
}

}

bool Signatures::loadPatterns() {
    const std::string key = gameVersionKey();

    const std::vector<std::filesystem::path> candidates{
        moduleDirectory() / "assets" / "signatures" / (key + ".json"),
        Paths::assets() / "signatures" / (key + ".json"),
        Paths::config() / "signatures.json",
    };

    bool loadedAny = false;

    for (const auto& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;

        std::ifstream stream(path);
        nlohmann::json document;
        try {
            stream >> document;
        } catch (const std::exception& e) {
            Log::error(kLog, "{} is not valid JSON: {}", path.string(), e.what());
            continue;
        }

        int patternCount = 0;
        int offsetCount = 0;
        readSection(document, signatures_, offsets_, patternCount, offsetCount);

        // A pack covers a minor version, and a build inside it that moved something
        // carries its own entries under its exact version, read over the shared ones.
        if (document.contains("builds") && document["builds"].is_object()) {
            const auto build = document["builds"].find(gameVersion());
            if (build != document["builds"].end() && build->is_object()) {
                int buildPatterns = 0;
                int buildOffsets = 0;
                readSection(*build, signatures_, offsets_, buildPatterns, buildOffsets);
                Log::info(kLog, "{} carries {} pattern(s) and {} offset(s) of its own", gameVersion(),
                          buildPatterns, buildOffsets);
            }
        }

        Log::info(kLog, "loaded {} patterns and {} offsets from {}", patternCount, offsetCount,
                  path.filename().string());
        loadedAny = true;
    }

    if (!loadedAny) {
        Log::warn(kLog,
                  "no signature pack for game {}, expected assets/signatures/{}.json. "
                  "Velyx will start in reduced mode.",
                  gameVersion(), key);
    }

    return loadedAny;
}

uintptr_t Signatures::resolvePattern(const SignatureSpec& spec, const std::string& pattern) const {
    const auto& text = memory::gameText();
    const memory::Pattern compiled(pattern);
    if (!compiled.valid()) return 0;

    // Exactly one match, or none: a pattern that fits twice names nothing, and a
    // hook on the wrong one of two is worse than no hook.
    const std::vector<uintptr_t> matches = memory::findAll(compiled, text);
    if (matches.size() != 1) {
        if (matches.size() > 1) {
            Log::debug(kLog, "'{}': pattern matched {} times, not taken", spec.name, matches.size());
        }
        return 0;
    }

    uintptr_t address = matches.front();
    if (spec.kind == SignatureKind::Relative) {
        address = memory::resolveRelative(address, spec.operandOffset, spec.instructionLength);
    }
    if (spec.functionStart) {
        const uintptr_t start = memory::functionStart(address);
        if (start == 0) {
            Log::debug(kLog, "'{}': no function contains {:#x}", spec.name, address);
            return 0;
        }
        address = start;
    }
    return address + static_cast<uintptr_t>(spec.addend);
}

uintptr_t Signatures::resolveAnchor(const SignatureSpec& spec) const {
    if (spec.text.empty()) return 0;

    const uintptr_t string = memory::findText(spec.text);
    if (string == 0) {
        Log::debug(kLog, "'{}': the anchor text is not in this build", spec.name);
        return 0;
    }

    const uintptr_t site = memory::findReferenceTo(string, memory::gameText());
    if (site == 0) {
        Log::debug(kLog, "'{}': nothing in the code references the anchor text", spec.name);
        return 0;
    }

    const uintptr_t start = memory::functionStart(site);
    if (start == 0) {
        Log::debug(kLog, "'{}': no function contains the anchor's reference", spec.name);
        return 0;
    }
    return start + static_cast<uintptr_t>(spec.addend);
}

uintptr_t Signatures::resolveSlot(const SignatureSpec& spec) const {
    if (spec.vtable.empty() || spec.slot < 0) return 0;

    const uintptr_t vtable = address(spec.vtable);
    if (vtable == 0) return 0;

    const auto entry = memory::read<uintptr_t>(vtable + static_cast<uintptr_t>(spec.slot) * 8);
    if (!memory::gameText().contains(entry)) {
        Log::debug(kLog, "'{}': slot {} of {} does not point into the code", spec.name, spec.slot,
                   spec.vtable);
        return 0;
    }
    return entry + static_cast<uintptr_t>(spec.addend);
}

bool Signatures::resolveOne(SignatureResult& result) {
    const SignatureSpec& spec = result.spec;

    uintptr_t address = 0;
    switch (spec.kind) {
        case SignatureKind::Anchor:
            // A retail build strips most of its assertions; an anchor entry may carry
            // patterns for those builds, tried once the text is not there.
            address = resolveAnchor(spec);
            for (size_t i = 0; address == 0 && i < spec.patterns.size(); ++i) {
                address = resolvePattern(spec, spec.patterns[i]);
            }
            break;
        case SignatureKind::VtableSlot:
            address = resolveSlot(spec);
            break;
        case SignatureKind::Direct:
        case SignatureKind::Relative:
            for (const std::string& pattern : spec.patterns) {
                address = resolvePattern(spec, pattern);
                if (address != 0) break;
            }
            break;
    }

    if (address == 0) return false;

    result.address = address;
    result.resolved = true;
    return true;
}

void Signatures::scan() {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    const auto& text = memory::gameText();
    if (!text.valid()) {
        Log::error(kLog, "could not locate the game .text section");
        return;
    }

    int resolvedCount = 0;
    int failedCount = 0;

    // Slots read a vtable another entry names, so they go last, once the vtables are
    // in. Everything else is independent.
    for (const bool slotsPass : {false, true}) {
        for (auto& [name, result] : signatures_) {
            if (result.resolved) continue;
            if ((result.spec.kind == SignatureKind::VtableSlot) != slotsPass) continue;

            const SignatureSpec& spec = result.spec;
            const bool empty = spec.patterns.empty() && spec.text.empty() && spec.vtable.empty();
            if (empty) {
                ++failedCount;
                continue;
            }

            if (resolveOne(result)) {
                ++resolvedCount;
                continue;
            }

            ++failedCount;
            if (spec.required) {
                Log::warn(kLog, "required signature '{}' ({}) did not match", name,
                          spec.owner.empty() ? "unowned" : spec.owner);
            }
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
    Log::info(kLog, "resolved {}/{} signatures in {} ms", resolvedCount,
              resolvedCount + failedCount, elapsed);
}

bool Signatures::loadCache(const std::string& cacheKey) {
    const auto path = Paths::cache() / std::format("signatures-{}.json", cacheKey);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    std::ifstream stream(path);
    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception&) {
        return false;
    }

    const uintptr_t base = memory::gameModule().base;
    int hits = 0;

    for (const auto& [name, rva] : document.items()) {
        if (!rva.is_number_unsigned()) continue;

        auto it = signatures_.find(name);
        if (it == signatures_.end()) continue;

        it->second.address = base + rva.get<uintptr_t>();
        it->second.resolved = true;
        ++hits;
    }

    if (hits > 0) Log::info(kLog, "reused {} cached addresses", hits);
    return hits > 0;
}

void Signatures::saveCache(const std::string& cacheKey) const {
    const uintptr_t base = memory::gameModule().base;

    nlohmann::json document = nlohmann::json::object();
    for (const auto& [name, result] : signatures_) {
        if (!result.resolved || result.address < base) continue;
        document[name] = result.address - base;
    }

    std::error_code ec;
    std::filesystem::create_directories(Paths::cache(), ec);

    std::ofstream stream(Paths::cache() / std::format("signatures-{}.json", cacheKey));
    stream << document.dump(1, '\t');
}

void Signatures::resolveAll() {
    if (resolved_) return;
    resolved_ = true;

    loadPatterns();

    std::string patternFingerprint;
    patternFingerprint.reserve(signatures_.size() * 24);
    for (const auto& [name, result] : signatures_) {
        patternFingerprint += name;
        for (const std::string& pattern : result.spec.patterns) patternFingerprint += pattern;
        patternFingerprint += result.spec.text;
        patternFingerprint += result.spec.vtable;
        patternFingerprint += std::to_string(result.spec.slot);
        patternFingerprint += result.spec.functionStart ? "f" : "";
    }
    const std::string cacheKey =
        gameVersion() + "-" + strings::hashId(patternFingerprint).substr(0, 8);

    if (!loadCache(cacheKey)) {
        scan();
        saveCache(cacheKey);
    }

    const auto absent = missing();
    if (!absent.empty()) {
        Log::warn(kLog, "{} signature(s) unresolved: {}", absent.size(),
                  strings::join(absent, ", "));
    }
}

uintptr_t Signatures::address(std::string_view name) const {
    const auto it = signatures_.find(std::string(name));
    return it != signatures_.end() && it->second.resolved ? it->second.address : 0;
}

int Signatures::offset(std::string_view name, int fallback) const {
    const auto it = offsets_.find(std::string(name));
    if (it == offsets_.end() || it->second < 0) return fallback;
    return it->second;
}

std::vector<std::string> Signatures::missing() const {
    std::vector<std::string> names;
    for (const auto& [name, result] : signatures_) {
        if (!result.resolved && result.spec.required) names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

std::vector<SignatureResult> Signatures::all() const {
    std::vector<SignatureResult> results;
    results.reserve(signatures_.size());
    for (const auto& [name, result] : signatures_) results.push_back(result);

    std::ranges::sort(results, [](const SignatureResult& a, const SignatureResult& b) {
        return a.spec.name < b.spec.name;
    });
    return results;
}

bool Signatures::healthy() const { return missing().empty(); }

}
