#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace velyx::memory {

struct ModuleRange {
    uintptr_t base = 0;
    size_t size = 0;

    [[nodiscard]] bool valid() const { return base != 0 && size != 0; }
    [[nodiscard]] uintptr_t end() const { return base + size; }
    [[nodiscard]] bool contains(uintptr_t address) const {
        return address >= base && address < end();
    }
};

const ModuleRange& gameModule();

const ModuleRange& gameText();

// The game's read-only data, where its strings and vtables live.
const ModuleRange& gameRodata();

// The start of the function that contains `address`, from the image's own exception
// table — every function on x64 Windows has an entry there, with its exact bounds,
// which is what lets a pattern anchored anywhere inside a function name it. A piece
// split off by the compiler is followed back to its primary entry. Zero when nothing
// covers the address.
uintptr_t functionStart(uintptr_t address);

// The first instruction in `range` whose rip-relative displacement lands on `target`:
// a lea, a mov, a call, whatever it is. Zero when nothing points there.
uintptr_t findReferenceTo(uintptr_t target, const ModuleRange& range);

// A NUL-terminated string in the read-only data, matched whole: the byte before it
// is a NUL too, so that a suffix of a longer string does not answer. Zero when absent.
uintptr_t findText(std::string_view text);

class Pattern {
public:
    Pattern() = default;
    explicit Pattern(std::string_view ida);

    [[nodiscard]] bool valid() const { return !bytes_.empty(); }
    [[nodiscard]] size_t size() const { return bytes_.size(); }
    [[nodiscard]] bool matchesAt(const uint8_t* data) const;

    [[nodiscard]] const std::vector<uint8_t>& bytes() const { return bytes_; }
    [[nodiscard]] const std::vector<bool>& mask() const { return mask_; }

    [[nodiscard]] size_t firstConcrete() const { return firstConcrete_; }
    [[nodiscard]] bool hasConcrete() const { return hasConcrete_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<bool> mask_;
    size_t firstConcrete_ = 0;
    bool hasConcrete_ = false;
};

uintptr_t find(const Pattern& pattern, const ModuleRange& range);
uintptr_t find(std::string_view ida, const ModuleRange& range);
uintptr_t find(std::string_view ida);

std::vector<uintptr_t> findAll(const Pattern& pattern, const ModuleRange& range);

uintptr_t resolveRelative(uintptr_t address, int operandOffset, int instructionLength);

uintptr_t followChain(uintptr_t base, const std::vector<int>& offsets);

bool readable(const void* pointer, size_t size = sizeof(void*));

// Finds the one live object whose first field is `vtable`, by walking this process's
// committed private memory.
//
// It exists because Bedrock has no global to read. `ClientInstance` is owned by a
// shared_ptr inside MinecraftGame and never lands in .data — verified on 1.26.44.3 by
// scanning the running game: exactly one object carries the vtable, and not one
// pointer to it lives in the image. A signature can still name the vtable, because
// the constructor loads its address with a lea; from there the object is whatever
// starts with it.
//
// Only heap-shaped regions are read — committed, private, plain read/write — so this
// never touches a guard page or the game's code. Returns 0 when nothing matches,
// which is the normal answer before a world is loaded.
uintptr_t findByVtable(uintptr_t vtable);

// The same walk, keeping the first candidate `accept` agrees with. A vtable's value
// turns up in freed blocks, in copies and on stacks, so the first qword that matches
// is rarely the object; something has to say which one is real.
uintptr_t findByVtable(uintptr_t vtable, const std::function<bool(uintptr_t)>& accept);

// Copies out of this process without ever dereferencing. Asking whether an address is
// readable and then reading it is two operations with a gap in between, and the game
// frees pages inside that gap — so the answer has to come from the same call that does
// the reading. False means the range was not fully there, and nothing was written.
bool copy(void* destination, uintptr_t source, size_t bytes);

template <typename T>
T read(uintptr_t address, T fallback = T{}) {
    T value{};
    if (!copy(&value, address, sizeof(T))) return fallback;
    return value;
}

template <typename T>
T* at(uintptr_t address) {
    return reinterpret_cast<T*>(address);
}

template <typename T>
T field(const void* object, int offset, T fallback = T{}) {
    if (!object || offset < 0) return fallback;
    return read<T>(reinterpret_cast<uintptr_t>(object) + static_cast<uintptr_t>(offset), fallback);
}

// Minecraft is built with MSVC, and the client is not necessarily built with the same
// compiler, so its `std::string` and ours are two different objects that happen to
// share a name. Every string that crosses the line goes through the game's layout,
// written out by hand: sixteen bytes that hold either the text or a pointer to it,
// then the size, then the capacity.
std::string readString(uintptr_t address);

// The other direction: a string laid out the way the game expects to receive one.
// The game only ever reads through it — a `std::string const&` parameter is never
// freed by the callee — so the buffer stays ours to own and to release.
class GameString {
public:
    explicit GameString(std::string_view text);
    ~GameString();

    GameString(const GameString&) = delete;
    GameString& operator=(const GameString&) = delete;

    template <typename T = void>
    [[nodiscard]] const T* as() const {
        return reinterpret_cast<const T*>(&layout_);
    }

private:
    struct Layout {
        char buffer[16]{};
        uint64_t size = 0;
        uint64_t capacity = 15;
    } layout_;

    char* heap_ = nullptr;
};

// The game's own heap, for the one case where a string changes hands. A `const&` is
// only read, which is what GameString is for; an object the game passes by value is
// destroyed by the function it is passed to, strings and all, so a string put into
// one has to be a block the game can free. The allocator is an object behind a global
// the pack names: allocating is the second slot of its vtable, freeing the third.
void setGameAllocator(uintptr_t global);
[[nodiscard]] bool gameHeapReady();

// New text for a string the game owns: the new buffer comes from the game's heap and
// is laid out the way the game's own code lays it out, and the old one goes back.
// False, with nothing touched, when the allocator is not known or will not give.
bool assignGameString(uintptr_t address, std::string_view text);

class ProtectGuard {
public:
    ProtectGuard(void* address, size_t size, unsigned long protection);
    ~ProtectGuard();

    ProtectGuard(const ProtectGuard&) = delete;
    ProtectGuard& operator=(const ProtectGuard&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }

private:
    void* address_ = nullptr;
    size_t size_ = 0;
    unsigned long previous_ = 0;
    bool ok_ = false;
};

bool nop(uintptr_t address, size_t count);
bool patch(uintptr_t address, const std::vector<uint8_t>& bytes);

}
