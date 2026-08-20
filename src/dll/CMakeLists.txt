file(GLOB_RECURSE VELYX_DLL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE VELYX_DLL_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(Velyx SHARED ${VELYX_DLL_SOURCES} ${VELYX_DLL_HEADERS})

set_target_properties(Velyx PROPERTIES
        OUTPUT_NAME "Velyx"
        PREFIX ""
        FOLDER velyx)

target_link_libraries(Velyx PRIVATE
        velyx_flags
        velyx_core
        minhook
        nlohmann_json
        d3d11
        d3d12
        dxgi
        dxguid
        d2d1
        dwrite
        windowscodecs
        version
        winhttp
        winmm
        ws2_32
        shlwapi
        ole32
        oleaut32
        uuid)

# Chargee dans un processus AppContainer : rien ne doit etre resolu depuis le dossier du jeu.
if (NOT MSVC)
    target_link_options(Velyx PRIVATE -static -static-libgcc -static-libstdc++
            -Wl,--exclude-all-symbols)
endif ()
