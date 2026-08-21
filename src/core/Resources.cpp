#include "Resources.hpp"

#include <fstream>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx::resources {
namespace {

constexpr const char* kLog = "Resources";

struct EnumState {
    HMODULE module = nullptr;
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

} // namespace

std::vector<Blob> findAll(HMODULE module, const wchar_t* type) {
    std::vector<Blob> blobs;

    EnumState state{module, &blobs};
    EnumResourceNamesW(module, type, &onName, reinterpret_cast<LONG_PTR>(&state));

    return blobs;
}

Blob find(HMODULE module, const wchar_t* type, const wchar_t* name) {
    return load(module, type, name);
}

int materialise(HMODULE module, const wchar_t* type, const std::filesystem::path& root) {
    int written = 0;

    for (const Blob& blob : findAll(module, type)) {
        const auto destination = root / std::filesystem::path(blob.name);

        std::error_code ec;
        if (std::filesystem::exists(destination, ec) &&
            std::filesystem::file_size(destination, ec) == blob.size) {
            continue;
        }

        std::filesystem::create_directories(destination.parent_path(), ec);

        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) {
            Log::warn(kLog, "could not write {}", destination.string());
            continue;
        }

        out.write(static_cast<const char*>(blob.data), static_cast<std::streamsize>(blob.size));
        ++written;
    }

    if (written > 0) Log::info(kLog, "unpacked {} embedded file(s) into {}", written, root.string());
    return written;
}

} // namespace velyx::resources
