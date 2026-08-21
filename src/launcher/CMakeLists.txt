file(GLOB_RECURSE VELYX_LAUNCHER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE VELYX_LAUNCHER_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# Velyx ships as a single executable: the client DLL, the interface fonts and
# the signature templates are linked in as resources and unpacked on first run.
set(VELYX_PAYLOAD_SOURCES "")
set(VELYX_PAYLOAD_TARGETS "")

file(GLOB VELYX_FONT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/fonts/*")
foreach (font IN LISTS VELYX_FONT_FILES)
    file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${font}")
    list(APPEND VELYX_PAYLOAD_SOURCES "${font}")
    list(APPEND VELYX_PAYLOAD_TARGETS "${relative}")
endforeach ()

foreach (doc IN ITEMS README.md template.json)
    list(APPEND VELYX_PAYLOAD_SOURCES "${CMAKE_SOURCE_DIR}/assets/signatures/${doc}")
    list(APPEND VELYX_PAYLOAD_TARGETS "assets/signatures/${doc}")
endforeach ()

# The resource compiler needs a plain path, so the freshly built DLL is staged
# next to the generated script first.
set(VELYX_PAYLOAD_DLL "${CMAKE_CURRENT_BINARY_DIR}/payload/Velyx.dll")
add_custom_command(OUTPUT "${VELYX_PAYLOAD_DLL}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:Velyx>" "${VELYX_PAYLOAD_DLL}"
        DEPENDS Velyx
        COMMENT "Staging Velyx.dll for the launcher payload"
        VERBATIM)
add_custom_target(velyx_payload_dll DEPENDS "${VELYX_PAYLOAD_DLL}")

list(APPEND VELYX_PAYLOAD_SOURCES "${VELYX_PAYLOAD_DLL}")
list(APPEND VELYX_PAYLOAD_TARGETS "bin/Velyx.dll")

# Resource names go through the compiler uppercased, so each entry gets a
# symbolic name and the manifest keeps the destination path it maps to.
set(VELYX_PAYLOAD_RC "")
set(VELYX_PAYLOAD_MAP "")
list(LENGTH VELYX_PAYLOAD_TARGETS VELYX_PAYLOAD_COUNT)
math(EXPR VELYX_PAYLOAD_LAST "${VELYX_PAYLOAD_COUNT} - 1")

foreach (index RANGE ${VELYX_PAYLOAD_LAST})
    list(GET VELYX_PAYLOAD_SOURCES ${index} entry_source)
    list(GET VELYX_PAYLOAD_TARGETS ${index} entry_target)
    string(APPEND VELYX_PAYLOAD_RC "VELYX_FILE_${index} VELYXFILE \"${entry_source}\"\n")
    string(APPEND VELYX_PAYLOAD_MAP "VELYX_FILE_${index}=${entry_target}\n")
endforeach ()

set(VELYX_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/velyx_payload.manifest")
file(WRITE "${VELYX_MANIFEST}" "${VELYX_PAYLOAD_MAP}")

set(VELYX_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/velyx_payload.rc")
file(WRITE "${VELYX_PAYLOAD}" "${VELYX_PAYLOAD_RC}VELYX_MANIFEST VELYXMAP \"${VELYX_MANIFEST}\"\n")

add_executable(VelyxLauncher WIN32 ${VELYX_LAUNCHER_SOURCES} ${VELYX_LAUNCHER_HEADERS}
        "${VELYX_PAYLOAD}")

add_dependencies(VelyxLauncher velyx_payload_dll)
set_source_files_properties("${VELYX_PAYLOAD}" PROPERTIES
        OBJECT_DEPENDS "${VELYX_PAYLOAD_DLL};${VELYX_MANIFEST};${VELYX_PAYLOAD_SOURCES}")

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
    # mingw looks for WinMain unless -municode points it at wWinMain.
    target_compile_options(VelyxLauncher PRIVATE -municode)
    target_link_options(VelyxLauncher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif ()
