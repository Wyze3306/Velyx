#include "SystemTuning.hpp"

#include <windows.h>
#include <powrprof.h>

#include <format>
#include <string>

#include "core/Log.hpp"
#include "core/Strings.hpp"

namespace velyx::tuning {
namespace {

constexpr const char* kLog = "Tuning";

// Two keys rather than one: the first is what the Game bar's own switch writes, the
// second is what the capture service actually reads. Turning off one and not the
// other is the reason the setting has a reputation for not sticking.
constexpr const wchar_t* kGameConfigStore = L"System\\GameConfigStore";
constexpr const wchar_t* kGameDvrValue = L"GameDVR_Enabled";
constexpr const wchar_t* kCapture = L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR";
constexpr const wchar_t* kCaptureValue = L"AppCaptureEnabled";

constexpr const wchar_t* kGpuPreferences = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

// The scheme Windows ships as High performance. Its GUID is fixed across editions.
constexpr GUID kHighPerformance = {
    0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};

bool readDword(const wchar_t* key, const wchar_t* value, DWORD& out) {
    DWORD size = sizeof(out);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, &type,
                                        &out, &size);
    return status == ERROR_SUCCESS;
}

bool writeDword(const wchar_t* key, const wchar_t* value, DWORD data, std::string* error) {
    const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, key, value, REG_DWORD, &data,
                                           sizeof(data));
    if (status == ERROR_SUCCESS) return true;

    if (error) *error = std::format("Windows refused the change ({})", static_cast<int>(status));
    return false;
}

bool readString(const wchar_t* key, const std::wstring& value, std::wstring& out) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, value.c_str(), RRF_RT_REG_SZ,
                                        &type, buffer, &size);
    if (status != ERROR_SUCCESS) return false;

    out.assign(buffer);
    return true;
}

// The power API lives in a DLL the launcher does not otherwise need, and an edition
// without it should cost the setting rather than the launcher.
using GetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
using SetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);

HMODULE powerLibrary() {
    static const HMODULE library = LoadLibraryW(L"powrprof.dll");
    return library;
}

template <typename Fn>
Fn powerEntry(const char* symbol) {
    const HMODULE library = powerLibrary();
    if (library == nullptr) return nullptr;
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(library, symbol)));
}

GUID g_previousScheme{};
bool g_holding = false;

}

State backgroundRecording() {
    DWORD enabled = 0;

    // Either key saying off is off: the capture will not run without both.
    if (readDword(kCapture, kCaptureValue, enabled) && enabled == 0) return State::Off;
    if (readDword(kGameConfigStore, kGameDvrValue, enabled)) {
        return enabled == 0 ? State::Off : State::On;
    }

    return State::Unknown;
}

bool setBackgroundRecording(bool on, std::string* error) {
    const DWORD value = on ? 1 : 0;

    const bool first = writeDword(kGameConfigStore, kGameDvrValue, value, error);
    const bool second = writeDword(kCapture, kCaptureValue, value, error);

    if (first || second) {
        Log::info(kLog, "background recording {}", on ? "on" : "off");
        return true;
    }
    return false;
}

State highPerformanceGpu(const std::string& activationId) {
    if (activationId.empty()) return State::Unknown;

    std::wstring current;
    if (!readString(kGpuPreferences, strings::toUtf16(activationId), current)) return State::Off;

    return current.find(L"GpuPreference=2") != std::wstring::npos ? State::On : State::Off;
}

bool setHighPerformanceGpu(const std::string& activationId, bool on, std::string* error) {
    if (activationId.empty()) {
        if (error) *error = "this instance has no package identity yet";
        return false;
    }

    const std::wstring name = strings::toUtf16(activationId);

    if (!on) {
        // Removed rather than set back to zero, so what is left behind is what was
        // there before Velyx: no entry at all, which is what "let Windows decide"
        // looks like in this key.
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kGpuPreferences,
                                                  name.c_str());
        if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return true;

        if (error) *error = std::format("Windows refused the change ({})", static_cast<int>(status));
        return false;
    }

    const std::wstring data = L"GpuPreference=2;";
    const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, kGpuPreferences, name.c_str(),
                                           REG_SZ, data.c_str(),
                                           static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        if (error) *error = std::format("Windows refused the change ({})", static_cast<int>(status));
        return false;
    }

    Log::info(kLog, "{} pinned to the high-performance graphics card", activationId);
    return true;
}

bool powerPlanAvailable() {
    return powerEntry<GetActiveSchemeFn>("PowerGetActiveScheme") != nullptr &&
           powerEntry<SetActiveSchemeFn>("PowerSetActiveScheme") != nullptr;
}

bool holdingPowerPlan() { return g_holding; }

void holdPowerPlan(bool hold) {
    if (hold == g_holding) return;

    const auto get = powerEntry<GetActiveSchemeFn>("PowerGetActiveScheme");
    const auto set = powerEntry<SetActiveSchemeFn>("PowerSetActiveScheme");
    if (get == nullptr || set == nullptr) return;

    if (hold) {
        GUID* active = nullptr;
        if (get(nullptr, &active) != ERROR_SUCCESS || active == nullptr) return;

        g_previousScheme = *active;
        LocalFree(active);

        if (set(nullptr, &kHighPerformance) != ERROR_SUCCESS) return;

        g_holding = true;
        Log::info(kLog, "power plan held at high performance");
        return;
    }

    set(nullptr, &g_previousScheme);
    g_holding = false;
    Log::info(kLog, "power plan put back");
}

}
