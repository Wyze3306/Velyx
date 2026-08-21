#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace velyx {

/// The interface is written in English in the source; another language is a table of
/// English -> translation loaded from `assets/lang/<code>.json` at startup. A string
/// with no entry comes back untouched, so a missing or partial table costs nothing
/// but English.
namespace lang {

/// Loads `<directory>/<code>.json`. "en" clears the table, since English needs none.
/// Tables already loaded are kept alive rather than freed: `tr` hands out views into
/// them, and a language change must not leave one dangling.
void load(std::string_view code, const std::filesystem::path& directory);

[[nodiscard]] const std::string& code();

/// The language codes there are tables for, "en" first. Reads the directory.
[[nodiscard]] std::vector<std::string> available(const std::filesystem::path& directory);

} // namespace lang

/// Translates a string for display. The result is a view of the table entry, or of
/// `english` itself, so it stays valid as long as the string passed in does.
[[nodiscard]] std::string_view tr(std::string_view english);

} // namespace velyx
