file(GLOB_RECURSE VELYX_LAUNCHER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE VELYX_LAUNCHER_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_executable(VelyxLauncher WIN32 ${VELYX_LAUNCHER_SOURCES} ${VELYX_LAUNCHER_HEADERS})

set_target_properties(VelyxLauncher PROPERTIES
        OUTPUT_NAME "Velyx"
        FOLDER velyx)

target_link_libraries(VelyxLauncher PRIVATE
        velyx_flags
        velyx_core
        nlohmann_json
        d2d1
        dwrite
        dwmapi
        ole32
        oleaut32
        uuid
        shell32
        shlwapi
        advapi32)

if (NOT MSVC)
    # -municode : mingw cherche wWinMain plutot que WinMain.
    target_compile_options(VelyxLauncher PRIVATE -municode)
    target_link_options(VelyxLauncher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif ()
