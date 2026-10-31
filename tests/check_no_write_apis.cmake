# fails when the storage engine or the cli uses an api that can write to a file or a device. the one reviewed write path is
# the exporter in src/export/: it may create new files and directories and remove the files it created, and nothing
# else, not even there. erasure of disposable image files (P7) will get its own entry here when it exists.
# the ui (ui/, ImGui) isn't scanned: it never touches storage, it only talks to storage_core.
# tools/ isn't either: the fixture generator writes test images, into the build directory ctest gives it.
# a text search, so it also catches names in comments: say "writing" in prose, not the api.
# usage: cmake -DROOT=<source dir> -P check_no_write_apis.cmake
if(NOT ROOT)
    message(FATAL_ERROR "pass -DROOT=<source dir>")
endif()

file(GLOB_RECURSE sources ${ROOT}/src/*.cpp ${ROOT}/src/*.h ${ROOT}/include/*.h)
# can write to a device, or to any file that's already there: nowhere in the engine
set(forbidden
    WriteFile DeviceIoControl GENERIC_WRITE GENERIC_ALL FILE_WRITE_DATA FILE_APPEND_DATA CreateFileMapping MapViewOfFile
    SetEndOfFile SetFileValidData DeleteFile MoveFile CopyFile ReplaceFile
    O_WRONLY O_RDWR O_CREAT O_TRUNC pwrite ioctl mmap ftruncate unlink
    fopen "std::fstream" "::fstream" basic_fstream wfstream "ios::app" "ios::trunc" "ios::ate" "rename(" "fs::copy" "filesystem::copy" "resize_file"
    "permissions(" "last_write_time(" "create_symlink" "create_hard_link"
)
# creating new files and directories, and removing files: only the exporter
set(export_only ofstream "<fstream>" "ios::out" noreplace "create_director" "remove(")
set(export_dir ${ROOT}/src/export/)

set(found "")
foreach(source ${sources})
    file(READ ${source} text)
    set(words ${forbidden})
    string(FIND "${source}" "${export_dir}" in_export)
    if(NOT in_export EQUAL 0)
        list(APPEND words ${export_only})
    endif()
    foreach(word ${words})
        string(FIND "${text}" "${word}" at)
        if(NOT at EQUAL -1)
            file(RELATIVE_PATH relative ${ROOT} ${source})
            list(APPEND found "${relative}: ${word}")
        endif()
    endforeach()
endforeach()

# and the exporter has to open its files with noreplace, so it can't ever replace one: every open() says so, and no
# ofstream opens a file in its constructor, where it couldn't
file(GLOB export_sources ${export_dir}*.cpp)
foreach(source ${export_sources})
    file(RELATIVE_PATH relative ${ROOT} ${source})
    file(READ ${source} text)
    string(REPLACE ";" "," text "${text}") # c++ is full of them, and they'd split the lines apart
    string(REPLACE "\n" ";" lines "${text}")
    foreach(line IN LISTS lines)
        if(line MATCHES "[.]open[(]" AND NOT line MATCHES "ios::noreplace")
            list(APPEND found "${relative}: an open() without noreplace: ${line}")
        endif()
        if(line MATCHES "ofstream [A-Za-z_]+[({]")
            list(APPEND found "${relative}: an ofstream opened by its constructor: ${line}")
        endif()
    endforeach()
endforeach()

list(LENGTH sources count)
if(found)
    list(JOIN found "\n  " report)
    message(FATAL_ERROR "write capable apis outside the reviewed write path:\n  ${report}")
endif()
message(STATUS "no write capable apis outside src/export/ in ${count} files")
