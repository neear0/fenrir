#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "block_source.h"

// physical drives, read-only: listing them (no administrator rights needed) and reading one (administrator rights
// needed, windows only so far). nothing here opens a drive for writing, or sends it anything but the queries
// tests/check_no_write_apis.cmake allows: its description, its size and geometry, which disk a volume is on
namespace storage {
	struct DiskInfo {
		std::uint32_t number = 0;          // \\.\PhysicalDrive<number>
		std::filesystem::path path;
		std::string vendor, model, serial; // utf-8, as the drive reports them (any of them can be empty)
		std::string bus;                   // "USB", "SATA", "NVMe"...
		bool removable = false;            // what the drive says, a usb hard disk often says it isn't
		std::optional<bool> rotational;    // it spins (seeks cost time): a hard disk. nothing when it doesn't say
		std::uint64_t size = 0;            // bytes
		std::uint32_t sector_size = 0;     // logical, what reads have to be aligned to
		std::vector<std::string> volumes;  // its volumes' mount points ("C:\"), or their names when they have none
		bool system = false;               // windows, or the volume it boots from, is on it
		std::string error;                 // when something about it couldn't be read, what
	};

	// every physical drive the os has, in number order. unsupported where there's no way to list them yet (not windows)
	Result<std::vector<DiskInfo>> list_disks() noexcept;

	// \\.\PhysicalDrive<n> (any case), the drive number. nothing for anything else
	std::optional<std::uint32_t> physical_drive_number(const std::filesystem::path& path) noexcept;

	// a whole physical drive, opened for reading only. reads of any offset and length are turned into whole sectors
	// underneath, so it reads like any other source. access_denied without administrator rights
	class DiskSource final : public IBlockSource {
	public:
		// expected: what list_disks() said about it, refused with mismatch when the drive at that number has another
		// size or serial now (unplugged and something else plugged in)
		static Result<std::unique_ptr<DiskSource>> open(std::uint32_t number, const DiskInfo* expected = nullptr) noexcept;
		~DiskSource() override;

		const DiskInfo& info() const noexcept { return info_; }

		std::uint64_t size_bytes() const noexcept override { return info_.size; }
		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override;
		std::optional<std::uint32_t> physical_disk() const noexcept override { return info_.number; }

	private:
		DiskSource() = default;
		struct t_impl;
		std::unique_ptr<t_impl> impl_;
		DiskInfo info_;
	};

	// the physical drives the volume path is on is spread over (a path that doesn't exist yet counts as the
	// directory it would be in). empty for a network share. unsupported when it can't be told
	Result<std::vector<std::uint32_t>> disks_of(const std::filesystem::path& path) noexcept;

	// for anything that writes while reading a drive: fails (invalid_argument) when destination is on that drive, and
	// (unsupported) when which drive it's on can't be told. writing to the drive being recovered could overwrite what's
	// being recovered
	Result<void> check_destination(const std::filesystem::path& destination, std::uint32_t source_disk) noexcept;
}
