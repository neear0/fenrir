# fenrir's own targets: the storage engine and the cli. included from the root CMakeLists.txt after the global hardening
# flags, so these get the static runtime, cfg and cet like the ui does

# warnings as errors on our code only, CelosUI and the downloaded dependencies keep their own
function(fenrir_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
    endif()
endfunction()

# storage_core: recovery and (later) erasure, plain c++23. nothing in it may include CelosUI, ImGui or graphics headers
add_library(storage_core STATIC
    include/storage/result.h
    include/storage/block_source.h
    include/storage/bytes.h
    include/storage/jobs.h
    include/storage/hash.h
    include/storage/hex_dump.h
    include/storage/file_image_source.h
    include/storage/memory_source.h
    include/storage/fault_injecting_source.h
    include/storage/partition.h
    include/storage/diagnostic.h
    include/storage/recovery_candidate.h
    include/storage/fat32.h
    include/storage/exporter.h
    src/core/error.cpp
    src/core/hash.cpp
    src/core/hex_dump.cpp
    src/io/block_source.cpp
    src/io/fault_injecting_source.cpp
    src/partition/partition.cpp
    src/partition/scan.cpp
    src/fs/fat32/scan.cpp
    src/export/exporter.cpp
)
if(WIN32)
    target_sources(storage_core PRIVATE src/io/file_image_source_win32.cpp)
else()
    target_sources(storage_core PRIVATE src/io/file_image_source_posix.cpp)
    target_compile_definitions(storage_core PUBLIC _FILE_OFFSET_BITS=64)
endif()
target_include_directories(storage_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(storage_core PUBLIC cxx_std_23)
fenrir_warnings(storage_core)

# fenrir-cli: read-only debug and test commands
add_executable(storage_cli src/cli/main.cpp)
target_link_libraries(storage_cli PRIVATE storage_core)
set_target_properties(storage_cli PROPERTIES OUTPUT_NAME fenrir-cli)
fenrir_warnings(storage_cli)
if(MSVC)
    # the same dll loading rule as the ui, imported dlls only come from system32
    target_link_options(storage_cli PRIVATE $<$<NOT:$<CONFIG:Debug>>:/DEPENDENTLOADFLAG:0x800>)
endif()
