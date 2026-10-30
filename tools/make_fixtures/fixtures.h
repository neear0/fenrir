#pragma once

// disk images for tests, put together byte by byte from the mbr convention and the uefi spec's gpt layout (2.10, 5.2 and
// 5.3) without any of storage_core's code: the parser is checked against these, so they mustn't share its mistakes.
// that's why crc-32 and the guid encoding are written again here. used by storage_tests in memory and by
// fenrir-make-fixtures, which writes them out for the cli tests

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fixtures {
	using t_image = std::vector<std::byte>;
	constexpr std::size_t mib = std::size_t(1) << 20;

	inline void put_le(t_image& image, std::size_t at, std::uint64_t value, int width) {
		for (int i = 0; i < width; i++)
			image.at(at + i) = static_cast<std::byte>(value >> (8 * i));
	}
	inline void put8(t_image& image, std::size_t at, std::uint8_t value) { put_le(image, at, value, 1); }
	inline void put16(t_image& image, std::size_t at, std::uint16_t value) { put_le(image, at, value, 2); }
	inline void put32(t_image& image, std::size_t at, std::uint32_t value) { put_le(image, at, value, 4); }
	inline void put64(t_image& image, std::size_t at, std::uint64_t value) { put_le(image, at, value, 8); }
	inline void put_text(t_image& image, std::size_t at, std::string_view text) {
		for (std::size_t i = 0; i < text.size(); i++)
			image.at(at + i) = static_cast<std::byte>(text[i]);
	}

	// bit by bit, the slow way, so it can't share a table bug with the engine's
	inline std::uint32_t crc32(const t_image& image, std::size_t at, std::size_t length) {
		std::uint32_t crc = 0xFFFFFFFF;
		for (std::size_t i = 0; i < length; i++) {
			crc ^= std::to_integer<std::uint32_t>(image.at(at + i));
			for (int bit = 0; bit < 8; bit++)
				crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
		}
		return ~crc;
	}

	// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" into the 16 bytes at at: the first three groups little-endian
	inline void put_guid(t_image& image, std::size_t at, std::string_view text) {
		auto hex = [&](std::size_t i) {
			auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
			return static_cast<std::uint8_t>(digit(text.at(i)) * 16 + digit(text.at(i + 1)));
		};
		const std::uint32_t a = std::uint32_t(hex(0)) << 24 | std::uint32_t(hex(2)) << 16 | std::uint32_t(hex(4)) << 8 | hex(6);
		put32(image, at, a);
		put16(image, at + 4, static_cast<std::uint16_t>(hex(9) << 8 | hex(11)));
		put16(image, at + 6, static_cast<std::uint16_t>(hex(14) << 8 | hex(16)));
		const std::size_t rest[8] = { 19, 21, 24, 26, 28, 30, 32, 34 };
		for (int i = 0; i < 8; i++)
			put8(image, at + 8 + i, hex(rest[i]));
	}

	struct t_mbr_entry {
		std::uint8_t status = 0, type = 0;
		std::uint32_t first = 0, count = 0;
	};

	// the 4 entries and the boot signature of an mbr or an extended boot record at byte at
	inline void put_mbr(t_image& image, std::size_t at, const std::array<t_mbr_entry, 4>& entries, std::uint32_t disk_signature = 0) {
		if (disk_signature != 0)
			put32(image, at + 0x1B8, disk_signature);
		for (int i = 0; i < 4; i++) {
			const std::size_t e = at + 0x1BE + 16 * static_cast<std::size_t>(i);
			put8(image, e, entries[i].status);
			put8(image, e + 4, entries[i].type);
			put32(image, e + 8, entries[i].first);
			put32(image, e + 12, entries[i].count);
		}
		put8(image, at + 510, 0x55);
		put8(image, at + 511, 0xAA);
	}

	// just enough of each filesystem's first sector for its signature
	inline void put_fat32_boot(t_image& image, std::size_t at) {
		put8(image, at, 0xEB); put8(image, at + 1, 0x58); put8(image, at + 2, 0x90);
		put_text(image, at + 3, "MSDOS5.0");
		put16(image, at + 11, 512);
		put_text(image, at + 82, "FAT32   ");
		put8(image, at + 510, 0x55); put8(image, at + 511, 0xAA);
	}
	inline void put_ntfs_boot(t_image& image, std::size_t at) {
		put8(image, at, 0xEB); put8(image, at + 1, 0x52); put8(image, at + 2, 0x90);
		put_text(image, at + 3, "NTFS    ");
		put8(image, at + 510, 0x55); put8(image, at + 511, 0xAA);
	}
	inline void put_ext_superblock(t_image& image, std::size_t at) {
		put32(image, at + 1024 + 24, 2); // 4 KiB blocks
		put16(image, at + 1024 + 56, 0xEF53);
	}

	struct t_gpt_entry {
		std::string_view type, unique;
		std::uint64_t first = 0, last = 0, attributes = 0;
		std::u16string name;
	};

	struct t_gpt_layout {
		std::uint32_t sector_size = 512;
		std::uint32_t entry_count = 128;
		std::uint32_t entry_size = 128;
		std::string_view disk_guid = "5A1B3C4D-0E0F-4A1B-9C2D-3E4F5A6B7C8D";
	};

	constexpr std::string_view efi_system = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
	constexpr std::string_view basic_data = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
	constexpr std::string_view linux_filesystem = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";

	// the sectors a gpt leaves usable on an image this size
	inline std::uint64_t gpt_first_usable(const t_gpt_layout& layout) {
		return 2 + (std::uint64_t(layout.entry_count) * layout.entry_size + layout.sector_size - 1) / layout.sector_size;
	}
	inline std::uint64_t gpt_last_usable(const t_image& image, const t_gpt_layout& layout) {
		const std::uint64_t sectors = image.size() / layout.sector_size;
		return sectors - 1 - (gpt_first_usable(layout) - 2) - 1;
	}

	// protective mbr, primary header and entries at lba 1 and 2, backup entries and header at the end
	inline void put_gpt(t_image& image, const t_gpt_layout& layout, const std::vector<t_gpt_entry>& entries) {
		const std::uint64_t ss = layout.sector_size, sectors = image.size() / ss, last = sectors - 1;
		const std::uint64_t array_bytes = std::uint64_t(layout.entry_count) * layout.entry_size;
		const std::uint64_t array_sectors = gpt_first_usable(layout) - 2;
		const std::uint64_t first_usable = gpt_first_usable(layout), last_usable = gpt_last_usable(image, layout);

		put_mbr(image, 0, { t_mbr_entry{ 0, 0xEE, 1, static_cast<std::uint32_t>(sectors - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : sectors - 1) } });

		auto put_array = [&](std::uint64_t lba) {
			const std::size_t base = static_cast<std::size_t>(lba * ss);
			for (std::size_t i = 0; i < entries.size(); i++) {
				const t_gpt_entry& e = entries[i];
				const std::size_t at = base + i * layout.entry_size;
				put_guid(image, at, e.type);
				put_guid(image, at + 16, e.unique);
				put64(image, at + 32, e.first);
				put64(image, at + 40, e.last);
				put64(image, at + 48, e.attributes);
				for (std::size_t c = 0; c < e.name.size() && c < 36; c++)
					put16(image, at + 56 + 2 * c, static_cast<std::uint16_t>(e.name[c]));
			}
			return crc32(image, base, static_cast<std::size_t>(array_bytes));
		};
		auto put_header = [&](std::uint64_t lba, std::uint64_t alternate, std::uint64_t entries_lba, std::uint32_t array_crc) {
			const std::size_t at = static_cast<std::size_t>(lba * ss);
			put_text(image, at, "EFI PART");
			put32(image, at + 8, 0x00010000);
			put32(image, at + 12, 92);
			put32(image, at + 16, 0);
			put64(image, at + 24, lba);
			put64(image, at + 32, alternate);
			put64(image, at + 40, first_usable);
			put64(image, at + 48, last_usable);
			put_guid(image, at + 56, layout.disk_guid);
			put64(image, at + 72, entries_lba);
			put32(image, at + 80, layout.entry_count);
			put32(image, at + 84, layout.entry_size);
			put32(image, at + 88, array_crc);
			put32(image, at + 16, crc32(image, at, 92));
		};

		const std::uint32_t primary_crc = put_array(2);
		const std::uint32_t backup_crc = put_array(last - array_sectors);
		put_header(1, last, 2, primary_crc);
		put_header(last, 1, last - array_sectors, backup_crc);
	}

	// 8 MiB, 512 byte sectors, an mbr with:
	//   1  sectors  2048-6143   0x0C FAT32, bootable, a fat32 boot sector
	//   2  sectors  6144-10239  0x83 Linux, an ext superblock
	//      sectors 10240-16383  0x0F extended, holding
	//   5  sectors 12288-13311  0x07 NTFS / exFAT, an ntfs boot sector
	//   6  sectors 15360-16383  0x83 Linux, nothing in it
	inline t_image mbr_disk() {
		t_image image(8 * mib);
		put_mbr(image, 0, {
			t_mbr_entry{ 0x80, 0x0C, 2048, 4096 },
			t_mbr_entry{ 0x00, 0x83, 6144, 4096 },
			t_mbr_entry{ 0x00, 0x0F, 10240, 6144 },
		}, 0x46454E52);
		put_mbr(image, 10240 * 512, { t_mbr_entry{ 0, 0x07, 2048, 1024 }, t_mbr_entry{ 0, 0x05, 3072, 3072 } });
		put_mbr(image, 13312 * 512, { t_mbr_entry{ 0, 0x83, 2048, 1024 } });
		put_fat32_boot(image, 2048 * 512);
		put_ext_superblock(image, 6144 * 512);
		put_ntfs_boot(image, 12288 * 512);
		return image;
	}

	// 8 MiB, a gpt with 128 entries of 128 bytes:
	//   1  1 MiB to 2 MiB        EFI system            "EFI system partition", a fat32 boot sector
	//   2  2 MiB to 6 MiB        Microsoft basic data  "Daten é 🐺" (two byte and surrogate pair utf-16), an ntfs boot sector
	//   3  6 MiB to last usable  Linux filesystem      "Linux", an ext superblock
	inline t_image gpt_disk(std::uint32_t sector_size = 512) {
		t_image image(8 * mib);
		const t_gpt_layout layout{ .sector_size = sector_size };
		const std::uint64_t per_mib = mib / sector_size;
		put_gpt(image, layout, {
			{ efi_system, "11111111-2222-4333-8444-555555555555", 1 * per_mib, 2 * per_mib - 1, 0, u"EFI system partition" },
			{ basic_data, "66666666-7777-4888-9999-AAAAAAAAAAAA", 2 * per_mib, 6 * per_mib - 1, 0x8000000000000000, u"Daten é \U0001F43A" },
			{ linux_filesystem, "BBBBBBBB-CCCC-4DDD-8EEE-FFFFFFFFFFFF", 6 * per_mib, gpt_last_usable(image, layout), 0, u"Linux" },
		});
		put_fat32_boot(image, 1 * mib);
		put_ntfs_boot(image, 2 * mib);
		put_ext_superblock(image, 6 * mib);
		return image;
	}

	// a usb stick formatted without a partition table: fat32 from sector 0
	inline t_image superfloppy_disk() {
		t_image image(1 * mib);
		put_fat32_boot(image, 0);
		return image;
	}
}
