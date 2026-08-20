#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace velyx::screenshot {

struct Result {
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

Result capture(const std::filesystem::path& destination);

std::filesystem::path suggestedPath(std::string_view server);

struct Entry {
    std::filesystem::path path;
    std::string server;
    std::string day;
    long long sizeBytes = 0;
    long long modifiedMs = 0;
};

std::vector<Entry> gallery(size_t limit = 200);

bool revealInExplorer(const std::filesystem::path& path);

} // namespace velyx::screenshot
