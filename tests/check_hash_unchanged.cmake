# hashes IMAGE with cmake's sha-256 and with fenrir-cli hash and fails unless they agree, then scans its partitions and
# fails unless the hash and the modification time are still what they were.
# usage: cmake -DCLI=<fenrir-cli> -DIMAGE=<image> -P check_hash_unchanged.cmake
file(SHA256 ${IMAGE} before)
file(TIMESTAMP ${IMAGE} time_before "%Y-%m-%dT%H:%M:%S")

execute_process(COMMAND ${CLI} hash ${IMAGE} OUTPUT_VARIABLE cli_hash RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fenrir-cli hash failed: ${result}")
endif()
string(REGEX MATCH "sha256: +([0-9a-f]+)" match "${cli_hash}")
if(NOT CMAKE_MATCH_1 STREQUAL before)
    message(FATAL_ERROR "fenrir-cli says ${CMAKE_MATCH_1}, cmake says ${before}")
endif()

execute_process(COMMAND ${CLI} partitions ${IMAGE} OUTPUT_QUIET RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fenrir-cli partitions failed: ${result}")
endif()

file(SHA256 ${IMAGE} after)
file(TIMESTAMP ${IMAGE} time_after "%Y-%m-%dT%H:%M:%S")
if(NOT after STREQUAL before OR NOT time_after STREQUAL time_before)
    message(FATAL_ERROR "the image changed: ${before} ${time_before} -> ${after} ${time_after}")
endif()
message(STATUS "sha256 ${before}, the same before and after the scan")
