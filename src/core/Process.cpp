#include "Process.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <aclapi.h>
#include <sddl.h>

#include <chrono>
#include <format>
#include <thread>

#include "Log.hpp"
#include "Strings.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Process";

struct HandleGuard {
    HANDLE handle = nullptr;
    explicit HandleGuard(HANDLE h) : handle(h) {}
    ~HandleGuard() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    [[nodiscard]] bool valid() const { return handle && handle != INVALID_HANDLE_VALUE; }
};

std::string lastErrorMessage(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    std::string message = size && buffer ? std::string(buffer, size) : std::string("unknown error");
    if (buffer) LocalFree(buffer);

    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
    return std::format("{} (0x{:08X})", message, static_cast<unsigned>(code));
}

}

std::vector<ProcessInfo> Process::enumerate() {
    std::vector<ProcessInfo> processes;

    const HandleGuard snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return processes;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot.handle, &entry)) return processes;

    do {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.name = strings::toUtf8(entry.szExeFile);
        processes.push_back(std::move(info));
    } while (Process32NextW(snapshot.handle, &entry));

    return processes;
}

std::vector<ProcessInfo> Process::findByName(std::string_view executableName) {
    std::vector<ProcessInfo> matches;

    for (auto& process : enumerate()) {
        if (strings::toLower(process.name) == strings::toLower(executableName)) {
            matches.push_back(std::move(process));
        }
    }

    return matches;
}

bool Process::isRunning(uint32_t pid) {
    const HandleGuard handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!handle.valid()) return false;

    DWORD exitCode = 0;
    return GetExitCodeProcess(handle.handle, &exitCode) && exitCode == STILL_ACTIVE;
}

bool Process::terminate(uint32_t pid) {
    const HandleGuard handle(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
    if (!handle.valid()) return false;
    return TerminateProcess(handle.handle, 0) != 0;
}

uintptr_t Process::moduleBase(uint32_t pid, std::string_view moduleName) {
    const HandleGuard snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot.valid()) return 0;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Module32FirstW(snapshot.handle, &entry)) return 0;

    const std::string wanted = strings::toLower(moduleName);
    do {
        if (strings::toLower(strings::toUtf8(entry.szModule)) == wanted) {
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        }
    } while (Module32NextW(snapshot.handle, &entry));

    return 0;
}

bool Process::isModuleLoaded(uint32_t pid, std::string_view moduleName) {
    return moduleBase(pid, moduleName) != 0;
}

bool Process::grantAppContainerAccess(const std::filesystem::path& target) {

    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) {
        Log::warn(kLog, "could not build the AppContainer SID: {}",
                  lastErrorMessage(GetLastError()));
        return false;
    }

    PACL existing = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto path = target.wstring();

    DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, &existing, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        LocalFree(sid);
        Log::warn(kLog, "GetNamedSecurityInfo failed for {}: {}",
                  target.string(), lastErrorMessage(status));
        return false;
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);

    PACL updated = nullptr;
    status = SetEntriesInAclW(1, &access, existing, &updated);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, updated, nullptr);
    }

    if (updated) LocalFree(updated);
    if (descriptor) LocalFree(descriptor);
    LocalFree(sid);

    if (status != ERROR_SUCCESS) {
        Log::warn(kLog, "could not grant AppContainer access to {}: {}",
                  target.string(), lastErrorMessage(status));
        return false;
    }

    return true;
}

bool Process::injectLibrary(uint32_t pid, const std::filesystem::path& dll, std::string* error) {
    const auto fail = [&](std::string message) {
        Log::error(kLog, "injection into {} failed: {}", pid, message);
        if (error) *error = std::move(message);
        return false;
    };

    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        return fail(std::format("{} does not exist", dll.string()));
    }

    // Bedrock runs in an AppContainer: without this ACE, LoadLibraryW returns 0.
    grantAppContainerAccess(dll);

    const HandleGuard process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid));
    if (!process.valid()) {
        return fail(std::format("OpenProcess: {}", lastErrorMessage(GetLastError())));
    }

    const std::wstring wide = std::filesystem::absolute(dll, ec).wstring();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(process.handle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        return fail(std::format("VirtualAllocEx: {}", lastErrorMessage(GetLastError())));
    }

    if (!WriteProcessMemory(process.handle, remote, wide.c_str(), bytes, nullptr)) {
        const auto message = lastErrorMessage(GetLastError());
        VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
        return fail(std::format("WriteProcessMemory: {}", message));
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));

    const HandleGuard thread(
        CreateRemoteThread(process.handle, nullptr, 0, loadLibrary, remote, 0, nullptr));
    if (!thread.valid()) {
        const auto message = lastErrorMessage(GetLastError());
        VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);
        return fail(std::format("CreateRemoteThread: {}", message));
    }

    WaitForSingleObject(thread.handle, 15000);

    DWORD remoteModule = 0;
    GetExitCodeThread(thread.handle, &remoteModule);
    VirtualFreeEx(process.handle, remote, 0, MEM_RELEASE);

    if (remoteModule == 0) {
        return fail("LoadLibraryW returned NULL inside the game process");
    }

    Log::info(kLog, "injected {} into pid {}", dll.filename().string(), pid);
    return true;
}

uint32_t Process::waitForProcess(std::string_view executableName, int timeoutMs, uint32_t ignorePid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& process : findByName(executableName)) {
            if (process.pid != ignorePid) return process.pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    return 0;
}

}
