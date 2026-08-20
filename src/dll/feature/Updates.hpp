#pragma once

#include <string>

namespace velyx::updates {

struct Release {
    std::string tag;
    std::string name;
    std::string notes;
    std::string url;
    std::string publishedAt;
    bool prerelease = false;
};

struct Status {
    bool checking = false;
    bool checked = false;
    bool updateAvailable = false;
    Release latest;
    std::string error;
};

void check(const std::string& channel);

void shutdown();

const Status& status();

bool openReleasePage();

int compareVersions(const std::string& left, const std::string& right);

} // namespace velyx::updates
