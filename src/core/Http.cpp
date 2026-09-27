#include "Http.hpp"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <optional>
#include <utility>
#include <vector>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx::http {
namespace {

constexpr const char* kLog = "Http";
constexpr DWORD kChunk = 256 * 1024;

// Every handle here has exactly one owner and several ways out of the function it was
// opened in, which is one closing call per exit and one forgotten sooner or later.
class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET value) : value_(value) {}
    ~Handle() { if (value_) WinHttpCloseHandle(value_); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_) WinHttpCloseHandle(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_ = nullptr;
};

struct Target {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

// The archive hands out plain http:// links to the Xbox asset servers while GitHub is
// https, so the scheme has to come from the address rather than from an assumption.
std::optional<Target> crack(std::string_view url) {
    const std::wstring wide = strings::toUtf16(url);

    std::wstring host(256, L'\0');
    std::wstring path(2048, L'\0');
    std::wstring query(2048, L'\0');

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host.data();
    parts.dwHostNameLength = static_cast<DWORD>(host.size() - 1);
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size() - 1);
    parts.lpszExtraInfo = query.data();
    parts.dwExtraInfoLength = static_cast<DWORD>(query.size() - 1);

    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return std::nullopt;

    Target target;
    target.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    target.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    target.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    target.port = parts.nPort;
    target.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    if (target.host.empty()) return std::nullopt;
    if (target.path.empty()) target.path = L"/";

    return target;
}

Handle openSession() {
    Handle session(WinHttpOpen(L"Velyx", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));

    // A download that stalls has to end as an error rather than as a progress bar that
    // never moves again; the read timeout is the one that decides that.
    if (session) WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 60000);
    return session;
}

DWORD statusOf(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

uint64_t lengthOf(HINTERNET request) {
    wchar_t value[32]{};
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                             value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return _wcstoui64(value, nullptr, 10);
}

// Opens the request and reads the response headers. `extraHeaders` carries the resume
// range when there is one. The handles come back alive so the body can be read from
// them; they die with the caller's scope.
bool send(const Target& target, const std::wstring& extraHeaders, Handle& connection,
          Handle& request, const Handle& session, std::string* error) {
    connection = Handle(WinHttpConnect(session.get(), target.host.c_str(), target.port, 0));
    if (!connection) {
        if (error) *error = "connection to " + strings::toUtf8(target.host) + " refused";
        return false;
    }

    const DWORD flags = (target.secure ? WINHTTP_FLAG_SECURE : 0u) | WINHTTP_FLAG_REFRESH;
    request = Handle(WinHttpOpenRequest(connection.get(), L"GET", target.path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        if (error) *error = "the request could not be opened";
        return false;
    }

    const wchar_t* headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                  : extraHeaders.c_str();
    const DWORD headerLength = extraHeaders.empty() ? 0 : static_cast<DWORD>(-1);

    if (!WinHttpSendRequest(request.get(), headers, headerLength, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        if (error) *error = strings::systemError(GetLastError());
        return false;
    }

    return true;
}

} // namespace

std::string get(std::string_view url, std::string* error) {
    const auto target = crack(url);
    if (!target) {
        if (error) *error = "unreadable address";
        return {};
    }

    const Handle session = openSession();
    if (!session) {
        if (error) *error = "WinHttpOpen failed";
        return {};
    }

    Handle connection;
    Handle request;
    if (!send(*target, {}, connection, request, session, error)) return {};

    const DWORD status = statusOf(request.get());
    if (status != 200) {
        if (error) *error = "the server answered " + std::to_string(status);
        return {};
    }

    std::string body;
    std::vector<char> buffer(kChunk);

    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                             &read)) {
            if (error) *error = strings::systemError(GetLastError());
            return {};
        }
        if (read == 0) break;
        body.append(buffer.data(), read);
    }

    return body;
}

bool download(std::string_view url, const std::filesystem::path& target,
              const ProgressFn& onProgress, std::string* error) {
    const auto address = crack(url);
    if (!address) {
        if (error) *error = "unreadable address";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);

    auto partial = target;
    partial += ".part";

    // What is already on disk is what we ask the server to skip. A server that ignores
    // the range answers 200 with the whole file, and the count below starts again at
    // zero rather than appending a second copy to the first.
    uint64_t already = std::filesystem::exists(partial, ec)
                           ? static_cast<uint64_t>(std::filesystem::file_size(partial, ec))
                           : 0;
    if (ec) already = 0;

    const Handle session = openSession();
    if (!session) {
        if (error) *error = "WinHttpOpen failed";
        return false;
    }

    const std::wstring range =
        already > 0 ? L"Range: bytes=" + std::to_wstring(already) + L"-\r\n" : L"";

    Handle connection;
    Handle request;
    if (!send(*address, range, connection, request, session, error)) return false;

    const DWORD status = statusOf(request.get());
    if (status != 200 && status != 206) {
        if (error) *error = "the server answered " + std::to_string(status);
        return false;
    }

    if (status == 200 && already > 0) {
        Log::info(kLog, "{} does not resume, taking the file from the start",
                  strings::toUtf8(address->host));
        already = 0;
    }

    const uint64_t total = lengthOf(request.get()) + already;

    std::ofstream file;
    file.open(partial, already > 0 ? (std::ios::binary | std::ios::app)
                                   : (std::ios::binary | std::ios::trunc));
    if (!file) {
        if (error) *error = "nothing can be written to " + partial.string();
        return false;
    }

    std::vector<char> buffer(kChunk);
    uint64_t received = already;

    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                             &read)) {
            if (error) *error = strings::systemError(GetLastError());
            return false;
        }
        if (read == 0) break;

        file.write(buffer.data(), read);
        if (!file) {
            if (error) *error = "the disk stopped taking " + partial.string();
            return false;
        }

        received += read;

        if (onProgress && !onProgress(received, total)) {
            if (error) *error = "download cancelled";
            return false;
        }
    }

    file.close();

    if (total != 0 && received < total) {
        if (error) *error = "the connection ended early (" + std::to_string(received) + " of " +
                            std::to_string(total) + " bytes)";
        return false;
    }

    std::filesystem::remove(target, ec);
    std::filesystem::rename(partial, target, ec);
    if (ec) {
        if (error) *error = "the file could not be moved into place: " + ec.message();
        return false;
    }

    Log::info(kLog, "downloaded {} ({} bytes)", target.filename().string(), received);
    return true;
}

} // namespace velyx::http
