#include "Updates.hpp"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

#include <json/json.hpp>
#include <velyx/Version.hpp>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx::updates {
namespace {

constexpr const char* kLog = "Updates";
constexpr const wchar_t* kHost = L"api.github.com";
constexpr const wchar_t* kPath = L"/repos/Wyze3306/Velyx/releases?per_page=20";
constexpr const char* kReleasesPage = "https://github.com/Wyze3306/Velyx/releases";

std::mutex g_mutex;
Status g_status;
std::thread g_worker;

void setStatus(Status value) {
    const std::lock_guard lock(g_mutex);
    g_status = std::move(value);
}

std::string fetch(std::string* error) {
    HINTERNET session = WinHttpOpen(L"Velyx", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (error) *error = "WinHttpOpen failed";
        return {};
    }

    HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        if (error) *error = "WinHttpConnect failed";
        return {};
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"GET", kPath, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if (error) *error = "WinHttpOpenRequest failed";
        return {};
    }

    const std::wstring headers = L"Accept: application/vnd.github+json\r\n"
                                 L"User-Agent: Velyx\r\n";

    std::string body;
    if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            body.append(chunk.data(), read);
        }
    } else if (error) {
        *error = std::format("request failed ({})", GetLastError());
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return body;
}

std::vector<int> parts(const std::string& version) {
    std::string cleaned = version;
    if (!cleaned.empty() && (cleaned.front() == 'v' || cleaned.front() == 'V')) {
        cleaned.erase(cleaned.begin());
    }

    std::vector<int> numbers;
    for (const std::string& piece : strings::split(cleaned, '.')) {
        int value = 0;
        for (const char c : piece) {
            if (!std::isdigit(static_cast<unsigned char>(c))) break;
            value = value * 10 + (c - '0');
        }
        numbers.push_back(value);
    }
    return numbers;
}

} // namespace

int compareVersions(const std::string& left, const std::string& right) {
    const std::vector<int> a = parts(left);
    const std::vector<int> b = parts(right);

    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

void check(const std::string& channel) {
    {
        const std::lock_guard lock(g_mutex);
        if (g_status.checking) return;
        g_status = Status{};
        g_status.checking = true;
    }

    if (g_worker.joinable()) g_worker.join();

    g_worker = std::thread([channel] {
        Status result;
        result.checking = false;
        result.checked = true;

        std::string error;
        const std::string body = fetch(&error);

        if (body.empty()) {
            result.error = error.empty() ? "empty response" : error;
            Log::warn(kLog, "update check failed: {}", result.error);
            setStatus(std::move(result));
            return;
        }

        nlohmann::json document;
        try {
            document = nlohmann::json::parse(body);
        } catch (const std::exception& e) {
            result.error = e.what();
            setStatus(std::move(result));
            return;
        }

        if (!document.is_array()) {
            result.error = "unexpected response shape";
            setStatus(std::move(result));
            return;
        }

        const bool stableOnly = channel == "stable";

        for (const auto& entry : document) {
            if (entry.value("draft", false)) continue;

            const bool prerelease = entry.value("prerelease", false);
            if (stableOnly && prerelease) continue;

            Release release;
            release.tag = entry.value("tag_name", std::string{});
            release.name = entry.value("name", release.tag);
            release.notes = entry.value("body", std::string{});
            release.url = entry.value("html_url", std::string(kReleasesPage));
            release.publishedAt = entry.value("published_at", std::string{});
            release.prerelease = prerelease;

            if (release.tag.empty()) continue;

            if (result.latest.tag.empty() ||
                compareVersions(result.latest.tag, release.tag) < 0) {
                result.latest = release;
            }
        }

        if (!result.latest.tag.empty()) {
            result.updateAvailable = compareVersions(version::kString, result.latest.tag) < 0;
            Log::info(kLog, "latest release is {} (running {}){}", result.latest.tag,
                      version::kString, result.updateAvailable ? ", update available" : "");
        } else {
            Log::info(kLog, "no release published yet");
        }

        setStatus(std::move(result));
    });
}

void shutdown() {
    if (g_worker.joinable()) g_worker.join();
}

const Status& status() {
    const std::lock_guard lock(g_mutex);
    return g_status;
}

bool openReleasePage() {
    std::string target;
    {
        const std::lock_guard lock(g_mutex);
        target = g_status.latest.url.empty() ? kReleasesPage : g_status.latest.url;
    }

    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
                                                   strings::toUtf16(target).c_str(), nullptr,
                                                   nullptr, SW_SHOWNORMAL)) > 32;
}

} // namespace velyx::updates
