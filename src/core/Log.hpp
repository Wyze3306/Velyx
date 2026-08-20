#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace velyx {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
    long long timestampMs = 0;
};

class Log {
public:
    static void init(const std::filesystem::path& file, bool attachConsole);
    static void shutdown();

    static void setMinimumLevel(LogLevel level);
    static LogLevel minimumLevel();

    static void write(LogLevel level, std::string_view category, std::string_view message);

    static std::vector<LogRecord> recent(size_t count = 256);

    static void setActiveModule(std::string_view name);
    static std::string activeModule();

    template <typename... Args>
    static void trace(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Trace, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    static void debug(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Debug, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    static void info(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Info, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    static void warn(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    static void error(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, category, fmt, std::forward<Args>(args)...);
    }
    template <typename... Args>
    static void fatal(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Fatal, category, fmt, std::forward<Args>(args)...);
    }

private:
    template <typename... Args>
    static void log(LogLevel level, std::string_view category,
                    std::format_string<Args...> fmt, Args&&... args) {
        if (level < minimumLevel()) return;
        write(level, category, std::format(fmt, std::forward<Args>(args)...));
    }
};

const char* toString(LogLevel level);

}
