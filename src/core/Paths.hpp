#pragma once

#include <filesystem>

namespace velyx {

class Paths {
public:

    static void setRoot(const std::filesystem::path& root);

    static const std::filesystem::path& root();

    static std::filesystem::path config();
    static std::filesystem::path profiles();
    static std::filesystem::path profile(const std::string& name);
    static std::filesystem::path profileVersions(const std::string& name);
    static std::filesystem::path themes();
    static std::filesystem::path scripts();
    static std::filesystem::path plugins();
    static std::filesystem::path screenshots();
    static std::filesystem::path replays();
    static std::filesystem::path stats();
    static std::filesystem::path notes();
    static std::filesystem::path waypoints();
    static std::filesystem::path logs();
    static std::filesystem::path crashes();
    static std::filesystem::path cache();
    static std::filesystem::path assets();

    static std::filesystem::path instances();
    static std::filesystem::path versions();
    static std::filesystem::path accounts();

    static void ensureLayout();

    static std::string sanitize(std::string_view name);

private:
    static std::filesystem::path defaultRoot();
};

}
