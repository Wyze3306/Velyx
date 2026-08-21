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

/// Writes every resource of `type` under `root`, using each resource's name as
/// its relative path. A file already present with the same size is left alone.
/// Returns how many files were written.
int materialise(HMODULE module, const wchar_t* type, const std::filesystem::path& root);

} // namespace velyx::resources
