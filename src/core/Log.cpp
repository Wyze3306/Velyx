#include "Log.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace velyx {
namespace {

constexpr size_t kRingCapacity = 1024;

struct State {
    std::mutex mutex;
    std::FILE* file = nullptr;
    bool ownsConsole = false;
    std::deque<LogRecord> ring;
    std::string activeModule;
};

State& state() {
    static State instance;
    return instance;
}

std::atomic<LogLevel> g_minimum{LogLevel::Info};

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_s(&tm, &time);

    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buffer;
}

const char* ansiColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\x1b[38;5;245m";
        case LogLevel::Debug: return "\x1b[38;5;108m";
        case LogLevel::Info:  return "\x1b[38;5;79m";
        case LogLevel::Warn:  return "\x1b[38;5;221m";
        case LogLevel::Error: return "\x1b[38;5;203m";
        case LogLevel::Fatal: return "\x1b[1;38;5;203m";
    }
    return "\x1b[0m";
}

}

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

void Log::init(const std::filesystem::path& file, bool attachConsole) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);

    if (attachConsole && !s.ownsConsole) {
        if (AllocConsole()) {
            s.ownsConsole = true;
            SetConsoleTitleA("Velyx");
            SetConsoleOutputCP(CP_UTF8);

            const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (GetConsoleMode(out, &mode)) {
                SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }

            std::FILE* dummy = nullptr;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            freopen_s(&dummy, "CONOUT$", "w", stderr);
        }
    }

    if (!file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);

        if (std::filesystem::exists(file, ec)) {
            auto previous = file;
            previous.replace_extension(".previous.log");
            std::filesystem::remove(previous, ec);
            std::filesystem::rename(file, previous, ec);
        }

        s.file = _wfopen(file.c_str(), L"w");
    }
}

void Log::shutdown() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);

    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    if (s.ownsConsole) {
        FreeConsole();
        s.ownsConsole = false;
    }
}

void Log::setMinimumLevel(LogLevel level) { g_minimum.store(level, std::memory_order_relaxed); }

LogLevel Log::minimumLevel() { return g_minimum.load(std::memory_order_relaxed); }

void Log::write(LogLevel level, std::string_view category, std::string_view message) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);

    s.ring.push_back(LogRecord{level, std::string(category), std::string(message), nowMs()});
    if (s.ring.size() > kRingCapacity) s.ring.pop_front();

    const std::string time = timestamp();

    if (s.file) {
        std::fprintf(s.file, "[%s] [%s] [%.*s] %.*s\n", time.c_str(), toString(level),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(s.file);
    }

    if (s.ownsConsole) {
        std::fprintf(stdout, "\x1b[38;5;240m%s\x1b[0m %s%s\x1b[0m \x1b[38;5;114m%.*s\x1b[0m %.*s\n",
                     time.c_str(), ansiColor(level), toString(level),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(stdout);
    }

#ifdef VELYX_DEBUG
    OutputDebugStringA(std::string("[Velyx] ").append(message).append("\n").c_str());
#endif
}

std::vector<LogRecord> Log::recent(size_t count) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);

    const size_t available = s.ring.size();
    const size_t take = count < available ? count : available;

    return {s.ring.end() - static_cast<std::ptrdiff_t>(take), s.ring.end()};
}

void Log::setActiveModule(std::string_view name) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.activeModule.assign(name);
}

std::string Log::activeModule() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    return s.activeModule;
}

}
