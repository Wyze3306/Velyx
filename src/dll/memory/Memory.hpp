#pragma once

#include <cstdint>
#include <cstring>
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

template <typename T>
T read(uintptr_t address, T fallback = T{}) {
    if (!readable(reinterpret_cast<const void*>(address), sizeof(T))) return fallback;
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
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
