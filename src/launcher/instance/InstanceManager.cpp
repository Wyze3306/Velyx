#include "InstanceManager.hpp"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Process.hpp"
#include "core/Strings.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Instances";
constexpr const char* kGameExecutable = "Minecraft.Windows.exe";
constexpr const char* kStoreFile = "instances.json";

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int runPowerShell(const std::wstring& command, std::string* output) {

    std::wstring commandLine = L"powershell.exe -NoProfile -NonInteractive "
                               L"-ExecutionPolicy Bypass -Command \"" + command + L"\"";

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        return -1;
    }

    std::string collected;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        collected.append(buffer, read);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    if (output) *output = collected;
    return static_cast<int>(exitCode);
}

std::filesystem::path storeFile() { return Paths::instances() / kStoreFile; }

// An apartment belongs to a thread, not to a process, and the launcher runs its long
// operations on a worker, so anything reaching for COM has to open its own.
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned_ = SUCCEEDED(hr);
        ready_ = owned_ || hr == RPC_E_CHANGED_MODE;
    }

    ~ComApartment() {
        if (owned_) CoUninitialize();
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool ready() const { return ready_; }

private:
    bool owned_ = false;
    bool ready_ = false;
};

std::string packageNameFor(const Instance& instance) { return "Velyx." + instance.id; }

struct PackageIdentity {
    std::string familyName;
    std::string applicationId;
};

// Windows is the only authority on what an instance is called once it is registered:
// asking it keeps the stored identity from drifting out of the activation string.
std::optional<PackageIdentity> queryPackage(const std::string& packageName) {
    std::string output;
    const std::wstring command =
        L"$p = @(Get-AppxPackage -Name '" + strings::toUtf16(packageName) +
        L"')[0]; if ($p) { $a = @((Get-AppxPackageManifest -Package $p.PackageFullName)"
        L".Package.Applications.Application)[0]; $p.PackageFamilyName + '|' + $a.Id }";

    if (runPowerShell(command, &output) != 0) return std::nullopt;

    const std::string reply(strings::trim(output));
    const size_t separator = reply.find('|');
    if (separator == std::string::npos) return std::nullopt;

    PackageIdentity identity;
    identity.familyName = std::string(strings::trim(std::string_view(reply).substr(0, separator)));
    identity.applicationId =
        std::string(strings::trim(std::string_view(reply).substr(separator + 1)));

    if (identity.familyName.empty() || identity.applicationId.empty()) return std::nullopt;
    return identity;
}

// ActivateApplication is free to report no pid at all, and the id it does hand back
// usually belongs to the GDK starter process, which is alive when the call returns and
// gone a second or two later — long enough to pass a liveness check and be dead by the
// time we inject. So the pid is only kept when it really is the game; otherwise the
// instance is the Minecraft process that was not there a moment ago.
uint32_t resolveGamePid(uint32_t activated, const std::vector<uint32_t>& before, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        if (activated != 0 && Process::isRunning(activated) &&
            strings::toLower(Process::imageName(activated)) == strings::toLower(kGameExecutable)) {
            return activated;
        }

        for (const ProcessInfo& process : Process::findByName(kGameExecutable)) {
            if (std::ranges::find(before, process.pid) == before.end()) return process.pid;
        }

        if (std::chrono::steady_clock::now() >= deadline) return 0;
        Sleep(150);
    }
}

// A remote LoadLibraryW has nowhere to go until the loader inside the game has run, and
// the pid can still turn out to be a starter that exits on us. The game's own module
// appearing in the snapshot is the signal to go; an AppContainer is free to keep that
// snapshot shut, so a process that simply stays alive long enough has to do instead.
bool waitForGameReady(uint32_t pid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        if (!Process::isRunning(pid)) return false;
        if (Process::isModuleLoaded(pid, kGameExecutable)) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        Sleep(200);
    }

    Sleep(2000);
    return Process::isRunning(pid);
}

// Windows recycles pids, so the number alone stops meaning anything the moment the process
// it named exits: the answer is only trustworthy through a handle opened while it still ran.
std::optional<DWORD> exitCodeOf(HANDLE process) {
    DWORD code = 0;
    if (!process || !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE) {
        return std::nullopt;
    }
    return code;
}

std::string activationHint(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80270254UL:  // E_APPLICATION_NOT_REGISTERED
            return "Windows does not know this package. Recreate the instance and check "
                   "that developer mode is still on.";
        case 0x8027025AUL:  // E_APPLICATION_ACTIVATION_TIMED_OUT
            return "The game took too long to start.";
        case 0x80070005UL:  // E_ACCESSDENIED
            return "Windows refused the activation. Run Velyx without administrator "
                   "rights: an elevated process cannot start a Store "
                   "application.";
        default:
            return {};
    }
}

std::string slugify(std::string_view name) {
    std::string slug;
    slug.reserve(name.size());

    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }

    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "instance";
    return slug;
}

bool replaceAttribute(std::string& xml, std::string_view tag, std::string_view attribute,
                      std::string_view value) {
    const std::string open = std::string("<") + std::string(tag);

    // "<Application" is also a prefix of "<Applications", so the name has to end here.
    size_t tagStart = std::string::npos;
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1)) {
        const char next = xml[at + open.size()];
        if (next == '>' || next == '/' || std::isspace(static_cast<unsigned char>(next))) {
            tagStart = at;
            break;
        }
    }

    if (tagStart == std::string::npos) return false;

    const size_t tagEnd = xml.find('>', tagStart);
    if (tagEnd == std::string::npos) return false;

    const std::string needle = std::string(attribute) + "=\"";
    const size_t attributeStart = xml.find(needle, tagStart);
    if (attributeStart == std::string::npos || attributeStart > tagEnd) return false;

    const size_t valueStart = attributeStart + needle.size();
    const size_t valueEnd = xml.find('"', valueStart);
    if (valueEnd == std::string::npos) return false;

    xml.replace(valueStart, valueEnd - valueStart, value);
    return true;
}

bool replaceElement(std::string& xml, std::string_view tag, std::string_view value) {
    const std::string open = std::string("<") + std::string(tag) + ">";
    const std::string close = std::string("</") + std::string(tag) + ">";

    const size_t start = xml.find(open);
    if (start == std::string::npos) return false;

    const size_t end = xml.find(close, start);
    if (end == std::string::npos) return false;

    xml.replace(start + open.size(), end - start - open.size(), value);
    return true;
}

}

InstanceManager& InstanceManager::get() {
    static InstanceManager instance;
    return instance;
}

void InstanceManager::load() {
    instances_.clear();

    std::error_code ec;
    std::filesystem::create_directories(Paths::instances(), ec);

    std::ifstream stream(storeFile());
    if (!stream) return;

    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception& e) {
        Log::warn(kLog, "instances.json unreadable: {}", e.what());
        return;
    }

    if (!document.is_array()) return;

    for (const auto& entry : document) {
        Instance instance;
        instance.id = entry.value("id", std::string{});
        instance.name = entry.value("name", instance.id);
        instance.accountId = entry.value("accountId", std::string{});
        instance.gameVersion = entry.value("gameVersion", std::string{});
        instance.profile = entry.value("profile", std::string("Global"));
        instance.root = std::filesystem::path(entry.value("root", std::string{}));
        instance.packageFamilyName = entry.value("packageFamilyName", std::string{});
        instance.applicationId = entry.value("applicationId", std::string("App"));
        instance.registered = entry.value("registered", false);
        instance.injectVelyx = entry.value("injectVelyx", true);
        instance.lastPlayedMs = entry.value("lastPlayedMs", 0LL);
        instance.totalPlaySeconds = entry.value("totalPlaySeconds", 0LL);

        if (!instance.id.empty()) instances_.push_back(std::move(instance));
    }

    Log::info(kLog, "loaded {} instance(s)", instances_.size());
}

void InstanceManager::save() const {
    nlohmann::json document = nlohmann::json::array();

    for (const Instance& instance : instances_) {
        nlohmann::json entry;
        entry["id"] = instance.id;
        entry["name"] = instance.name;
        entry["accountId"] = instance.accountId;
        entry["gameVersion"] = instance.gameVersion;
        entry["profile"] = instance.profile;
        entry["root"] = instance.root.string();
        entry["packageFamilyName"] = instance.packageFamilyName;
        entry["applicationId"] = instance.applicationId;
        entry["registered"] = instance.registered;
        entry["injectVelyx"] = instance.injectVelyx;
        entry["lastPlayedMs"] = instance.lastPlayedMs;
        entry["totalPlaySeconds"] = instance.totalPlaySeconds;
        document.push_back(entry);
    }

    std::error_code ec;
    std::filesystem::create_directories(Paths::instances(), ec);

    std::ofstream stream(storeFile());
    if (stream) stream << document.dump(2);
}

Instance* InstanceManager::find(const std::string& id) {
    const auto it = std::ranges::find_if(instances_,
                                         [&](const Instance& i) { return i.id == id; });
    return it == instances_.end() ? nullptr : &*it;
}

std::optional<std::filesystem::path> InstanceManager::findInstalledGame(std::string* version) {
    std::string output;
    const int status = runPowerShell(
        L"$p = Get-AppxPackage -Name Microsoft.MinecraftUWP; "
        L"if ($p) { Write-Output ($p.InstallLocation + '|' + $p.Version) }",
        &output);

    if (status != 0) return std::nullopt;

    const auto trimmed = std::string(strings::trim(output));
    if (trimmed.empty()) return std::nullopt;

    const auto parts = strings::split(trimmed, '|');
    if (parts.empty()) return std::nullopt;

    if (version && parts.size() > 1) *version = std::string(strings::trim(parts[1]));

    std::filesystem::path location(std::string(strings::trim(parts[0])));

    std::error_code ec;
    if (!std::filesystem::exists(location / kGameExecutable, ec)) return std::nullopt;

    return location;
}

bool InstanceManager::developerModeEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    const LONG status =
        RegQueryValueExW(key, L"AllowDevelopmentWithoutDevLicense", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    return status == ERROR_SUCCESS && value != 0;
}

std::string InstanceManager::readPublisher(const std::filesystem::path& manifest) {
    std::ifstream stream(manifest);
    if (!stream) return {};

    const std::string xml((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());

    const size_t identity = xml.find("<Identity");
    if (identity == std::string::npos) return {};

    const size_t attribute = xml.find("Publisher=\"", identity);
    if (attribute == std::string::npos) return {};

    const size_t start = attribute + 11;
    const size_t end = xml.find('"', start);
    if (end == std::string::npos) return {};

    return xml.substr(start, end - start);
}

bool InstanceManager::patchManifest(const std::filesystem::path& manifest,
                                    const std::string& packageName,
                                    const std::string& displayName, std::string* error) {
    std::ifstream input(manifest);
    if (!input) {
        if (error) *error = "AppxManifest.xml introuvable";
        return false;
    }

    std::string xml((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    if (!replaceAttribute(xml, "Identity", "Name", packageName)) {
        if (error) *error = "Identity/Name missing from the manifest";
        return false;
    }

    replaceElement(xml, "DisplayName", displayName);
    replaceAttribute(xml, "Application", "Id", "App");
    replaceAttribute(xml, "uap:VisualElements", "DisplayName", displayName);

    std::ofstream output(manifest, std::ios::trunc);
    if (!output) {
        if (error) *error = "the manifest is read only";
        return false;
    }
    output << xml;

    return true;
}

std::string InstanceManager::CloneResult::failure() const {
    std::string message = std::format("the game files could not be copied "
                                      "({} copied, {} failed)", copied, failed);
    if (!firstError.empty()) message += ". First error: " + firstError;
    return message;
}

InstanceManager::CloneResult InstanceManager::cloneFiles(const std::filesystem::path& source,
                                                         const std::filesystem::path& destination,
                                                         CloneMode mode,
                                                         const ProgressFn& onProgress) {
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);

    CloneResult result;

    // Reading an installed package means reading C:\Program Files\WindowsApps, which Windows
    // hands to TrustedInstaller and to app containers rather than to us. The iterator answers
    // that with an empty walk, so ask it once, up front, where the answer is still legible.
    std::error_code walkEc;
    const std::filesystem::recursive_directory_iterator probe(source, walkEc);
    if (walkEc) {
        result.firstError = source.string() + " : " + walkEc.message();
        result.failed = 1;
        return result;
    }

    size_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        if (entry.is_regular_file(ec)) ++total;
    }

    // Linking until proven otherwise: see the first refusal below.
    bool linking = mode == CloneMode::Link;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        const auto relative = std::filesystem::relative(entry.path(), source, ec);
        const auto target = destination / relative;

        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;

        std::filesystem::create_directories(target.parent_path(), ec);

        std::error_code fileEc;
        if (linking) {
            std::filesystem::create_hard_link(entry.path(), target, fileEc);

            // Whether hard links work at all is a property of the two paths, not of the
            // file: a different volume, or a filesystem that has none. The first refusal
            // settles it for the rest of the walk, which otherwise pays for a failing
            // call on every one of several thousand files before copying it anyway.
            if (fileEc) {
                linking = false;
                Log::info(kLog, "hard links unavailable here ({}), copying the game instead",
                          fileEc.message());

                fileEc.clear();
                std::filesystem::copy_file(entry.path(), target,
                                           std::filesystem::copy_options::overwrite_existing,
                                           fileEc);
            }
        } else {
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing, fileEc);
        }

        if (fileEc) {
            ++result.failed;
            if (result.firstError.empty()) {
                result.firstError = relative.string() + " : " + fileEc.message();
            }
            continue;
        }

        ++result.copied;
        if (onProgress && (result.copied % 64 == 0 || result.copied == total)) {

            // Linking an instance is instant and costs no disk; copying one is neither.
            // Whichever is happening is worth saying while it happens, not afterwards.
            onProgress(result.copied, total,
                       linking ? relative.string() : "copying: no hard links on this drive");
        }
    }

    return result;
}

std::vector<InstanceManager::VersionSource> InstanceManager::availableVersions() {
    std::vector<VersionSource> sources;

    std::string installedVersion;
    if (const auto installed = findInstalledGame(&installedVersion)) {
        sources.push_back(VersionSource{installedVersion, *installed, true});
    }

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(Paths::versions(), ec)) {
        if (!entry.is_directory(ec)) continue;
        if (!std::filesystem::exists(entry.path() / kGameExecutable, ec)) continue;

        const std::string version = entry.path().filename().string();
        const bool known = std::ranges::any_of(
            sources, [&](const VersionSource& v) { return v.version == version; });
        if (known) continue;

        sources.push_back(VersionSource{version, entry.path(), false});
    }

    std::ranges::sort(sources, [](const VersionSource& a, const VersionSource& b) {
        return a.version > b.version;
    });

    return sources;
}

bool InstanceManager::setVersion(Instance& instance, const VersionSource& source,
                                 const ProgressFn& onProgress, std::string* error) {
    const auto fail = [&](std::string message) {
        Log::error(kLog, "version switch failed: {}", message);
        if (error) *error = std::move(message);
        return false;
    };

    if (instance.running() && Process::isRunning(instance.pid)) {
        return fail("stop the instance before changing its version");
    }

    std::error_code ec;
    if (!std::filesystem::exists(source.root / kGameExecutable, ec)) {
        return fail("this version is no longer on the disk");
    }

    unregisterInstance(instance, nullptr);

    // Worlds and the Xbox sign-in live in the AppContainer's own data container,
    // not here, so replacing the install root keeps them.
    std::filesystem::remove_all(instance.root, ec);

    const CloneResult clone = cloneFiles(source.root, instance.root, CloneMode::Link, onProgress);

    if (clone.failed != 0 || !std::filesystem::exists(instance.root / kGameExecutable, ec)) {
        return fail(clone.failure());
    }

    const std::string packageName = "Velyx." + instance.id;
    if (!patchManifest(instance.root / "AppxManifest.xml", packageName,
                       "Minecraft - " + instance.name, error)) {
        return false;
    }

    Process::grantAppContainerAccess(instance.root);

    if (!registerInstance(instance, error)) return false;

    instance.gameVersion = source.version;
    save();

    Log::info(kLog, "instance '{}' switched to {} ({} files)", instance.name, source.version,
              clone.copied);
    return true;
}

bool InstanceManager::create(const std::string& name, CloneMode mode, const ProgressFn& onProgress,
                             std::string* error) {
    const auto fail = [&](std::string message) {
        Log::error(kLog, "could not create the instance: {}", message);
        if (error) *error = std::move(message);
        return false;
    };

    std::string version;
    const auto source = findInstalledGame(&version);
    if (!source) {
        return fail("Minecraft Bedrock is not installed on this machine.");
    }

    if (!developerModeEnabled()) {
        return fail("Windows developer mode has to be enabled "
                    "(Settings > Privacy & security > For developers).");
    }

    Instance instance;
    instance.name = name;
    instance.gameVersion = version;

    const std::string base = slugify(name);
    instance.id = base;
    for (int suffix = 2; find(instance.id) != nullptr; ++suffix) {
        instance.id = base + "-" + std::to_string(suffix);
    }

    instance.root = Paths::instances() / instance.id;

    std::error_code ec;
    if (std::filesystem::exists(instance.root, ec)) {
        return fail("the folder " + instance.root.string() + " already exists");
    }
    std::filesystem::create_directories(instance.root, ec);

    const CloneResult clone = cloneFiles(*source, instance.root, mode, onProgress);

    // Windows registers and activates a half-copied package without a word of complaint,
    // and the game then dies before it is ever a process — which reads, much later, as a
    // launch that went nowhere. The payload is checked here, where the cause is still known.
    if (clone.failed != 0 || !std::filesystem::exists(instance.root / kGameExecutable, ec)) {
        std::filesystem::remove_all(instance.root, ec);
        return fail(clone.failure());
    }

    // A distinct package identity is what lets Windows run several copies at once,
    // each with its own data container and therefore its own Xbox sign-in.
    const std::string packageName = "Velyx." + instance.id;
    const auto manifest = instance.root / "AppxManifest.xml";

    if (!patchManifest(manifest, packageName, "Minecraft - " + name, error)) {
        std::filesystem::remove_all(instance.root, ec);
        return false;
    }

    instance.packageFamilyName = packageName + "_8wekyb3d8bbwe";

    Process::grantAppContainerAccess(instance.root);

    if (!registerInstance(instance, error)) {
        std::filesystem::remove_all(instance.root, ec);
        return false;
    }

    instances_.push_back(instance);
    save();

    Log::info(kLog, "created instance '{}' ({} files)", name, clone.copied);
    return true;
}

bool InstanceManager::registerInstance(Instance& instance, std::string* error) {
    const auto manifest = instance.root / "AppxManifest.xml";

    std::error_code ec;
    if (!std::filesystem::exists(manifest, ec)) {
        if (error) *error = "no manifest found for " + instance.name;
        return false;
    }

    std::string output;
    const std::wstring command =
        L"Add-AppxPackage -Path '" + manifest.wstring() +
        L"' -Register -ExternalLocation '" + instance.root.wstring() + L"'";

    if (runPowerShell(command, &output) != 0) {

        const std::wstring fallback = L"Add-AppxPackage -Path '" + manifest.wstring() +
                                      L"' -Register";
        if (runPowerShell(fallback, &output) != 0) {
            if (error) *error = "Add-AppxPackage failed: " + std::string(strings::trim(output));
            return false;
        }
    }

    if (const auto identity = queryPackage(packageNameFor(instance))) {
        instance.packageFamilyName = identity->familyName;
        instance.applicationId = identity->applicationId;
    }

    instance.registered = true;
    Log::info(kLog, "registered instance '{}' ({})", instance.name, instance.packageFamilyName);
    return true;
}

bool InstanceManager::unregisterInstance(Instance& instance, std::string* error) {
    std::string output;
    const std::wstring command = L"Get-AppxPackage -Name '" +
                                 strings::toUtf16(packageNameFor(instance)) +
                                 L"' | Remove-AppxPackage";

    if (runPowerShell(command, &output) != 0) {
        if (error) *error = "Remove-AppxPackage failed: " + std::string(strings::trim(output));
        return false;
    }

    instance.registered = false;
    return true;
}

bool InstanceManager::launch(Instance& instance, std::string* error) {
    const auto fail = [&](std::string message) {
        Log::error(kLog, "launch failed: {}", message);
        if (error) *error = std::move(message);
        return false;
    };

    if (instance.running() && Process::isRunning(instance.pid)) {
        return fail("this instance is already running");
    }

    // A registration is per user and Windows drops it on its own — when the payload moves,
    // when developer mode goes off, sometimes across a major update — so the stored flag
    // only tells us what we did last time, never what the system still holds.
    if (const auto identity = queryPackage(packageNameFor(instance))) {
        instance.packageFamilyName = identity->familyName;
        instance.applicationId = identity->applicationId;
        instance.registered = true;
    } else {
        instance.registered = false;
        if (!registerInstance(instance, error)) return false;
    }

    save();

    // A registration only names a folder; it does not promise the folder still holds a game.
    // Activating an empty one succeeds and starts nothing, which is indistinguishable from
    // the game refusing to come up unless the payload is ruled out first.
    std::error_code payloadEc;
    if (!std::filesystem::exists(instance.root / kGameExecutable, payloadEc)) {
        return fail(std::format("{} is missing from {}: the game files were not "
                                "were copied. Recreate the instance.",
                                kGameExecutable, instance.root.string()));
    }

    // The sandbox runs as its own AppContainer identity, and only files that grant that
    // identity read access exist as far as it is concerned. Re-granting costs nothing and
    // covers whatever landed in the folder since the instance was made.
    if (!Process::grantAppContainerAccess(instance.root)) {
        Log::warn(kLog, "the AppContainer grant on {} did not go through",
                  instance.root.string());
    }

    std::vector<uint32_t> alreadyRunning;
    for (const ProcessInfo& process : Process::findByName(kGameExecutable)) {
        alreadyRunning.push_back(process.pid);
    }

    Log::info(kLog, "activating {} from {}", instance.activationId(), instance.root.string());

    {
        const ComApartment apartment;

        IApplicationActivationManager* manager = nullptr;
        HRESULT hr = E_FAIL;
        if (apartment.ready()) {
            hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER,
                                  __uuidof(IApplicationActivationManager),
                                  reinterpret_cast<void**>(&manager));
        }

        if (FAILED(hr) || !manager) {
            return fail(std::format("ApplicationActivationManager indisponible (0x{:08X})",
                                    static_cast<unsigned>(hr)));
        }

        // Without this the game starts behind the launcher.
        CoAllowSetForegroundWindow(manager, nullptr);

        DWORD pid = 0;
        const std::wstring appId = strings::toUtf16(instance.activationId());

        // AO_NOERRORUI buys silence at the price of the only explanation there is: Windows
        // knows why an app it accepted never started, and its dialog is where it says so.
        hr = manager->ActivateApplication(appId.c_str(), nullptr, AO_NONE, &pid);
        manager->Release();

        if (FAILED(hr)) {
            const std::string hint = activationHint(hr);
            return fail(std::format("ActivateApplication failed (0x{:08X}){}{}",
                                    static_cast<unsigned>(hr), hint.empty() ? "" : " : ", hint));
        }

        instance.pid = pid;
    }

    const uint32_t activatedPid = instance.pid;

    // Held from here so the starter's exit code survives the process itself.
    const HANDLE starter =
        activatedPid == 0
            ? nullptr
            : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, activatedPid);

    instance.pid = resolveGamePid(activatedPid, alreadyRunning, 30000);

    if (instance.pid == 0) {
        std::string detail;
        if (activatedPid == 0) {
            detail = " Windows reported no process.";
        } else if (const auto code = exitCodeOf(starter)) {
            detail = std::format(" The process Windows started (pid {}) exited with "
                                 "code 0x{:08X} without opening the game.",
                                 activatedPid, static_cast<unsigned>(*code));
        } else {
            detail = std::format(" The process Windows started (pid {}, {}) is still running "
                                 "but never opened the game.",
                                 activatedPid, Process::imageName(activatedPid));
        }

        if (starter) CloseHandle(starter);
        return fail("the game was activated but its process cannot be found." + detail);
    }

    if (starter) CloseHandle(starter);

    Log::info(kLog, "activation reported pid {}, the game runs as {} (pid {})", activatedPid,
              Process::imageName(instance.pid), instance.pid);

    instance.lastPlayedMs = nowMs();
    save();

    if (!instance.injectVelyx) {
        Log::info(kLog, "launched instance '{}' (pid {}, without Velyx)", instance.name, instance.pid);
        return true;
    }

    auto dll = Paths::root() / "bin" / "Velyx.dll";

    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        dll = std::filesystem::path(modulePath).parent_path() / "Velyx.dll";
    }

    // Bedrock reaches its window through more than one process, so a pid that dies on us
    // was a step on the way there rather than the game: look the game up again and retry.
    std::string injectionError = "the game process stopped before the injection";

    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (waitForGameReady(instance.pid, 30000) &&
            Process::injectLibrary(instance.pid, dll, &injectionError)) {
            Log::info(kLog, "launched instance '{}' with Velyx (pid {})", instance.name,
                      instance.pid);
            return true;
        }

        if (Process::isRunning(instance.pid)) break;

        const uint32_t running = resolveGamePid(0, alreadyRunning, 15000);
        if (running == 0 || running == instance.pid) break;

        Log::warn(kLog, "pid {} was not the game, retrying with pid {}", instance.pid, running);
        instance.pid = running;
        save();
    }

    if (error) *error = "The game is running but Velyx could not be injected: " + injectionError;
    return false;
}

bool InstanceManager::remove(const std::string& id, bool deleteFiles, std::string* error) {
    Instance* instance = find(id);
    if (!instance) return false;

    if (instance->running() && Process::isRunning(instance->pid)) {
        if (error) *error = "stop the instance before deleting it";
        return false;
    }

    unregisterInstance(*instance, nullptr);

    if (deleteFiles) {
        std::error_code ec;
        std::filesystem::remove_all(instance->root, ec);
    }

    std::erase_if(instances_, [&](const Instance& entry) { return entry.id == id; });
    save();
    return true;
}

void InstanceManager::refreshRunningState() {
    const auto processes = Process::findByName(kGameExecutable);

    for (Instance& instance : instances_) {
        if (instance.pid != 0 && !Process::isRunning(instance.pid)) instance.pid = 0;
    }

    (void)processes;
}

}
