#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace velyx::strings {

std::string toUtf8(std::wstring_view text);
std::wstring toUtf16(std::string_view text);

std::string toLower(std::string_view text);
std::string toUpper(std::string_view text);

std::string_view trim(std::string_view text);
std::vector<std::string> split(std::string_view text, char delimiter, bool keepEmpty = false);
std::string join(const std::vector<std::string>& parts, std::string_view separator);

bool startsWith(std::string_view text, std::string_view prefix, bool caseSensitive = true);
bool endsWith(std::string_view text, std::string_view suffix, bool caseSensitive = true);
bool containsInsensitive(std::string_view haystack, std::string_view needle);

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

std::optional<int> fuzzyScore(std::string_view query, std::string_view target);

std::string formatDuration(long long seconds, bool compact = true);

std::string formatThousands(long long value);

std::string formatFloat(double value, int decimals = 1);

std::string stripFormatting(std::string_view text);

std::string hashId(std::string_view text);

std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view encoded);

constexpr uint32_t hash32(std::string_view text) {
    uint32_t hash = 2166136261u;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

}
