add_library(velyx_core STATIC
        Log.cpp
        Paths.cpp
        Strings.cpp
        Color.cpp
        Process.cpp)

target_link_libraries(velyx_core
        PUBLIC velyx_flags nlohmann_json
        PRIVATE shlwapi shell32 ole32 advapi32 psapi)

set_target_properties(velyx_core PROPERTIES FOLDER velyx)
