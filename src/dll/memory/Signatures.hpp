#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace velyx {

// How a pack entry names an address. Every kind ends in an address the client can
// stand on; they differ in what survives a rebuild.
enum class SignatureKind {

    // The pattern's match is the target.
    Direct,

    // The pattern lands on an instruction that references the target through a
    // rip-relative displacement, which is followed.
    Relative,

    // The target is the function that carries an assertion string: the string is
    // found in the read-only data, the one instruction referencing it in the code,
    // and the function containing that instruction in the exception table. Nothing
    // in it is a byte of code, so a rebuild does not move it.
    Anchor,

    // The target is a slot of a vtable another entry names.
    VtableSlot,
};

struct SignatureSpec {
    std::string name;

    // Alternatives, tried in order; the first that matches exactly once wins. A pack
    // that has to cover two builds of the same version puts both here.
    std::vector<std::string> patterns;

    SignatureKind kind = SignatureKind::Direct;
    int operandOffset = 1;
    int instructionLength = 5;
    int addend = 0;

    // After matching, walk to the start of the function the match sits in. What a
    // pattern anchored on a distinctive instruction in the middle of a function needs
    // to name the function itself.
    bool functionStart = false;

    // Anchor: the assertion text, byte for byte.
    std::string text;

    // VtableSlot: the entry holding the vtable, and the slot.
    std::string vtable;
    int slot = -1;

    bool required = false;
    std::string owner;

    // What the pack said, for the log and the Diagnostics page.
    [[nodiscard]] std::string describe() const;
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

    bool resolveOne(SignatureResult& result);
    uintptr_t resolvePattern(const SignatureSpec& spec, const std::string& pattern) const;
    uintptr_t resolveAnchor(const SignatureSpec& spec) const;
    uintptr_t resolveSlot(const SignatureSpec& spec) const;

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
