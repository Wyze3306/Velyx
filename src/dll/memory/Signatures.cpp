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
    return text == "relative" ? SignatureKind::Relative : SignatureKind::Direct;
}

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
        if (document.contains("signatures") && document["signatures"].is_object()) {
            for (const auto& [name, entry] : document["signatures"].items()) {
                auto it = signatures_.find(name);
                if (it == signatures_.end()) {

                    SignatureResult placeholder;
                    placeholder.spec.name = name;
                    placeholder.spec.owner = "external";
                    it = signatures_.emplace(name, std::move(placeholder)).first;
                }

                auto& spec = it->second.spec;
                if (entry.is_string()) {
                    spec.pattern = entry.get<std::string>();
                } else if (entry.is_object()) {
                    spec.pattern = entry.value("pattern", spec.pattern);
                    spec.kind = parseKind(entry.value("kind", std::string("direct")));
                    spec.operandOffset = entry.value("operand", spec.operandOffset);
                    spec.instructionLength = entry.value("length", spec.instructionLength);
                    spec.addend = entry.value("addend", spec.addend);
                }
                ++patternCount;
            }
        }

        int offsetCount = 0;
        if (document.contains("offsets") && document["offsets"].is_object()) {
            for (const auto& [name, entry] : document["offsets"].items()) {
                if (!entry.is_number_integer()) continue;
                offsets_[name] = entry.get<int>();
                ++offsetCount;
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

    for (auto& [name, result] : signatures_) {
        if (result.resolved) continue;
        if (result.spec.pattern.empty()) {
            ++failedCount;
            continue;
        }

        uintptr_t address = memory::find(result.spec.pattern, text);
        if (address == 0) {
            ++failedCount;
            if (result.spec.required) {
                Log::warn(kLog, "required signature '{}' ({}) did not match", name,
                          result.spec.owner.empty() ? "unowned" : result.spec.owner);
            }
            continue;
        }

        if (result.spec.kind == SignatureKind::Relative) {
            address = memory::resolveRelative(address, result.spec.operandOffset,
                                              result.spec.instructionLength);
        }
        address += static_cast<uintptr_t>(result.spec.addend);

        result.address = address;
        result.resolved = true;
        ++resolvedCount;
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
        patternFingerprint += result.spec.pattern;
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
