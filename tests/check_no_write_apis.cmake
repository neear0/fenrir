# fails when the storage engine or the cli uses an api that can write to a file or a device. there are two reviewed write
# paths: the exporter in src/export/, which may create new files and directories and remove the files it created, and
# imaging in src/imaging/, which may create a new image file, write into it (or into one its map was made with), and
# replace its own map whole. nothing else, not even there. erasing a disposable test image (P7, src/sanitation/) goes
# through imaging's ImageFile::open_existing, so it has no write api of its own: nothing here lets it have one.
# the ui (ui/, ImGui) isn't scanned: it never touches storage, it only talks to storage_core.
# tools/ isn't either: the fixture generator writes test images, into the build directory ctest gives it.
# a text search, so it also catches names in comments: say "writing" in prose, not the api.
# usage: cmake -DROOT=<source dir> -P check_no_write_apis.cmake
cmake_policy(SET CMP0057 NEW) # if(IN_LIST)
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
    CREATE_ALWAYS OPEN_ALWAYS TRUNCATE_EXISTING SetFileInformationByHandle "link("
)
# creating new files and directories, and removing files: only the exporter
set(export_only ofstream "<fstream>" "ios::out" noreplace "create_director" "remove(")
set(export_dir ${ROOT}/src/export/)
# writing into the image it made, and replacing its map: only imaging. it can't truncate or replace anything else
set(imaging_only WriteFile GENERIC_WRITE MoveFile DeleteFile SetFileInformationByHandle O_WRONLY O_RDWR O_CREAT pwrite ftruncate "rename(" unlink "link(")
set(imaging_dir ${ROOT}/src/imaging/)
# querying drives: only src/platform/windows/, and only with these three, which change nothing: a drive's description,
# its size and sector size, and which drives a volume is on. drives are opened there with no access or read access
set(platform_only DeviceIoControl ioctl) # ioctl: <winioctl.h>, where the query codes are
set(platform_dir ${ROOT}/src/platform/windows/)
set(allowed_queries IOCTL_STORAGE_QUERY_PROPERTY IOCTL_DISK_GET_DRIVE_GEOMETRY_EX IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)

set(found "")
foreach(source ${sources})
    file(READ ${source} text)
    set(words ${forbidden})
    string(FIND "${source}" "${export_dir}" in_export)
    string(FIND "${source}" "${imaging_dir}" in_imaging)
    if(in_imaging EQUAL 0)
        list(REMOVE_ITEM words ${imaging_only})
    endif()
    string(FIND "${source}" "${platform_dir}" in_platform)
    if(in_platform EQUAL 0)
        list(REMOVE_ITEM words ${platform_only})
        string(REGEX MATCHALL "(IOCTL|FSCTL|SMART)_[A-Z0-9_]+" codes "${text}")
        foreach(code ${codes})
            if(NOT code IN_LIST allowed_queries)
                file(RELATIVE_PATH relative ${ROOT} ${source})
                list(APPEND found "${relative}: ${code}, not one of the queries allowed")
            endif()
        endforeach()
    endif()
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

# and imaging only creates files that aren't there yet, or opens ones that are without cutting them short: every
# CreateFileW says CREATE_NEW or OPEN_EXISTING, every O_CREAT comes with O_EXCL
file(GLOB imaging_sources ${imaging_dir}*.cpp)
foreach(source ${imaging_sources})
    file(RELATIVE_PATH relative ${ROOT} ${source})
    file(READ ${source} text)
    string(REPLACE ";" "," text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    foreach(line IN LISTS lines)
        if(line MATCHES "CreateFileW[(]" AND NOT line MATCHES "CREATE_NEW|OPEN_EXISTING")
            list(APPEND found "${relative}: a CreateFileW without CREATE_NEW or OPEN_EXISTING: ${line}")
        endif()
        if(line MATCHES "O_CREAT" AND NOT line MATCHES "O_EXCL")
            list(APPEND found "${relative}: an O_CREAT without O_EXCL: ${line}")
        endif()
    endforeach()
endforeach()

list(LENGTH sources count)
if(found)
    list(JOIN found "\n  " report)
    message(FATAL_ERROR "write capable apis outside the reviewed write paths:\n  ${report}")
endif()
message(STATUS "no write capable apis outside src/export/ and src/imaging/, and only the allowed drive queries, in ${count} files")
