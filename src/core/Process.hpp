#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace velyx {

struct ProcessInfo {
    uint32_t pid = 0;
    std::string name;
    std::filesystem::path executable;
};

class Process {
public:
    static std::vector<ProcessInfo> enumerate();

    static std::vector<ProcessInfo> findByName(std::string_view executableName);

    static bool isRunning(uint32_t pid);
    static bool terminate(uint32_t pid);

    // Empty when the process is gone or keeps its image path to itself.
    static std::string imageName(uint32_t pid);

    static uintptr_t moduleBase(uint32_t pid, std::string_view moduleName);

    static bool isModuleLoaded(uint32_t pid, std::string_view moduleName);

    static bool injectLibrary(uint32_t pid, const std::filesystem::path& dll, std::string* error = nullptr);

    static bool grantAppContainerAccess(const std::filesystem::path& target);

    static uint32_t waitForProcess(std::string_view executableName, int timeoutMs, uint32_t ignorePid = 0);
};

}
