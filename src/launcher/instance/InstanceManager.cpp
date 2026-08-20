#include "InstanceManager.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
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
    const size_t tagStart = xml.find(std::string("<") + std::string(tag));
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

    size_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(*source, ec)) {
        if (entry.is_regular_file(ec)) ++total;
    }

    size_t done = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(*source, ec)) {
        const auto relative = std::filesystem::relative(entry.path(), *source, ec);
        const auto destination = instance.root / relative;

        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(destination, ec);
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;

        std::filesystem::create_directories(destination.parent_path(), ec);

        if (mode == CloneMode::Link) {
            std::filesystem::create_hard_link(entry.path(), destination, ec);

            if (ec) {
                ec.clear();
                std::filesystem::copy_file(entry.path(), destination,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        } else {
            std::filesystem::copy_file(entry.path(), destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }

        ++done;
        if (onProgress && (done % 64 == 0 || done == total)) {
            onProgress(done, total, relative.string());
        }
    }

    // A distinct package identity is what lets Windows run several copies at once,
    // each with its own data container and therefore its own Xbox sign-in.
    const std::string packageName = "Velyx." + instance.id;
    const auto manifest = instance.root / "AppxManifest.xml";

    if (!patchManifest(manifest, packageName, "Minecraft — " + name, error)) {
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

    std::string details;
    runPowerShell(L"(Get-AppxPackage -Name 'Velyx." + strings::toUtf16(instance.id) +
                      L"').PackageFamilyName",
                  &details);

    const auto trimmed = std::string(strings::trim(details));
    if (!trimmed.empty()) instance.packageFamilyName = trimmed;

    instance.registered = true;
    Log::info(kLog, "registered instance '{}' ({})", instance.name, instance.packageFamilyName);
    return true;
}

bool InstanceManager::unregisterInstance(Instance& instance, std::string* error) {
    std::string output;
    const std::wstring command = L"Get-AppxPackage -Name 'Velyx." +
                                 strings::toUtf16(instance.id) + L"' | Remove-AppxPackage";

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

    if (!instance.registered && !registerInstance(instance, error)) return false;

    if (instance.running() && Process::isRunning(instance.pid)) {
        return fail("cette instance est déjà lancée");
    }

    {
        IApplicationActivationManager* manager = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                      CLSCTX_LOCAL_SERVER,
                                      __uuidof(IApplicationActivationManager),
                                      reinterpret_cast<void**>(&manager));
        if (FAILED(hr) || !manager) {
            return fail("ApplicationActivationManager indisponible");
        }

        DWORD pid = 0;
        const std::wstring appId = strings::toUtf16(instance.activationId());
        hr = manager->ActivateApplication(appId.c_str(), nullptr, AO_NOERRORUI, &pid);
        manager->Release();

        if (FAILED(hr)) {
            return fail(std::format("ActivateApplication failed (0x{:08X})",
                                    static_cast<unsigned>(hr)));
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
