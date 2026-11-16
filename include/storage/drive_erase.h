#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disks.h"
#include "sanitize.h"

// erasing a real drive, P8: an overwrite of every sector, flushed, read back. nothing else: no ATA or NVMe command
// reaches a drive from fenrir (those methods exist only on the simulated drive). this build lets it near a drive only
// when it's attached over USB, doesn't hold windows, fenrir, its temp folder or a path it was told to keep, and is
// still the drive the plan was made for, all checked again right before the first write.
// the code that writes is compiled in only with -DFENRIR_DRIVE_ERASE=ON (windows): without it, execute() says
// unsupported and the build has no code that could write to a drive. only fenrir-cli, as administrator, uses it
namespace storage {
	// whether this build has the drive writer at all
	bool drive_erase_built() noexcept;

	// why a drive mustn't be erased, each reason in words, empty when none applies. protected_disks: the drives holding
	// what has to survive (fenrir, its temp folder, a report's folder...)
	std::vector<std::string> drive_blockers(const DiskInfo& disk, const std::vector<std::uint32_t>& protected_disks);

	// what an overwrite of it counts as: a hard disk when it says it spins, USB flash otherwise (a USB SSD is counted the
	// same way: there's no telling what its controller does)
	Media drive_media(const DiskInfo& disk) noexcept;

	// the identity a plan holds for a drive, from what the drive says about itself
	TargetIdentity drive_identity(const DiskInfo& disk);

	class DriveOverwriteBackend final : public ISanitizationBackend {
	public:
		// keep: paths whose drives must survive too (a report's folder), on top of fenrir's own and its temp folder's
		explicit DriveOverwriteBackend(std::uint32_t number, std::vector<std::filesystem::path> keep = {}) : number_(number), keep_(std::move(keep)) {}

		// what list_disks() says about it now, with every blocker found. not_found when there's no drive with that number
		Result<Target> inspect() override;
		// inspected again first: a different drive (mismatch), or one with any blocker (unsupported), means nothing is done.
		// then its volumes go offline (busy, nothing written, when one is in use), it's opened for writing and asked who
		// it is once more on that handle, and only then written
		Result<SanitizeReport> execute(const Authorization& approved, const CancellationToken* cancel = nullptr, ProgressSink* progress = nullptr) override;

	private:
		std::uint32_t number_;
		std::vector<std::filesystem::path> keep_;
	};
}
