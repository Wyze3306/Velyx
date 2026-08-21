#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace velyx::resources {

struct Blob {
    std::wstring name;
    const void* data = nullptr;
    size_t size = 0;
};

std::vector<Blob> findAll(HMODULE module, const wchar_t* type);

Blob find(HMODULE module, const wchar_t* type, const wchar_t* name);

/// Writes the files bundled into `module` under `root`, following the payload
/// manifest linked in beside them. A file already there with the same size is
/// left alone. Returns how many files were written.
int unpack(HMODULE module, const std::filesystem::path& root);

} // namespace velyx::resources
