#include <format>

#include "storage/bytes.h"
#include "storage/partition.h"

namespace {
	constexpr std::uint8_t hex_value(char c) {
		if (c >= '0' && c <= '9')
			return static_cast<std::uint8_t>(c - '0');
		if (c >= 'A' && c <= 'F')
			return static_cast<std::uint8_t>(c - 'A' + 10);
		if (c >= 'a' && c <= 'f')
			return static_cast<std::uint8_t>(c - 'a' + 10);
		throw "not a hex digit"; // only reached while compiling, where it's a compile error
	}

	// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" as it's stored on disk, at compile time
	consteval storage::Guid guid(const char (&text)[37]) {
		// where each stored byte's two hex digits are in the text: the first three groups byte-swapped
		constexpr int at[16] = { 6, 4, 2, 0, 11, 9, 16, 14, 19, 21, 24, 26, 28, 30, 32, 34 };
		storage::Guid result;
		for (int i = 0; i < 16; i++)
			result.bytes[i] = static_cast<std::uint8_t>(hex_value(text[at[i]]) << 4 | hex_value(text[at[i] + 1]));
		return result;
	}

	struct t_gpt_type {
		storage::Guid type;
		std::string_view name;
	};

	// the uefi spec's, microsoft's, the discoverable partitions spec's and apple's best known ones
	constexpr t_gpt_type gpt_types[] = {
		{ guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"), "EFI system" },
		{ guid("024DEE41-33E7-11D3-9D69-0008C781F39F"), "MBR partition scheme" },
		{ guid("21686148-6449-6E6F-744E-656564454649"), "BIOS boot" },
		{ guid("E3C9E316-0B5C-4DB8-817D-F92DF00215AE"), "Microsoft reserved" },
		{ guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"), "Microsoft basic data" },
		{ guid("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"), "Windows recovery" },
		{ guid("5808C8AA-7E8F-42E0-85D2-E1E90434CFB3"), "Windows LDM metadata" },
		{ guid("AF9B60A0-1431-4F62-BC68-3311714A69AD"), "Windows LDM data" },
		{ guid("E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D"), "Windows Storage Spaces" },
		{ guid("0FC63DAF-8483-4772-8E79-3D69D8477DE4"), "Linux filesystem" },
		{ guid("4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"), "Linux root (x86-64)" },
		{ guid("933AC7E1-2EB4-4F13-B844-0E14E2AEF915"), "Linux home" },
		{ guid("BC13C2FF-59E6-4262-A352-B275FD6F7172"), "Linux extended boot" },
		{ guid("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"), "Linux swap" },
		{ guid("E6D6D379-F507-44C2-A23C-238F2A3DF928"), "Linux LVM" },
		{ guid("A19D880F-05FC-4D3B-A006-743F0F84911E"), "Linux RAID" },
		{ guid("48465300-0000-11AA-AA11-00306543ECAC"), "Apple HFS+" },
		{ guid("7C3457EF-0000-11AA-AA11-00306543ECAC"), "Apple APFS" },
		{ guid("426F6F74-0000-11AA-AA11-00306543ECAC"), "Apple boot" },
	};
}

bool storage::Guid::is_zero() const noexcept {
	for (std::uint8_t b : bytes)
		if (b != 0)
			return false;
	return true;
}

std::string storage::to_string(const Guid& g) {
	const auto& b = g.bytes;
	return std::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
		b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

std::string_view storage::to_string(PartitionScheme scheme) noexcept {
	switch (scheme) {
	case PartitionScheme::none: return "none";
	case PartitionScheme::mbr: return "MBR";
	case PartitionScheme::gpt: return "GPT";
	}
	return "?";
}

std::string_view storage::to_string(FileSystem filesystem) noexcept {
	switch (filesystem) {
	case FileSystem::unknown: return "unknown";
	case FileSystem::fat12_16: return "FAT12/16";
	case FileSystem::fat32: return "FAT32";
	case FileSystem::exfat: return "exFAT";
	case FileSystem::ntfs: return "NTFS";
	case FileSystem::ext: return "ext2/3/4";
	case FileSystem::bitlocker: return "BitLocker";
	case FileSystem::luks: return "LUKS";
	case FileSystem::linux_swap: return "Linux swap";
	}
	return "?";
}

std::string_view storage::to_string(Diagnostic::Severity severity) noexcept {
	switch (severity) {
	case Diagnostic::Severity::info: return "info";
	case Diagnostic::Severity::warning: return "warning";
	case Diagnostic::Severity::error: return "error";
	}
	return "?";
}

bool storage::has(const std::vector<Diagnostic>& diagnostics, Diagnostic::Severity severity) noexcept {
	for (const Diagnostic& diagnostic : diagnostics)
		if (diagnostic.severity == severity)
			return true;
	return false;
}

std::string_view storage::mbr_type_name(std::uint8_t type) noexcept {
	switch (type) {
	case 0x01: return "FAT12";
	case 0x04: case 0x06: case 0x0E: return "FAT16";
	case 0x05: case 0x0F: return "Extended";
	case 0x07: return "NTFS / exFAT";
	case 0x0B: case 0x0C: return "FAT32";
	case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E: return "Hidden FAT";
	case 0x17: return "Hidden NTFS / exFAT";
	case 0x27: return "Windows recovery";
	case 0x42: return "Windows dynamic disk";
	case 0x82: return "Linux swap";
	case 0x83: return "Linux";
	case 0x85: return "Linux extended";
	case 0x8E: return "Linux LVM";
	case 0xA5: return "FreeBSD";
	case 0xA6: return "OpenBSD";
	case 0xAF: return "Apple HFS+";
	case 0xEE: return "GPT protective";
	case 0xEF: return "EFI system";
	case 0xFD: return "Linux RAID";
	}
	return {};
}

std::string_view storage::gpt_type_name(const Guid& type) noexcept {
	for (const t_gpt_type& known : gpt_types)
		if (known.type == type)
			return known.name;
	return {};
}

storage::FileSystem storage::detect_filesystem(std::span<const std::byte> first_bytes) noexcept {
	ByteReader r(first_bytes);
	const bool boot_signature = r.size() >= 512 && r.le16(510) == 0xAA55;

	// encrypted volumes first: bitlocker's boot sector also looks like a fat or ntfs one
	if (r.matches(3, "-FVE-FS-"))
		return FileSystem::bitlocker;
	if (r.matches(0, std::string_view("LUKS\xBA\xBE", 6)))
		return FileSystem::luks;
	if (r.matches(3, "NTFS    "))
		return FileSystem::ntfs;
	if (r.matches(3, "EXFAT   "))
		return FileSystem::exfat;
	if (boot_signature && r.matches(82, "FAT32   "))
		return FileSystem::fat32;
	if (boot_signature && (r.matches(54, "FAT12   ") || r.matches(54, "FAT16   ") || r.matches(54, "FAT     ")))
		return FileSystem::fat12_16;
	// ext's magic is only 2 bytes, so the block size (1 KiB << log, at most 64 KiB) has to make sense too
	if (r.size() >= 1024 + 64 && r.le16(1024 + 56) == 0xEF53 && r.le32(1024 + 24) <= 6)
		return FileSystem::ext;
	if (r.matches(4086, "SWAPSPACE2") || r.matches(4086, "SWAP-SPACE"))
		return FileSystem::linux_swap;
	return FileSystem::unknown;
}

storage::Result<std::unique_ptr<storage::PartitionView>> storage::PartitionView::open(IBlockSource& parent, std::uint64_t offset, std::uint64_t size) noexcept {
	const std::optional<std::uint64_t> end = checked_add(offset, size);
	if (!end)
		return fail(Errc::overflow, "opening a partition: its end doesn't fit in 64 bits");
	if (*end > parent.size_bytes())
		return fail(Errc::out_of_range, "opening a partition: it runs past the end of the disk");
	std::unique_ptr<PartitionView> view(new (std::nothrow) PartitionView(parent, offset, size));
	if (!view)
		return fail(Errc::out_of_memory, "opening a partition");
	return view;
}

storage::Result<std::size_t> storage::PartitionView::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	if (offset > size_)
		return fail(Errc::out_of_range, "reading a partition: the offset is past its end");
	if (output.size() > size_ - offset)
		output = output.first(static_cast<std::size_t>(size_ - offset));
	if (output.empty())
		return std::size_t(0);
	return parent_.read_at(offset_ + offset, output); // offset_ + size_ fits, open() checked
}

storage::Result<std::unique_ptr<storage::PartitionView>> storage::open_partition(IBlockSource& disk, const Partition& partition) noexcept {
	if (!partition.inside_disk)
		return fail(Errc::out_of_range, "opening a partition: it runs past the end of the disk");
	return PartitionView::open(disk, partition.offset_bytes, partition.size_bytes);
}
