#include "Memory.hpp"

#include <windows.h>
#include <psapi.h>

#include <cctype>

#include "core/Log.hpp"

namespace velyx::memory {
namespace {

constexpr const char* kLog = "Memory";

ModuleRange computeGameModule() {
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return {};

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return {};

    return ModuleRange{reinterpret_cast<uintptr_t>(info.lpBaseOfDll), info.SizeOfImage};
}

ModuleRange computeGameText() {
    const auto& image = gameModule();
    if (!image.valid()) return {};

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return image;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return image;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            return ModuleRange{image.base + section->VirtualAddress,
                               section->Misc.VirtualSize};
        }
    }

    return image;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}

const ModuleRange& gameModule() {
    static const ModuleRange range = computeGameModule();
    return range;
}

const ModuleRange& gameText() {
    static const ModuleRange range = computeGameText();
    return range;
}

Pattern::Pattern(std::string_view ida) {
    size_t index = 0;
    while (index < ida.size()) {
        const char c = ida[index];

        if (std::isspace(static_cast<unsigned char>(c))) {
            ++index;
            continue;
        }

        if (c == '?') {
            bytes_.push_back(0);
            mask_.push_back(false);
            ++index;
            if (index < ida.size() && ida[index] == '?') ++index;
            continue;
        }

        const int high = hexValue(c);
        if (high < 0 || index + 1 >= ida.size()) {
            Log::error(kLog, "motif « {} » invalide à l'index {}", ida, index);
            bytes_.clear();
            mask_.clear();
            return;
        }

        const int low = hexValue(ida[index + 1]);
        if (low < 0) {
            Log::error(kLog, "motif « {} » invalide à l'index {}", ida, index + 1);
            bytes_.clear();
            mask_.clear();
            return;
        }

        bytes_.push_back(static_cast<uint8_t>(high * 16 + low));
        mask_.push_back(true);
        index += 2;
    }

    for (size_t i = 0; i < mask_.size(); ++i) {
        if (mask_[i]) {
            firstConcrete_ = i;
            hasConcrete_ = true;
            break;
        }
    }
}

bool Pattern::matchesAt(const uint8_t* data) const {
    for (size_t i = 0; i < bytes_.size(); ++i) {
        if (mask_[i] && data[i] != bytes_[i]) return false;
    }
    return true;
}

uintptr_t find(const Pattern& pattern, const ModuleRange& range) {
    if (!pattern.valid() || !range.valid() || pattern.size() > range.size) return 0;

    const auto* begin = reinterpret_cast<const uint8_t*>(range.base);
    const size_t last = range.size - pattern.size();

    if (!pattern.hasConcrete()) return range.base;

    const size_t anchor = pattern.firstConcrete();
    const uint8_t anchorByte = pattern.bytes()[anchor];

    size_t offset = 0;
    while (offset <= last) {
        const void* hit = std::memchr(begin + offset + anchor, anchorByte,
                                      last - offset + 1);
        if (!hit) return 0;

        offset = static_cast<size_t>(static_cast<const uint8_t*>(hit) - begin) - anchor;
        if (offset > last) return 0;

        if (pattern.matchesAt(begin + offset)) return range.base + offset;
        ++offset;
    }

    return 0;
}

uintptr_t find(std::string_view ida, const ModuleRange& range) {
    return find(Pattern(ida), range);
}

uintptr_t find(std::string_view ida) { return find(Pattern(ida), gameText()); }

std::vector<uintptr_t> findAll(const Pattern& pattern, const ModuleRange& range) {
    std::vector<uintptr_t> results;
    if (!pattern.valid() || !range.valid() || pattern.size() > range.size) return results;

    const auto* begin = reinterpret_cast<const uint8_t*>(range.base);
    const size_t last = range.size - pattern.size();

    for (size_t offset = 0; offset <= last; ++offset) {
        if (pattern.matchesAt(begin + offset)) results.push_back(range.base + offset);
    }

    return results;
}

uintptr_t resolveRelative(uintptr_t address, int operandOffset, int instructionLength) {
    if (!address) return 0;

    const auto displacement = read<int32_t>(address + static_cast<uintptr_t>(operandOffset));
    return address + static_cast<uintptr_t>(instructionLength) +
           static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
}

uintptr_t followChain(uintptr_t base, const std::vector<int>& offsets) {
    uintptr_t current = base;

    for (size_t i = 0; i < offsets.size(); ++i) {
        if (!current) return 0;
        current += static_cast<uintptr_t>(offsets[i]);

        if (i + 1 < offsets.size()) current = read<uintptr_t>(current);
    }

    return current;
}

bool readable(const void* pointer, size_t size) {
    if (!pointer) return false;

    // Page nulle : rien de valide n'y vit, et ce test seul attrape la plupart
    // des lectures apres liberation.
    if (reinterpret_cast<uintptr_t>(pointer) < 0x10000) return false;

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT) return false;

    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kReadable) == 0) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    const auto start = reinterpret_cast<uintptr_t>(pointer);
    const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + size <= regionEnd;
}

ProtectGuard::ProtectGuard(void* address, size_t size, unsigned long protection)
    : address_(address), size_(size) {
    DWORD previous = 0;
    ok_ = VirtualProtect(address, size, protection, &previous) != 0;
    previous_ = previous;
}

ProtectGuard::~ProtectGuard() {
    if (!ok_) return;
    DWORD ignored = 0;
    VirtualProtect(address_, size_, previous_, &ignored);
}

bool nop(uintptr_t address, size_t count) {
    if (!address || count == 0) return false;

    auto* target = reinterpret_cast<uint8_t*>(address);
    const ProtectGuard guard(target, count, PAGE_EXECUTE_READWRITE);
    if (!guard.ok()) return false;

    std::memset(target, 0x90, count);
    FlushInstructionCache(GetCurrentProcess(), target, count);
    return true;
}

bool patch(uintptr_t address, const std::vector<uint8_t>& bytes) {
    if (!address || bytes.empty()) return false;

    auto* target = reinterpret_cast<uint8_t*>(address);
    const ProtectGuard guard(target, bytes.size(), PAGE_EXECUTE_READWRITE);
    if (!guard.ok()) return false;

    std::memcpy(target, bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), target, bytes.size());
    return true;
}

}
