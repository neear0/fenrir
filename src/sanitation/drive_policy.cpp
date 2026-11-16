// which drives the drive eraser may touch, and what it says about them: queries only, no writing here

#include <algorithm>
#include <format>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include "storage/drive_erase.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;

	fs::path own_executable() {
#ifdef _WIN32
		wchar_t path[MAX_PATH + 1]{};
		if (GetModuleFileNameW(nullptr, path, MAX_PATH))
			return fs::path(path);
#endif
		return {};
	}
}

std::vector<std::string> storage::drive_blockers(const DiskInfo& disk, const std::vector<std::uint32_t>& protected_disks) {
	std::vector<std::string> out;
	if (!disk.error.empty())
		out.push_back("what it is couldn't all be read (" + disk.error + "): a drive that can't be identified isn't erased");
	if (disk.bus != "USB")
		out.push_back(std::format("it's attached over {}, not USB: this build only erases USB drives", disk.bus.empty() ? "an unknown bus" : disk.bus));
	if (disk.system)
		out.push_back("Windows is on it");
	if (disk.spanned_volume)
		out.push_back("a volume on it is on another drive too: erasing this one would wreck that volume");
	if (std::ranges::find(protected_disks, disk.number) != protected_disks.end())
		out.push_back("fenrir itself, its temp folder or a folder it was told to keep (a report's) is on it");
	if (disk.size == 0)
		out.push_back("it has no size: there may be no medium in it");
	if (disk.sector_size == 0 || (disk.sector_size & (disk.sector_size - 1)) != 0 || disk.sector_size > 65536 || (disk.sector_size != 0 && disk.size % disk.sector_size != 0))
		out.push_back("its sector size isn't usable");
	if (disk.serial.empty())
		out.push_back("it has no serial number: the confirmation and the checks before writing need one");
	return out;
}

storage::Media storage::drive_media(const DiskInfo& disk) noexcept {
	return disk.rotational.value_or(false) ? Media::hdd : Media::usb_flash;
}

storage::TargetIdentity storage::drive_identity(const DiskInfo& disk) {
	TargetIdentity id;
	id.media = drive_media(disk);
	id.model = disk.vendor.empty() ? disk.model : disk.vendor + " " + disk.model;
	id.serial = disk.serial;
	id.path = disk.path;
	id.size = disk.size;
	return id;
}

storage::Result<storage::Target> storage::DriveOverwriteBackend::inspect() {
	Result<std::vector<DiskInfo>> disks = list_disks();
	if (!disks)
		return std::unexpected(disks.error());
	const auto disk = std::ranges::find(*disks, number_, &DiskInfo::number);
	if (disk == disks->end())
		return fail(Errc::not_found, "erasing: there's no drive with that number");

	// the drives that must survive: where fenrir runs from, its temp folder, the folder it was started in, and every
	// path it was told to keep. one that can't be placed on a drive blocks every drive: fail closed
	std::vector<std::uint32_t> keep_disks;
	std::vector<std::string> blockers;
	std::vector<fs::path> keep = keep_;
	std::error_code error;
	keep.push_back(own_executable());
	keep.push_back(fs::temp_directory_path(error));
	keep.push_back(fs::current_path(error));
	for (const fs::path& path : keep) {
		if (path.empty()) {
			blockers.push_back("where fenrir or its temp folder is couldn't be found, so no drive can be ruled safe to erase");
			continue;
		}
		Result<std::vector<std::uint32_t>> on = disks_of(path);
		if (!on) {
			const std::u8string text = path.u8string();
			blockers.push_back(std::format("which drive {} is on can't be told, so no drive can be ruled safe to erase", std::string(reinterpret_cast<const char*>(text.data()), text.size())));
			continue;
		}
		for (std::uint32_t d : *on)
			if (std::ranges::find(keep_disks, d) == keep_disks.end())
				keep_disks.push_back(d);
	}
	for (std::string& b : drive_blockers(*disk, keep_disks))
		blockers.push_back(std::move(b));

	Target t;
	t.identity = drive_identity(*disk);
	t.capabilities.methods = { Method::overwrite };
	t.capabilities.sector_size = disk->sector_size;
	// windows being on it is a blocker already (drive_blockers()), not said twice
	t.capabilities.blockers = std::move(blockers);
	t.capabilities.volumes = disk->volumes;
	return t;
}
