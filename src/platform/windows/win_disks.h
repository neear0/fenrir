#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storage/disks.h"

// what disks.cpp knows about drives, for the drive writer next to it (drive_erase.cpp): queries only
namespace storage::win {
	// an open drive's description, size, sector size and whether it spins. false when its size couldn't be read
	bool describe_disk(void* handle, DiskInfo& info);
	// the \\?\Volume{...} names (no trailing backslash) of every volume on the drive. an error when one can't be placed
	// on a drive, or one on it is also on another: an erase would have to guess, or would take part of another drive
	Result<std::vector<std::wstring>> volumes_on(std::uint32_t disk);
}
