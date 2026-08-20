#include "CrashReporter.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>

#include <velyx/Version.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/module/ModuleManager.hpp"

namespace velyx::crash {
namespace {

constexpr const char* kLog = "Crash";

thread_local const char* t_breadcrumb = nullptr;
std::atomic<bool> g_reporting{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
uintptr_t g_selfBase = 0;
size_t g_selfSize = 0;

std::string timestampFile() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
    localtime_s(&tm, &time);

    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        case 0xE06D7363:                         return "CPP_EXCEPTION";
        default:                                 return "UNKNOWN";
    }
}

void resolveSelfRange() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&resolveSelfRange), &self);
    if (!self) return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    g_selfBase = reinterpret_cast<uintptr_t>(self);
    g_selfSize = nt->OptionalHeader.SizeOfImage;
}

void writeReport(EXCEPTION_POINTERS* info) {
    const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const bool insideVelyx =
        g_selfBase != 0 && address >= g_selfBase && address < g_selfBase + g_selfSize;

    std::error_code ec;
    std::filesystem::create_directories(Paths::crashes(), ec);

    const auto path = Paths::crashes() / ("crash-" + timestampFile() + ".txt");
    std::ofstream out(path);
    if (!out) return;

    const char* breadcrumb = t_breadcrumb ? t_breadcrumb : "inconnu";

    out << "Velyx " << version::kFull << "\n";
    out << "Minecraft " << Signatures::get().gameVersion() << "\n";
    out << "Exception " << exceptionName(info->ExceptionRecord->ExceptionCode) << " (0x" << std::hex
        << info->ExceptionRecord->ExceptionCode << std::dec << ")\n";
    out << "Adresse 0x" << std::hex << address << std::dec << "\n";
    out << "Origine " << (insideVelyx ? "Velyx" : "jeu ou pilote") << "\n";
    out << "Dernier module actif : " << breadcrumb << "\n\n";

    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const auto operation = info->ExceptionRecord->ExceptionInformation[0];
        out << (operation == 0 ? "Lecture" : operation == 1 ? "Écriture" : "Exécution")
            << " à 0x" << std::hex << info->ExceptionRecord->ExceptionInformation[1] << std::dec
            << "\n\n";
    }

    out << "Modules actifs :\n";
    for (const Module* module : modules().enabled()) {
        out << "  - " << module->name() << " (" << module->id() << ")\n";
    }

    const auto missing = Signatures::get().missing();
    if (!missing.empty()) {
        out << "\nSignatures requises manquantes :\n";
        for (const std::string& name : missing) out << "  - " << name << "\n";
    }

    out << "\nJournal :\n";
    for (const LogRecord& record : Log::recent(60)) {
        out << "  [" << toString(record.level) << "] " << record.category << " : " << record.message
            << "\n";
    }

    ClientConfig& settings = config();
    settings.lastCrashModule = breadcrumb;
    settings.lastCrashReason = exceptionName(info->ExceptionRecord->ExceptionCode);
    settings.save();
}

LONG WINAPI handler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (g_reporting.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;

    writeReport(info);

    g_reporting.store(false);
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

Breadcrumb::Breadcrumb(const char* label) : previous_(t_breadcrumb) { t_breadcrumb = label; }
Breadcrumb::~Breadcrumb() { t_breadcrumb = previous_; }

const char* currentBreadcrumb() { return t_breadcrumb ? t_breadcrumb : ""; }

void install() {
    resolveSelfRange();
    g_previousFilter = SetUnhandledExceptionFilter(&handler);
    Log::debug(kLog, "rapport de plantage armé");
}

void uninstall() {
    SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
}

std::optional<Report> lastReport() {
    std::error_code ec;
    if (!std::filesystem::exists(Paths::crashes(), ec)) return std::nullopt;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(Paths::crashes(), ec)) {
        if (entry.path().extension() == ".txt") files.push_back(entry.path());
    }
    if (files.empty()) return std::nullopt;

    std::ranges::sort(files);

    Report report;
    report.file = files.back();
    report.when = report.file.stem().string();

    std::ifstream in(report.file);
    std::string line;
    while (std::getline(in, line)) {
        if (strings::startsWith(line, "Exception ")) report.exception = line.substr(10);
        else if (strings::startsWith(line, "Adresse ")) report.address = line.substr(8);
        else if (strings::startsWith(line, "Dernier module actif : ")) report.suspect = line.substr(23);
        else if (strings::startsWith(line, "Origine ")) report.insideVelyx = line.find("Velyx") != std::string::npos;
    }

    return report;
}

void clearReports() {
    std::error_code ec;
    std::filesystem::remove_all(Paths::crashes(), ec);
    std::filesystem::create_directories(Paths::crashes(), ec);

    config().lastCrashModule.clear();
    config().lastCrashReason.clear();
    config().save();
}

} // namespace velyx::crash
