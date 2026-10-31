# the first meaningful demo, end to end: fenrir-cli recover copies the deleted (or with EXTRA=--carved, carved) files
# out of IMAGE into a fresh directory, and every one of them has to hash (with cmake's sha-256, not fenrir's) the same as
# the original in ORIGINALS, which fenrir-make-fixtures wrote from what it put on the disk. the image's hash and time
# can't change.
# usage: cmake -DCLI=<fenrir-cli> -DIMAGE=<image> -DORIGINALS=<dir> -DOUT=<dir> [-DEXTRA=--carved] [-DMIN=n] -P check_recover.cmake
if(NOT DEFINED MIN)
    set(MIN 7)
endif()
file(REMOVE_RECURSE ${OUT})
file(SHA256 ${IMAGE} image_before)
file(TIMESTAMP ${IMAGE} time_before "%Y-%m-%dT%H:%M:%S")

execute_process(COMMAND ${CLI} recover ${IMAGE} ${OUT} ${EXTRA} OUTPUT_VARIABLE output RESULT_VARIABLE result)
message(STATUS "${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fenrir-cli recover failed: ${result}")
endif()

file(GLOB_RECURSE originals RELATIVE ${ORIGINALS} ${ORIGINALS}/*)
list(LENGTH originals count)
if(count LESS MIN)
    message(FATAL_ERROR "only ${count} originals in ${ORIGINALS}")
endif()
foreach(name ${originals})
    if(NOT EXISTS ${OUT}/${name})
        message(FATAL_ERROR "not recovered: ${name}")
    endif()
    file(SHA256 ${ORIGINALS}/${name} want)
    file(SHA256 ${OUT}/${name} got)
    if(NOT want STREQUAL got)
        message(FATAL_ERROR "${name}: recovered ${got}, the original is ${want}")
    endif()
endforeach()

file(SHA256 ${IMAGE} image_after)
file(TIMESTAMP ${IMAGE} time_after "%Y-%m-%dT%H:%M:%S")
if(NOT image_after STREQUAL image_before OR NOT time_after STREQUAL time_before)
    message(FATAL_ERROR "the image changed")
endif()
message(STATUS "${count} files recovered, each the same as its original. the image is unchanged")
