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
)
target_include_directories(storage_tests SYSTEM PRIVATE ${doctest_SOURCE_DIR})
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
