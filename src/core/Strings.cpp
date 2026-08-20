#include "Strings.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace velyx::strings {
namespace {

char lowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool isWordBoundary(std::string_view text, size_t index) {
    if (index == 0) return true;
    const char previous = text[index - 1];
    if (previous == ' ' || previous == '_' || previous == '-' || previous == '.') return true;

    return std::islower(static_cast<unsigned char>(previous)) &&
           std::isupper(static_cast<unsigned char>(text[index]));
}

}

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring toUtf16(std::string_view text) {
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return {};

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::string toLower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), lowerAscii);
    return result;
}

std::string toUpper(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    const auto notSpace = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };

    while (!text.empty() && !notSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && !notSpace(text.back())) text.remove_suffix(1);
    return text;
}

std::vector<std::string> split(std::string_view text, char delimiter, bool keepEmpty) {
    std::vector<std::string> parts;
    size_t start = 0;

    while (true) {
        const size_t end = text.find(delimiter, start);
        std::string_view piece = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (keepEmpty || !piece.empty()) parts.emplace_back(piece);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) result.append(separator);
        result.append(parts[i]);
    }
    return result;
}

bool startsWith(std::string_view text, std::string_view prefix, bool caseSensitive) {
    if (prefix.size() > text.size()) return false;
    if (caseSensitive) return text.compare(0, prefix.size(), prefix) == 0;

    for (size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(prefix[i])) return false;
    }
    return true;
}

bool endsWith(std::string_view text, std::string_view suffix, bool caseSensitive) {
    if (suffix.size() > text.size()) return false;
    return startsWith(text.substr(text.size() - suffix.size()), suffix, caseSensitive);
}

bool containsInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (startsWith(haystack.substr(i), needle, false)) return true;
    }
    return false;
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(text);

    std::string result;
    result.reserve(text.size());

    size_t position = 0;
    while (true) {
        const size_t found = text.find(from, position);
        if (found == std::string_view::npos) {
            result.append(text.substr(position));
            break;
        }
        result.append(text.substr(position, found - position));
        result.append(to);
        position = found + from.size();
    }

    return result;
}

std::optional<int> fuzzyScore(std::string_view query, std::string_view target) {
    if (query.empty()) return 0;
    if (query.size() > target.size()) return std::nullopt;

    int score = 0;
    size_t targetIndex = 0;
    size_t previousMatch = std::string_view::npos;

    for (const char rawQueryChar : query) {
        const char queryChar = lowerAscii(rawQueryChar);

        bool matched = false;
        while (targetIndex < target.size()) {
            if (lowerAscii(target[targetIndex]) == queryChar) {
                matched = true;
                break;
            }
            ++targetIndex;
        }
        if (!matched) return std::nullopt;

        score += 10;
        if (targetIndex == 0) score += 25;
        if (isWordBoundary(target, targetIndex)) score += 15;
        if (previousMatch != std::string_view::npos && targetIndex == previousMatch + 1) {
            score += 12;
        }

        previousMatch = targetIndex;
        ++targetIndex;
    }

    score -= static_cast<int>(target.size() - query.size());
    return score;
}

std::string formatDuration(long long seconds, bool compact) {
    if (seconds < 0) seconds = 0;

    const long long hours = seconds / 3600;
    const long long minutes = (seconds % 3600) / 60;
    const long long secs = seconds % 60;

    char buffer[64]{};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), compact ? "%lldh %02lldm" : "%lld h %02lld min",
                      hours, minutes);
    } else if (minutes > 0) {
        std::snprintf(buffer, sizeof(buffer), compact ? "%lldm %02llds" : "%lld min %02lld s",
                      minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), compact ? "%llds" : "%lld s", secs);
    }
    return buffer;
}

std::string formatThousands(long long value) {
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value);

    std::string result;
    result.reserve(digits.size() + digits.size() / 3 + 1);

    const size_t leading = digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i - leading) % 3 == 0) result.push_back(' ');
        result.push_back(digits[i]);
    }

    return negative ? "-" + result : result;
}

std::string formatFloat(double value, int decimals) {
    char format[16]{};
    std::snprintf(format, sizeof(format), "%%.%df", decimals < 0 ? 0 : decimals);

    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

std::string stripFormatting(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {

        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        if (byte == 0xA7 && i + 1 < text.size()) {
            ++i;
            continue;
        }
        result.push_back(text[i]);
    }

    return result;
}

std::string base64Encode(std::string_view data) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string result;
    result.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t byte0 = static_cast<unsigned char>(data[i]);
        const uint32_t byte1 = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const uint32_t byte2 = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        const uint32_t triple = (byte0 << 16) | (byte1 << 8) | byte2;

        result.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        result.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size()) result.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        if (i + 2 < data.size()) result.push_back(kAlphabet[triple & 0x3F]);
    }

    return result;
}

std::optional<std::string> base64Decode(std::string_view encoded) {
    const auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };

    std::string result;
    result.reserve(encoded.size() / 4 * 3);

    uint32_t buffer = 0;
    int bits = 0;

    for (const char c : encoded) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;

        const int value = valueOf(c);
        if (value < 0) return std::nullopt;

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }

    return result;
}

std::string hashId(std::string_view text) {

    uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }

    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

}
