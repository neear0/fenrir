# fails when the storage engine or the cli uses an api that can write to a file or a device. until the reviewed write paths
# exist (exports to a separate drive, erasure of disposable image files), nothing in src/ or include/ may write anywhere.
# the ui (ui/, ImGui) isn't scanned: it never touches storage, it only talks to storage_core.
# usage: cmake -DROOT=<source dir> -P check_no_write_apis.cmake
if(NOT ROOT)
    message(FATAL_ERROR "pass -DROOT=<source dir>")
endif()

file(GLOB_RECURSE sources ${ROOT}/src/*.cpp ${ROOT}/src/*.h ${ROOT}/include/*.h)
set(forbidden
    WriteFile DeviceIoControl GENERIC_WRITE GENERIC_ALL FILE_WRITE_DATA FILE_APPEND_DATA CreateFileMapping MapViewOfFile
    SetEndOfFile SetFileValidData DeleteFile MoveFile CopyFile
    O_WRONLY O_RDWR O_CREAT O_TRUNC pwrite ioctl mmap ftruncate unlink
    fopen ofstream fstream "filesystem::remove" "filesystem::rename" "filesystem::copy" "filesystem::resize_file"
)

set(found "")
foreach(source ${sources})
    file(READ ${source} text)
    foreach(word ${forbidden})
        string(FIND "${text}" "${word}" at)
        if(NOT at EQUAL -1)
            file(RELATIVE_PATH relative ${ROOT} ${source})
            list(APPEND found "${relative}: ${word}")
        endif()
    endforeach()
endforeach()

list(LENGTH sources count)
if(found)
    list(JOIN found "\n  " report)
    message(FATAL_ERROR "write capable apis in the read-only engine:\n  ${report}")
endif()
message(STATUS "no write capable apis in ${count} files")
