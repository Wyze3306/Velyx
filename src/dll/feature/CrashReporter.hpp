#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace velyx::crash {

void install();
void uninstall();

class Breadcrumb {
public:
    explicit Breadcrumb(const char* label);
    ~Breadcrumb();

    Breadcrumb(const Breadcrumb&) = delete;
    Breadcrumb& operator=(const Breadcrumb&) = delete;

private:
    const char* previous_ = nullptr;
};

const char* currentBreadcrumb();

struct Report {
    std::filesystem::path file;
    std::string when;
    std::string exception;
    std::string address;
    std::string suspect;
    bool insideVelyx = false;
};

std::optional<Report> lastReport();

void clearReports();

} // namespace velyx::crash
