#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Http.hpp"

namespace velyx {

/// One build of the game as the Bedrock archive lists it: the version, the channel it
/// shipped on, and the mirrors that still hand out its package.
struct CatalogVersion {
    std::string version;
    bool preview = false;
    std::vector<std::string> mirrors;

    /// "1.26.44.3", or "1.26.50.20 (preview)" — what a menu line says.
    [[nodiscard]] std::string label() const;

    /// The name the downloaded package takes on disk. The archive's own file names
    /// carry the four-part build number rather than the version anyone recognises.
    [[nodiscard]] std::string fileName() const;
};

/// The builds Velyx can fetch, from MinecraftBedrockArchiver/GdkLinks. That list is a
/// file on GitHub, so it is read once and kept in the cache folder: the launcher opens
/// on a train as often as on a desk, and a version list is not worth an empty window.
namespace catalog {

/// The cached list, newest first, loaded from disk on the first call. Empty until a
/// refresh has landed at least once.
[[nodiscard]] const std::vector<CatalogVersion>& versions();

/// Reads the list from GitHub and rewrites the cache. Network-bound: belongs on the
/// job thread, not on the frame.
bool refresh(std::string* error = nullptr);

/// Where a downloaded package waits before anything is done with it.
[[nodiscard]] std::filesystem::path packageFile(const CatalogVersion& version);

/// Downloads the package, mirror after mirror until one of them answers. A part file
/// left by an earlier attempt is resumed rather than thrown away.
bool fetch(const CatalogVersion& version, const http::ProgressFn& onProgress,
           std::string* error = nullptr);

} // namespace catalog
} // namespace velyx
