#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace velyx {

enum class SignatureKind {

    Direct,

    Relative,
};

struct SignatureSpec {
    std::string name;
    std::string pattern;
    SignatureKind kind = SignatureKind::Direct;
    int operandOffset = 1;
    int instructionLength = 5;
    int addend = 0;
    bool required = false;
    std::string owner;
};

struct SignatureResult {
    SignatureSpec spec;
    uintptr_t address = 0;
    bool resolved = false;
};

class Signatures {
public:
    static Signatures& get();

    void require(SignatureSpec spec);
    void requireOffset(std::string name, std::string owner, int fallback = -1);

    void resolveAll();

    [[nodiscard]] uintptr_t address(std::string_view name) const;
    [[nodiscard]] int offset(std::string_view name, int fallback = -1) const;

    [[nodiscard]] bool has(std::string_view name) const { return address(name) != 0; }

    [[nodiscard]] std::vector<std::string> missing() const;
    [[nodiscard]] std::vector<SignatureResult> all() const;

    [[nodiscard]] bool healthy() const;

    [[nodiscard]] const std::string& gameVersion() const;

    [[nodiscard]] std::string gameVersionKey() const;

private:
    Signatures() = default;

    bool loadPatterns();
    bool loadCache(const std::string& cacheKey);
    void saveCache(const std::string& cacheKey) const;
    void scan();

    std::unordered_map<std::string, SignatureResult> signatures_;
    std::unordered_map<std::string, int> offsets_;
    std::unordered_map<std::string, std::string> offsetOwners_;
    mutable std::optional<std::string> gameVersion_;
    bool resolved_ = false;
};

namespace sig {

inline uintptr_t address(std::string_view name) { return Signatures::get().address(name); }
inline int offset(std::string_view name, int fallback = -1) {
    return Signatures::get().offset(name, fallback);
}
inline bool has(std::string_view name) { return Signatures::get().has(name); }

}

}
