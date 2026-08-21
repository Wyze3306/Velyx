file(GLOB_RECURSE VELYX_LAUNCHER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE VELYX_LAUNCHER_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# Everything the launcher needs ships inside the executable: the client DLL and
# the interface fonts are linked in as resources and unpacked on first run.
set(VELYX_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/velyx_payload.rc")

# windres glues adjacent quoted strings together the way a C compiler does, so the
# path of one entry would swallow the name of the next. A neutral LANGUAGE statement
# between entries puts a token in the way and keeps every path quoted, which is what
# lets the build survive a source directory with a space in its name.
set(VELYX_PAYLOAD_LINES "")

function (velyx_payload_entry name file)
    string(APPEND VELYX_PAYLOAD_LINES
           "LANGUAGE 0x00, 0x00\n\"${name}\" VELYXFILE \"${file}\"\n")
    set(VELYX_PAYLOAD_LINES "${VELYX_PAYLOAD_LINES}" PARENT_SCOPE)
endfunction ()

foreach (font IN ITEMS SpaceGrotesk-Regular SpaceGrotesk-Medium SpaceGrotesk-Bold
                       JetBrainsMono-Regular JetBrainsMono-Medium)
    velyx_payload_entry("assets/fonts/${font}.ttf" "${CMAKE_SOURCE_DIR}/assets/fonts/${font}.ttf")
endforeach ()

foreach (doc IN ITEMS README.md template.json)
    velyx_payload_entry("assets/signatures/${doc}" "${CMAKE_SOURCE_DIR}/assets/signatures/${doc}")
endforeach ()

velyx_payload_entry("bin/Velyx.dll" "$<TARGET_FILE:Velyx>")

file(GENERATE OUTPUT "${VELYX_PAYLOAD}" CONTENT "${VELYX_PAYLOAD_LINES}")

add_executable(VelyxLauncher WIN32 ${VELYX_LAUNCHER_SOURCES} ${VELYX_LAUNCHER_HEADERS}
        "${VELYX_PAYLOAD}")

add_dependencies(VelyxLauncher Velyx)
set_source_files_properties("${VELYX_PAYLOAD}" PROPERTIES GENERATED TRUE)

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
