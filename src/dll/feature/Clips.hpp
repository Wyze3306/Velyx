#pragma once

#include <string>
#include <vector>

namespace velyx {

struct ClipMarker {
    long long atMs = 0;
    long long sessionSeconds = 0;
    std::string server;
    std::string note;
};

class Clips {
public:
    static Clips& get();

    void load();

    ClipMarker mark(std::string note = {});

    [[nodiscard]] const std::vector<ClipMarker>& all() const { return markers_; }
    [[nodiscard]] std::vector<ClipMarker> recent(size_t limit = 50) const;

    void clear();

private:
    Clips() = default;

    void save() const;

    std::vector<ClipMarker> markers_;
    bool loaded_ = false;
};

} // namespace velyx
