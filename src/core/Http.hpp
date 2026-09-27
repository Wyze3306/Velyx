#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace velyx::http {

/// Bytes taken and bytes expected — the second is zero when the server declines to
/// say. Returning false stops the transfer where it stands; the part already on disk
/// is kept, so the next attempt carries on from there.
using ProgressFn = std::function<bool(uint64_t received, uint64_t total)>;

/// A GET whose answer fits in memory. Empty on failure, with `error` set.
[[nodiscard]] std::string get(std::string_view url, std::string* error = nullptr);

/// A GET whose answer does not. Nothing appears at `target` until the file is whole:
/// until then it is `target` with ".part" after it, which a later call resumes from
/// rather than starting over — a game package is a gigabyte, and connections drop.
bool download(std::string_view url, const std::filesystem::path& target,
              const ProgressFn& onProgress = {}, std::string* error = nullptr);

} // namespace velyx::http
