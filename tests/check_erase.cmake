# fenrir-cli erase: plans say what they may be called and what to type, nothing runs without the exact confirmation,
# a lying simulated drive fails its read back, a test image file ends up as zeros with a JSON report next to it, an
# unsupported method is refused with nothing else tried, and a real drive is refused outright.
# usage: cmake -DCLI=<fenrir-cli> -DOUT=<scratch dir> -P check_erase.cmake
foreach(var CLI OUT)
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

# plans
cli(TRUE erase plan sim:nvme-ssd --method nvme-sanitize-crypto)
expect("${out}" "at most: +purge")
expect("${out}" "can stop: +no")
expect("${out}" "nothing was done[.] to do it: .*--confirm \"ERASE 000003\"")
cli(FALSE erase plan sim:usb-flash --method nvme-sanitize-block)
expect("${out}" "refused, nothing was done")
expect("${out}" "nothing else is tried instead")

# a simulated drive: not without the exact confirmation, then verified, with a report
cli(FALSE erase run sim:hdd)
expect("${out}" "needs --confirm \"ERASE 000001\"")
cli(FALSE erase run sim:hdd --confirm "erase 000001")
expect("${out}" "isn't the confirmation")
cli(TRUE erase run sim:hdd --confirm "ERASE 000001" --report ${OUT}/sim)
expect("${out}" "status: +verified")
file(READ "${OUT}/sim/fenrir erase report.json" json)
expect("${json}" "\"status\": \"verified\"")
expect("${json}" "\"validation\": \"not assessed")

# firmware that lies is caught by reading back
cli(FALSE erase run sim:nvme-ssd --method nvme-sanitize-block --fault lies --confirm "ERASE 000003")
expect("${out}" "status: +failed")

# a test image file
file(WRITE ${OUT}/disposable.img "fenrir test image, to be erased: 0123456789abcdef")
cli(TRUE erase plan ${OUT}/disposable.img)
expect("${out}" "at most: +none")
expect("${out}" "not for sanitizing anything")
cli(TRUE erase run ${OUT}/disposable.img --confirm "ERASE disposable.img" --report ${OUT}/image)
expect("${out}" "status: +verified")
file(READ ${OUT}/disposable.img erased HEX)
if(NOT erased MATCHES "^(00)+$")
    message(FATAL_ERROR "the test image isn't all zeros: ${erased}")
endif()

# a drive that isn't USB (or holds windows or fenrir) is refused at the plan, every reason given. only planned here:
# a test never asks to run an erase of a real drive, whatever the build
cli(FALSE erase plan "\\\\.\\PhysicalDrive0")
expect("${out}" "refused, nothing was done")
