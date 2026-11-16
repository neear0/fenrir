# fenrir-cli image: a copy of a clean image is the same file, with a map that says so; a second run goes on from that
# map and changes nothing; a name that's taken is never written over; and an image that's itself missing a part (its
# map says so) gives a copy whose map has that part as bad, not as the zeros the image has there.
# usage: cmake -DCLI=<fenrir-cli> -DIMAGE=<fixture> -DOUT=<scratch dir> -P check_image.cmake
foreach(var CLI IMAGE OUT)
    if(NOT ${var})
        message(FATAL_ERROR "pass -D${var}=")
    endif()
endforeach()
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})

function(cli expect_ok)
    execute_process(COMMAND ${CLI} ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect_ok AND NOT result EQUAL 0)
        message(FATAL_ERROR "fenrir-cli ${ARGN} failed (${result}):\n${out}\n${err}")
    elseif(NOT expect_ok AND result EQUAL 0)
        message(FATAL_ERROR "fenrir-cli ${ARGN} should have failed:\n${out}")
    endif()
    set(out "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect text regex)
    if(NOT text MATCHES "${regex}")
        message(FATAL_ERROR "expected ${regex} in:\n${text}")
    endif()
endfunction()

# a clean copy
cli(TRUE image ${IMAGE} ${OUT}/copy.img)
expect("${out}" "done, all of it copied")
file(SHA256 ${IMAGE} original)
file(SHA256 ${OUT}/copy.img copied)
if(NOT original STREQUAL copied)
    message(FATAL_ERROR "the copy isn't the same as the image")
endif()
file(SIZE ${IMAGE} size)
cli(TRUE map ${OUT}/copy.map)
expect("${out}" "state: +finished")
expect("${out}" "copied: +${size} bytes")

# again: it goes on from its map, nothing to do
cli(TRUE image ${IMAGE} ${OUT}/copy.img)
expect("${out}" "went on from the copy")
file(SHA256 ${OUT}/copy.img again)
if(NOT original STREQUAL again)
    message(FATAL_ERROR "running it again changed the copy")
endif()

# a file that's there without a map is left alone
file(WRITE ${OUT}/taken.img "mine")
cli(FALSE image ${IMAGE} ${OUT}/taken.img)
expect("${out}" "already there")
file(READ ${OUT}/taken.img taken)
if(NOT taken STREQUAL "mine")
    message(FATAL_ERROR "a file that was already there was written over")
endif()

# an image with a map that says one sector of it, at 1 MiB, couldn't be read
file(COPY_FILE ${OUT}/copy.img ${OUT}/holed.img)
math(EXPR rest "${size} - 0x100200" OUTPUT_FORMAT HEXADECIMAL)
file(WRITE ${OUT}/holed.map "# made by hand\n0x0 +\n0x0 0x100000 +\n0x100000 0x200 -\n0x100200 ${rest} +\n")
cli(TRUE info ${OUT}/holed.img)
expect("${out}" "map: .*512 bytes bad")
cli(FALSE hex ${OUT}/holed.img 0x100000 16)
expect("${out}" "not in the image")
cli(TRUE image ${OUT}/holed.img ${OUT}/from_holed.img --map ${OUT}/from_holed.log)
expect("${out}" "bad: +512 bytes")
expect("${out}" "done[.] what wasn't copied")
cli(TRUE map ${OUT}/from_holed.log)
expect("${out}" "bad areas:.*\n +1048576 +512\n")
