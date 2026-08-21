#include "Resources.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx::resources {
namespace {

constexpr const char* kLog = "Resources";
constexpr const wchar_t* kFileType = L"VELYXFILE";
constexpr const wchar_t* kMapType = L"VELYXMAP";
constexpr const wchar_t* kMapName = L"VELYX_MANIFEST";

struct EnumState {
    std::vector<Blob>* out = nullptr;
};

Blob load(HMODULE module, const wchar_t* type, const wchar_t* name) {
    const HRSRC handle = FindResourceW(module, name, type);
    if (!handle) return {};

    const HGLOBAL loaded = LoadResource(module, handle);
    if (!loaded) return {};

    Blob blob;
    blob.name = IS_INTRESOURCE(name) ? std::to_wstring(reinterpret_cast<uintptr_t>(name)) : name;
    blob.data = LockResource(loaded);
    blob.size = SizeofResource(module, handle);
    return blob;
}

BOOL CALLBACK onName(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR parameter) {
    auto* state = reinterpret_cast<EnumState*>(parameter);

    Blob blob = load(module, type, name);
    if (blob.data && blob.size > 0) state->out->push_back(std::move(blob));

    return TRUE;
}

bool write(const Blob& blob, const std::filesystem::path& destination) {
    std::error_code ec;
    if (std::filesystem::exists(destination, ec) &&
        std::filesystem::file_size(destination, ec) == blob.size) {
        return false;
    }

    std::filesystem::create_directories(destination.parent_path(), ec);

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        Log::warn(kLog, "could not write {}", destination.string());
        return false;
    }

    out.write(static_cast<const char*>(blob.data), static_cast<std::streamsize>(blob.size));
    return out.good();
}

} // namespace

std::vector<Blob> findAll(HMODULE module, const wchar_t* type) {
    std::vector<Blob> blobs;

    EnumState state{&blobs};
    EnumResourceNamesW(module, type, &onName, reinterpret_cast<LONG_PTR>(&state));

    return blobs;
}

Blob find(HMODULE module, const wchar_t* type, const wchar_t* name) {
    return load(module, type, name);
}

int unpack(HMODULE module, const std::filesystem::path& root) {
    const Blob manifest = find(module, kMapType, kMapName);
    if (!manifest.data || manifest.size == 0) return 0;

    int written = 0;
    std::string_view remaining(static_cast<const char*>(manifest.data), manifest.size);

    while (!remaining.empty()) {
        std::string_view line = remaining.substr(0, remaining.find('\n'));
        remaining.remove_prefix(std::min(line.size() + 1, remaining.size()));

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const size_t separator = line.find('=');
        if (separator == std::string_view::npos) continue;

        const std::wstring name = strings::toUtf16(line.substr(0, separator));
        const std::string_view relative = line.substr(separator + 1);
        if (relative.empty()) continue;

        const Blob blob = find(module, kFileType, name.c_str());
        if (!blob.data || blob.size == 0) {
            Log::warn(kLog, "payload entry {} is missing", std::string(relative));
            continue;
        }

        if (write(blob, root / std::filesystem::path(relative))) ++written;
    }

    if (written > 0) Log::info(kLog, "unpacked {} bundled file(s) into {}", written, root.string());
    return written;
}

} // namespace velyx::resources
