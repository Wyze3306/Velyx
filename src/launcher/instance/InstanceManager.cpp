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

std::string activationHint(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x80270254UL:  // E_APPLICATION_NOT_REGISTERED
            return "Windows ne connaît pas ce paquet. Réinstallez l'instance et vérifiez "
                   "que le mode développeur est toujours actif.";
        case 0x8027025AUL:  // E_APPLICATION_ACTIVATION_TIMED_OUT
            return "Le jeu a mis trop de temps à démarrer.";
        case 0x80070005UL:  // E_ACCESSDENIED
            return "Windows a refusé l'activation. Lancez Velyx sans les droits "
                   "administrateur : un processus élevé ne peut pas démarrer une application "
                   "du Store.";
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
        if (error) *error = "Identity/Name absent du manifeste";
        return false;
    }

    replaceElement(xml, "DisplayName", displayName);
    replaceAttribute(xml, "Application", "Id", "App");
    replaceAttribute(xml, "uap:VisualElements", "DisplayName", displayName);

    std::ofstream output(manifest, std::ios::trunc);
    if (!output) {
        if (error) *error = "manifeste en lecture seule";
        return false;
    }
    output << xml;

    return true;
}

size_t InstanceManager::cloneFiles(const std::filesystem::path& source,
                                  const std::filesystem::path& destination, CloneMode mode,
                                  const ProgressFn& onProgress) {
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);

    size_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        if (entry.is_regular_file(ec)) ++total;
    }

    size_t done = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        const auto relative = std::filesystem::relative(entry.path(), source, ec);
        const auto target = destination / relative;

        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;

        std::filesystem::create_directories(target.parent_path(), ec);

        if (mode == CloneMode::Link) {
            std::filesystem::create_hard_link(entry.path(), target, ec);
            if (ec) {
                ec.clear();
                std::filesystem::copy_file(entry.path(), target,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        } else {
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }

        ++done;
        if (onProgress && (done % 64 == 0 || done == total)) {
            onProgress(done, total, relative.string());
        }
    }

    return done;
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
        return fail("arrêtez l'instance avant de changer sa version");
    }

    std::error_code ec;
    if (!std::filesystem::exists(source.root / kGameExecutable, ec)) {
        return fail("cette version n'est plus disponible sur le disque");
    }

    unregisterInstance(instance, nullptr);

    // Worlds and the Xbox sign-in live in the AppContainer's own data container,
    // not here, so replacing the install root keeps them.
    std::filesystem::remove_all(instance.root, ec);

    const size_t done = cloneFiles(source.root, instance.root, CloneMode::Link, onProgress);

    const std::string packageName = "Velyx." + instance.id;
    if (!patchManifest(instance.root / "AppxManifest.xml", packageName,
                       "Minecraft - " + instance.name, error)) {
        return false;
    }

    Process::grantAppContainerAccess(instance.root);

    if (!registerInstance(instance, error)) return false;

    instance.gameVersion = source.version;
    save();

    Log::info(kLog, "instance '{}' switched to {} ({} files)", instance.name, source.version, done);
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
        return fail("Minecraft Bedrock n'est pas installé sur cette machine.");
    }

    if (!developerModeEnabled()) {
        return fail("Le mode développeur de Windows doit être activé "
                    "(Paramètres > Confidentialité et sécurité > Espace développeurs).");
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
        return fail("le dossier " + instance.root.string() + " existe déjà");
    }
    std::filesystem::create_directories(instance.root, ec);

    const size_t done = cloneFiles(*source, instance.root, mode, onProgress);

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

    Log::info(kLog, "created instance '{}' ({} files)", name, done);
    return true;
}

bool InstanceManager::registerInstance(Instance& instance, std::string* error) {
    const auto manifest = instance.root / "AppxManifest.xml";

    std::error_code ec;
    if (!std::filesystem::exists(manifest, ec)) {
        if (error) *error = "manifeste introuvable pour " + instance.name;
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
            if (error) *error = "Add-AppxPackage a échoué : " + std::string(strings::trim(output));
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
        if (error) *error = "Remove-AppxPackage a échoué : " + std::string(strings::trim(output));
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
        return fail("cette instance est déjà lancée");
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
        hr = manager->ActivateApplication(appId.c_str(), nullptr, AO_NOERRORUI, &pid);
        manager->Release();

        if (FAILED(hr)) {
            const std::string hint = activationHint(hr);
            return fail(std::format("ActivateApplication a échoué (0x{:08X}){}{}",
                                    static_cast<unsigned>(hr), hint.empty() ? "" : " : ", hint));
        }

        instance.pid = pid;
    }

    instance.lastPlayedMs = nowMs();
    save();

    if (!instance.injectVelyx) {
        Log::info(kLog, "launched instance '{}' (pid {}, without Velyx)", instance.name, instance.pid);
        return true;
    }

    Sleep(2500);

    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    const auto dll = std::filesystem::path(modulePath).parent_path() / "Velyx.dll";

    std::string injectionError;
    if (!Process::injectLibrary(instance.pid, dll, &injectionError)) {

        if (error) *error = "Le jeu est lancé mais Velyx n'a pas pu être injecté : " + injectionError;
        return false;
    }

    Log::info(kLog, "launched instance '{}' with Velyx (pid {})", instance.name, instance.pid);
    return true;
}

bool InstanceManager::remove(const std::string& id, bool deleteFiles, std::string* error) {
    Instance* instance = find(id);
    if (!instance) return false;

    if (instance->running() && Process::isRunning(instance->pid)) {
        if (error) *error = "arrêtez l'instance avant de la supprimer";
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
