#include "Memory.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <vector>

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

const IMAGE_NT_HEADERS64* ntHeaders() {
    const auto& image = gameModule();
    if (!image.valid()) return nullptr;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

ModuleRange computeSection(const char* name, size_t length) {
    const auto& image = gameModule();
    const IMAGE_NT_HEADERS64* nt = ntHeaders();
    if (!nt) return {};

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, name, length) == 0) {
            return ModuleRange{image.base + section->VirtualAddress, section->Misc.VirtualSize};
        }
    }
    return {};
}

ModuleRange computeGameText() {
    const ModuleRange text = computeSection(".text", 6);
    return text.valid() ? text : gameModule();
}

ModuleRange computeGameRodata() { return computeSection(".rdata", 7); }

// The exception directory: one RUNTIME_FUNCTION per function or function piece, in
// address order.
struct ExceptionTable {
    const RUNTIME_FUNCTION* entries = nullptr;
    size_t count = 0;
};

ExceptionTable exceptionTable() {
    const auto& image = gameModule();
    const IMAGE_NT_HEADERS64* nt = ntHeaders();
    if (!nt) return {};

    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(RUNTIME_FUNCTION)) return {};

    return {reinterpret_cast<const RUNTIME_FUNCTION*>(image.base + directory.VirtualAddress),
            directory.Size / sizeof(RUNTIME_FUNCTION)};
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

const ModuleRange& gameRodata() {
    static const ModuleRange range = computeGameRodata();
    return range;
}

uintptr_t functionStart(uintptr_t address) {
    const auto& image = gameModule();
    const ExceptionTable table = exceptionTable();
    if (!image.valid() || table.count == 0 || !image.contains(address)) return 0;

    const auto rva = static_cast<DWORD>(address - image.base);

    // Sorted by start, so the last entry starting at or before the address is the
    // one that could contain it.
    size_t low = 0;
    size_t high = table.count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (table.entries[middle].BeginAddress <= rva) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) return 0;

    const RUNTIME_FUNCTION* entry = &table.entries[low - 1];
    if (rva < entry->BeginAddress || rva >= entry->EndAddress) return 0;

    // A function the compiler split keeps one primary entry and chains the pieces to
    // it: flag 4 in the unwind info, with the primary RUNTIME_FUNCTION stored after
    // the unwind codes, which come in pairs.
    constexpr uint8_t kChainInfo = 0x4;
    for (int hop = 0; hop < 8; ++hop) {
        const auto* unwind = reinterpret_cast<const uint8_t*>(image.base + entry->UnwindData);
        if (!readable(unwind, 4)) break;

        const uint8_t flags = unwind[0] >> 3;
        if ((flags & kChainInfo) == 0) break;

        const uint8_t codes = unwind[2];
        const size_t paddedCodes = (codes + 1u) & ~1u;
        const auto* chained =
            reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + 4 + paddedCodes * 2);
        if (!readable(chained, sizeof(RUNTIME_FUNCTION))) break;
        entry = chained;
    }

    return image.base + entry->BeginAddress;
}

uintptr_t findReferenceTo(uintptr_t target, const ModuleRange& range) {
    if (target == 0 || !range.valid() || range.size < 5) return 0;

    const auto* begin = reinterpret_cast<const uint8_t*>(range.base);
    const size_t last = range.size - 4;

    // Every rip-relative operand is a signed 32-bit distance from the end of its four
    // bytes, whatever instruction it belongs to. Read unaligned at every offset.
    for (size_t offset = 0; offset < last; ++offset) {
        int32_t displacement = 0;
        std::memcpy(&displacement, begin + offset, sizeof(displacement));

        const uintptr_t next = range.base + offset + 4;
        if (next + static_cast<uintptr_t>(static_cast<intptr_t>(displacement)) == target) {
            return range.base + offset;
        }
    }
    return 0;
}

uintptr_t findText(std::string_view text) {
    const ModuleRange& data = gameRodata();
    if (text.empty() || !data.valid() || text.size() + 1 > data.size) return 0;

    const auto* begin = reinterpret_cast<const uint8_t*>(data.base);
    const auto* end = begin + data.size;
    const auto first = static_cast<uint8_t>(text.front());

    const uint8_t* cursor = begin;
    while (cursor < end) {
        const void* hit = std::memchr(cursor, first, static_cast<size_t>(end - cursor));
        if (!hit) return 0;

        const auto* candidate = static_cast<const uint8_t*>(hit);
        if (static_cast<size_t>(end - candidate) > text.size() &&
            std::memcmp(candidate, text.data(), text.size()) == 0 &&
            candidate[text.size()] == 0 && (candidate == begin || candidate[-1] == 0)) {
            return reinterpret_cast<uintptr_t>(candidate);
        }
        cursor = candidate + 1;
    }
    return 0;
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
            Log::error(kLog, "malformed pattern '{}' at index {}", ida, index);
            bytes_.clear();
            mask_.clear();
            return;
        }

        const int low = hexValue(ida[index + 1]);
        if (low < 0) {
            Log::error(kLog, "malformed pattern '{}' at index {}", ida, index + 1);
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

uintptr_t findByVtable(uintptr_t vtable, const std::function<bool(uintptr_t)>& accept) {
    if (vtable == 0) return 0;

    SYSTEM_INFO system{};
    GetSystemInfo(&system);

    auto address = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const auto limit = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);

    // One buffer for the whole walk. A megabyte is small enough to stay in cache and
    // large enough that the syscall per chunk disappears against the search.
    constexpr size_t kScanChunkBytes = 1u << 20;
    std::vector<uintptr_t> chunk(kScanChunkBytes / sizeof(uintptr_t));

    MEMORY_BASIC_INFORMATION info{};
    size_t regionsWalked = 0;
    size_t regionsRead = 0;
    size_t bytesRead = 0;
    size_t matches = 0;
    size_t rejected = 0;

    while (address < limit && VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const size_t size = info.RegionSize;
        if (size == 0) break;

        ++regionsWalked;

        // Committed and writable is the whole test. An earlier version also demanded
        // MEM_PRIVATE and exactly PAGE_READWRITE, which found nothing at all under
        // Wine: it backs its heap with mapped files, so the object sat in a region
        // reported as MEM_MAPPED and was never looked at.
        constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY;
        const bool usable = info.State == MEM_COMMIT && (info.Protect & kWritable) != 0 &&
                            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;

        if (usable) {
            ++regionsRead;
            bytesRead += size;

            // Copied out a chunk at a time rather than walked in place. VirtualQuery
            // said this region was committed, but the answer is already stale by the
            // time the walk reaches the far end of it — and joining a server is when
            // the game unmaps the most. Every crash report from that was a read of the
            // first byte of a page that had just gone.
            for (size_t done = 0; done < size; done += kScanChunkBytes) {
                const size_t want = std::min(kScanChunkBytes, size - done);

                SIZE_T got = 0;
                const BOOL whole =
                    ReadProcessMemory(GetCurrentProcess(),
                                      reinterpret_cast<const void*>(base + done), chunk.data(),
                                      want, &got);

                // A partial read is still worth searching: the bytes that arrived were
                // there. Nothing at all means the region went away under us, which is
                // a reason to move on rather than to stop.
                if (!whole && got == 0) continue;

                const size_t count = got / sizeof(uintptr_t);
                for (size_t i = 0; i < count; ++i) {
                    if (chunk[i] != vtable) continue;

                    ++matches;
                    const uintptr_t candidate = base + done + i * sizeof(uintptr_t);
                    if (!accept || accept(candidate)) {
                        Log::debug(kLog, "vtable scan: {} region(s), {} MB, {} match(es), taken",
                                   regionsRead, bytesRead / (1024 * 1024), matches);
                        return candidate;
                    }
                    ++rejected;
                }
            }
        }

        address = base + size;
    }

    // Which of the three ways it failed is the whole diagnosis, and guessing it from
    // the outside is what cost a day. Said plainly the first time, quietly after that,
    // because the scan repeats on a timer and this must not become the log.
    static bool announced = false;
    const size_t megabytes = bytesRead / (1024 * 1024);

    if (!announced) {
        announced = true;
        Log::warn(kLog,
                  "vtable scan found nothing: {} region(s) walked, {} read ({} MB), "
                  "{} match(es), {} rejected by the check",
                  regionsWalked, regionsRead, megabytes, matches, rejected);
    } else {
        Log::debug(kLog,
                   "vtable scan found nothing: {} region(s) walked, {} read ({} MB), "
                   "{} match(es), {} rejected by the check",
                   regionsWalked, regionsRead, megabytes, matches, rejected);
    }
    return 0;
}

uintptr_t findByVtable(uintptr_t vtable) { return findByVtable(vtable, {}); }

// ReadProcessMemory on our own process is a plain copy, except that the kernel
// validates the range instead of the CPU faulting on it. That difference is the whole
// point: a page the game unmapped between a check and a read takes the game down when
// dereferenced, and merely returns false here.
bool copy(void* destination, uintptr_t source, size_t bytes) {
    if (!destination || bytes == 0) return false;
    if (source < 0x10000) return false;

    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(source), destination,
                           bytes, &read)) {
        return false;
    }
    return read == bytes;
}

bool readable(const void* pointer, size_t size) {
    if (!pointer) return false;

    // Null page: nothing valid lives there, and this check alone catches most
    // use-after-free reads.
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

std::string readString(uintptr_t address) {
    if (!address) return {};

    const auto size = read<uint64_t>(address + 16);
    const auto capacity = read<uint64_t>(address + 24);

    if (size == 0 || size > 0x10000 || size > capacity) return {};

    // Copied out rather than constructed over the game's own bytes. A string the game
    // frees between the check and the copy is the same fault the vtable scan used to
    // take, only narrower — and a name or a server address is read every frame.
    std::string text(static_cast<size_t>(size), '\0');

    if (capacity > 15) {
        const auto data = read<uintptr_t>(address);
        if (!copy(text.data(), data, text.size())) return {};
        return text;
    }

    if (!copy(text.data(), address, text.size())) return {};
    return text;
}

GameString::GameString(std::string_view text) {
    layout_.size = text.size();

    if (text.size() <= 15) {
        std::memcpy(layout_.buffer, text.data(), text.size());
        layout_.capacity = 15;
        return;
    }

    heap_ = new char[text.size() + 1];
    std::memcpy(heap_, text.data(), text.size());
    heap_[text.size()] = '\0';

    std::memcpy(layout_.buffer, &heap_, sizeof(heap_));
    layout_.capacity = text.size();
}

GameString::~GameString() { delete[] heap_; }

namespace {

std::atomic<uintptr_t> g_allocatorGlobal{0};

using AllocateFn = void*(__fastcall*)(void* self, uint64_t bytes);
using FreeFn = void(__fastcall*)(void* self, void* block);

// MSVC hands out a block of a page or more aligned to 32 bytes, and keeps the address
// the allocator really returned in the eight bytes in front of it.
constexpr uint64_t kBigBlock = 0x1000;
constexpr uint64_t kBigAlignment = 32;
constexpr uint64_t kBigOverhead = kBigAlignment + sizeof(void*) - 1;

void* gameAllocator() {
    const uintptr_t global = g_allocatorGlobal.load(std::memory_order_acquire);
    if (global == 0) return nullptr;

    const auto object = read<uintptr_t>(global);
    if (object == 0 || !gameModule().contains(read<uintptr_t>(object))) return nullptr;
    return reinterpret_cast<void*>(object);
}

template <typename Fn>
Fn allocatorSlot(void* allocator, uintptr_t slot) {
    const auto vtable = read<uintptr_t>(reinterpret_cast<uintptr_t>(allocator));
    const auto entry = read<uintptr_t>(vtable + slot * sizeof(void*));
    return gameText().contains(entry) ? reinterpret_cast<Fn>(entry) : nullptr;
}

}

void setGameAllocator(uintptr_t global) {
    g_allocatorGlobal.store(global, std::memory_order_release);
}

bool gameHeapReady() { return gameAllocator() != nullptr; }

bool assignGameString(uintptr_t address, std::string_view text) {
    void* allocator = gameAllocator();
    if (allocator == nullptr || !readable(reinterpret_cast<const void*>(address), 32)) return false;

    const auto allocate = allocatorSlot<AllocateFn>(allocator, 1);
    const auto release = allocatorSlot<FreeFn>(allocator, 2);
    if (allocate == nullptr || release == nullptr) return false;

    const uint64_t size = text.size();
    uint64_t capacity = 15;
    char* buffer = nullptr;

    // The new block first: if the heap says no, the old text is still where it was.
    if (size > 15) {
        capacity = size | 0xf;
        if (capacity + 1 >= kBigBlock) {
            auto* raw = static_cast<char*>(allocate(allocator, capacity + 1 + kBigOverhead));
            if (raw == nullptr) return false;
            const uintptr_t aligned =
                (reinterpret_cast<uintptr_t>(raw) + kBigOverhead) & ~(kBigAlignment - 1);
            reinterpret_cast<char**>(aligned)[-1] = raw;
            buffer = reinterpret_cast<char*>(aligned);
        } else {
            buffer = static_cast<char*>(allocate(allocator, capacity + 1));
            if (buffer == nullptr) return false;
        }
        std::memcpy(buffer, text.data(), size);
        buffer[size] = '\0';
    }

    // Then the old one goes back where it came from.
    if (const auto previous = read<uint64_t>(address + 24); previous > 15) {
        auto block = read<uintptr_t>(address);
        if (previous + 1 >= kBigBlock) block = read<uintptr_t>(block - sizeof(void*));
        if (block != 0) release(allocator, reinterpret_cast<void*>(block));
    }

    auto* layout = reinterpret_cast<char*>(address);
    if (buffer != nullptr) {
        std::memcpy(layout, &buffer, sizeof(buffer));
    } else {
        std::memcpy(layout, text.data(), size);
        layout[size] = '\0';
    }
    std::memcpy(layout + 16, &size, sizeof(size));
    std::memcpy(layout + 24, &capacity, sizeof(capacity));
    return true;
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
