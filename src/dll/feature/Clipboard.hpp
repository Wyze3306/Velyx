#pragma once

#include <string>
#include <string_view>

namespace velyx::clipboard {

// The Windows clipboard, in the two directions anything here needs it. Kept apart from
// core, which knows nothing about windows beyond paths, and shared because more than
// one place hands the user something to paste.

bool copy(std::string_view text);

std::string read();

}
