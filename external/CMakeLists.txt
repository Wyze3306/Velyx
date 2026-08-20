add_library(minhook STATIC
        minhook/src/buffer.c
        minhook/src/hook.c
        minhook/src/trampoline.c
        minhook/src/hde/hde32.c
        minhook/src/hde/hde64.c)

target_include_directories(minhook PUBLIC minhook/include)
set_target_properties(minhook PROPERTIES C_STANDARD 11 FOLDER external)

if (NOT MSVC)
    target_compile_options(minhook PRIVATE -w)
endif ()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
