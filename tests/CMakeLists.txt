# doctest, downloaded and hash checked like the ui's dependencies. only its header is used
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(doctest)

add_executable(storage_tests
    unit/main.cpp
    unit/test_result.cpp
    unit/test_file_image_source.cpp
    unit/test_hash.cpp
    unit/test_sources.cpp
    unit/test_partitions.cpp
    unit/test_fat32.cpp
)
target_include_directories(storage_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
target_include_directories(storage_tests PRIVATE ${PROJECT_SOURCE_DIR}/tools) # make_fixtures/fixtures.h
target_compile_definitions(storage_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS) # its own std forward declarations break on newer msvc
target_link_libraries(storage_tests PRIVATE storage_core)
fenrir_warnings(storage_tests)
add_test(NAME storage_tests COMMAND storage_tests)

# nothing in the engine can write yet, and this keeps it that way until a reviewed write path is let in on purpose
add_test(NAME no_write_apis COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_write_apis.cmake)

# the cli on a 4 KiB image made while configuring, and on one that isn't there
string(REPEAT "fenrir" 682 fixture_text) # 4092 bytes
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/fixtures/four_kib.img "${fixture_text}four")
add_test(NAME cli_reports_size COMMAND storage_cli info ${CMAKE_CURRENT_BINARY_DIR}/fixtures/four_kib.img)
set_tests_properties(cli_reports_size PROPERTIES PASS_REGULAR_EXPRESSION "size: +4096 bytes")
add_test(NAME cli_refuses_missing_image COMMAND storage_cli info ${CMAKE_CURRENT_BINARY_DIR}/fixtures/missing.img)
set_tests_properties(cli_refuses_missing_image PROPERTIES WILL_FAIL TRUE)

# the disk images from tools/make_fixtures/fixtures.h, written before the cli tests that read them
add_executable(make_fixtures ${PROJECT_SOURCE_DIR}/tools/make_fixtures/main.cpp ${PROJECT_SOURCE_DIR}/tools/make_fixtures/fixtures.h ${PROJECT_SOURCE_DIR}/tools/make_fixtures/fat32.h)
set_target_properties(make_fixtures PROPERTIES OUTPUT_NAME fenrir-make-fixtures)
fenrir_warnings(make_fixtures)
set(images ${CMAKE_CURRENT_BINARY_DIR}/fixtures/images)
add_test(NAME make_fixtures COMMAND make_fixtures ${images})
set_tests_properties(make_fixtures PROPERTIES FIXTURES_SETUP disk_images)

# cli_<name>: fenrir-cli <args>, passing when its output matches every regex
function(cli_test name args)
    add_test(NAME cli_${name} COMMAND storage_cli ${args})
    set_tests_properties(cli_${name} PROPERTIES FIXTURES_REQUIRED disk_images)
    if(ARGN)
        set_tests_properties(cli_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARGN}")
    endif()
endfunction()
# one regex that has to match the whole listing in order (ctest passes on any one of several). special characters go
# in [] rather than behind a backslash, the backslash doesn't survive the trip into CTestTestfile.cmake. cmake's . also
# matches a newline, and (.|\n)* backtracks for minutes on a listing it doesn't match
cli_test(partitions_gpt "partitions;${images}/gpt.img"
    "scheme: +GPT, 512 byte sectors.*\n +1 +2048 +2048 +1.00 MiB +EFI system +FAT32 +EFI system partition\n +2 +4096 +8192 +4.00 MiB +Microsoft basic data +NTFS .*\n +3 +12288 +4063 +.* +Linux filesystem +ext2/3/4 +Linux\n")
cli_test(partitions_gpt_4k "partitions;${images}/gpt_4k.img" "scheme: +GPT, 4096 byte sectors.*\n +1 +256 +256 +1.00 MiB +EFI system")
cli_test(partitions_mbr "partitions;${images}/mbr.img"
    "scheme: +MBR.*\n +1[*] +2048 +4096 .*FAT32 +FAT32\n +2 +6144 +4096 .*Linux +ext2/3/4\n +5 +12288 +1024 .*NTFS / exFAT +NTFS\n +6 +15360 +1024 .*Linux +unknown\n")
cli_test(partitions_bad_primary "partitions;${images}/gpt_bad_primary.img" "warning: the primary GPT isn't valid [(]its header CRC doesn't match.*the backup at sector 16383 is used instead")
cli_test(partitions_truncated "partitions;${images}/gpt_truncated.img" " 3 +12288 +4063 .*[(]past the end[)]\n.*warning: the backup GPT isn't valid")
cli_test(partitions_superfloppy "partitions;${images}/superfloppy.img" "scheme: +none, the whole disk is one FAT32 filesystem")
cli_test(partitions_blank "partitions;${images}/blank.img" "scheme: +none\n.*sector 0 is empty")
cli_test(partitions_forced_sector_size "partitions;${images}/mbr.img;--sector-size;4096" "scheme: +MBR, 4096 byte sectors")
cli_test(hex_boot_signature "hex;${images}/mbr.img;0x1fe;2" "^00000000000001fe  55 aa +U[.]\n$")
cli_test(hex_refuses_past_end "hex;${images}/blank.img;65536;16")
set_tests_properties(cli_hex_refuses_past_end PROPERTIES WILL_FAIL TRUE)

# fenrir-cli's sha-256 against cmake's own, and the image's hash and time unchanged by a scan
add_test(NAME cli_hash_matches_and_scan_changes_nothing
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:storage_cli> -DIMAGE=${images}/gpt.img -P ${CMAKE_CURRENT_SOURCE_DIR}/check_hash_unchanged.cmake)
set_tests_properties(cli_hash_matches_and_scan_changes_nothing PROPERTIES FIXTURES_REQUIRED disk_images)

# fat32: the listing, and the deleted files recovered and compared with their originals
cli_test(files_fat32 "files;${images}/fat32.img;--why"
    "volume: +partition 1, FAT32, 512 byte clusters.*label FENRIR TEST\n.* +1 +partial +2500 +deleted +/fragmented.dat\n.*intact +20000 +deleted +/DCIM/Holiday photo 2026.jpg\n.*unrecoverable +4000 +deleted +/overwritten.bin\n")
cli_test(files_needs_fat32 "files;${images}/gpt.img;--partition;2")
set_tests_properties(cli_files_needs_fat32 PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_recover_matches_originals
    COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:storage_cli> -DIMAGE=${images}/fat32.img -DORIGINALS=${images}/fat32_originals
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/recovered -P ${CMAKE_CURRENT_SOURCE_DIR}/check_recover.cmake)
set_tests_properties(cli_recover_matches_originals PROPERTIES FIXTURES_REQUIRED disk_images)
