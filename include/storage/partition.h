#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "block_source.h"

// partition tables (mbr with extended partitions, gpt) read from a disk or disk image, checked against everything that
// can be checked: signatures, crcs, the backup gpt, boundaries, overlaps. nothing here writes, and a table that's
// damaged gives a map with diagnostics saying what's wrong rather than an error
namespace storage {
	// as gpt stores it: the first three fields little-endian, the last 8 bytes in order
	struct Guid {
		std::array<std::uint8_t, 16> bytes{};

		bool is_zero() const noexcept;
		friend bool operator==(const Guid&, const Guid&) = default;
	};
	std::string to_string(const Guid& guid); // "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

	enum class PartitionScheme : std::uint8_t {
		none, // no partition table: an empty or unformatted disk, or one filesystem on the whole disk (see whole_disk)
		mbr,
		gpt,
	};
	std::string_view to_string(PartitionScheme scheme) noexcept;

	// what the first sectors of a partition look like. a hint from signatures only: it says what the partition was
	// formatted as, not that the filesystem in it is intact
	enum class FileSystem : std::uint8_t {
		unknown, // nothing recognised: unformatted, damaged, or a filesystem fenrir doesn't know
		fat12_16,
		fat32,
		exfat,
		ntfs,
		ext,       // ext2, ext3 or ext4
		bitlocker, // encrypted, nothing in it can be read without unlocking it
		luks,      // encrypted, likewise
		linux_swap,
	};
	std::string_view to_string(FileSystem filesystem) noexcept;

	struct Partition {
		std::uint32_t number = 0;      // mbr: 1-4 primary, 5 and up logical, the way linux numbers them. gpt: entry index + 1
		std::uint64_t first_lba = 0;   // in sector_size units, from the start of the disk
		std::uint64_t sector_count = 0;
		std::uint64_t offset_bytes = 0; // first_lba and sector_count in bytes
		std::uint64_t size_bytes = 0;
		bool inside_disk = true;        // false when it runs past the end of the image, open_partition() refuses those

		// mbr only
		std::uint8_t mbr_type = 0;
		bool bootable = false;
		bool logical = false;           // inside the extended partition

		// gpt only
		Guid type_guid;
		Guid unique_guid;
		std::uint64_t attributes = 0;
		std::string name;               // utf-8, from the entry's utf-16 name

		std::string_view type_name;     // "EFI system", "Microsoft basic data", ... or empty when the type isn't known
		FileSystem filesystem = FileSystem::unknown;
	};

	struct Diagnostic {
		enum class Severity : std::uint8_t {
			info,    // worth knowing, nothing's wrong: e.g. a hybrid mbr
			warning, // something's damaged or odd, what's in the map is still usable
			error,   // something's damaged enough that part of the map is missing or can't be trusted
		};
		Severity severity;
		std::string message; // numbers and offsets only, never anything read from a filesystem
	};
	std::string_view to_string(Diagnostic::Severity severity) noexcept;

	struct PartitionMap {
		PartitionScheme scheme = PartitionScheme::none;
		std::uint32_t sector_size = 512;
		std::uint64_t disk_sectors = 0; // whole sectors in the source

		std::uint32_t mbr_signature = 0; // the 4 byte disk signature at 0x1B8, 0 when there's none
		Guid disk_guid;                  // gpt only
		bool gpt_from_backup = false;    // the primary gpt was damaged and the backup at the end of the disk was used
		std::uint64_t gpt_first_usable = 0, gpt_last_usable = 0;

		FileSystem whole_disk = FileSystem::unknown; // scheme none: a filesystem that starts at sector 0, if any

		std::vector<Partition> partitions; // in the order the table lists them
		std::vector<Diagnostic> diagnostics;

		bool has(Diagnostic::Severity severity) const noexcept;
	};

	struct PartitionScanOptions {
		// 0: 512, unless there's a gpt at 4096. a 4Kn disk's mbr only makes sense with 4096, and nothing in an mbr says
		// which, so an image of one without a gpt needs this set
		std::uint32_t sector_size = 0;
	};

	// reads the first sectors, the backup gpt at the end, extended boot records and each partition's first 4 KiB. only a
	// failed read of the first sectors is an error, any later one is a diagnostic and the map has what could be read
	Result<PartitionMap> scan_partitions(IBlockSource& source, const PartitionScanOptions& options = {}) noexcept;

	// a range of another source as a source of its own: what a filesystem scanner is given for one partition, it can't
	// read outside it. the parent has to outlive it
	class PartitionView final : public IBlockSource {
	public:
		static Result<std::unique_ptr<PartitionView>> open(IBlockSource& parent, std::uint64_t offset, std::uint64_t size) noexcept;

		std::uint64_t size_bytes() const noexcept override { return size_; }
		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override;

		std::uint64_t offset_in_parent() const noexcept { return offset_; }

	private:
		PartitionView(IBlockSource& parent, std::uint64_t offset, std::uint64_t size) noexcept : parent_(parent), offset_(offset), size_(size) {}

		IBlockSource& parent_;
		std::uint64_t offset_;
		std::uint64_t size_;
	};

	// the partition as a source, out_of_range when it runs past the end of the disk
	Result<std::unique_ptr<PartitionView>> open_partition(IBlockSource& disk, const Partition& partition) noexcept;

	// the filesystem signature in a partition's first bytes (4 KiB is enough for all of them, fewer just finds fewer)
	FileSystem detect_filesystem(std::span<const std::byte> first_bytes) noexcept;

	std::string_view mbr_type_name(std::uint8_t type) noexcept;
	std::string_view gpt_type_name(const Guid& type) noexcept;
}
