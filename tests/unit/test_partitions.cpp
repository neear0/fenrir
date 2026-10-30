#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "make_fixtures/fixtures.h"
#include "storage/fault_injecting_source.h"
#include "storage/file_image_source.h"
#include "storage/hash.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

using storage::Diagnostic;
using storage::FileSystem;
using storage::PartitionMap;
using storage::PartitionScheme;
using Severity = storage::Diagnostic::Severity;

namespace {
	PartitionMap scan(fixtures::t_image image, storage::PartitionScanOptions options = {}) {
		storage::MemorySource source(std::move(image));
		auto map = storage::scan_partitions(source, options);
		REQUIRE(map.has_value());
		return std::move(*map);
	}

	bool mentions(const PartitionMap& map, Severity severity, std::string_view text) {
		for (const Diagnostic& d : map.diagnostics)
			if (d.severity == severity && d.message.find(text) != std::string::npos)
				return true;
		return false;
	}

	std::string all_diagnostics(const PartitionMap& map) { // for CAPTURE, so a failure says what the scan thought
		std::string text;
		for (const Diagnostic& d : map.diagnostics)
			text += std::string(storage::to_string(d.severity)) + ": " + d.message + "\n";
		return text;
	}

	// gpt_disk's three partitions, in sector_size units
	void check_gpt_disk(const PartitionMap& map, std::uint32_t sector_size) {
		const std::uint64_t per_mib = fixtures::mib / sector_size;
		CHECK(map.scheme == PartitionScheme::gpt);
		CHECK(map.sector_size == sector_size);
		CHECK(map.disk_sectors == 8 * per_mib);
		CHECK(storage::to_string(map.disk_guid) == "5A1B3C4D-0E0F-4A1B-9C2D-3E4F5A6B7C8D");
		REQUIRE(map.partitions.size() == 3);

		const auto& efi = map.partitions[0];
		CHECK(efi.number == 1);
		CHECK(efi.first_lba == per_mib);
		CHECK(efi.sector_count == per_mib);
		CHECK(efi.offset_bytes == fixtures::mib);
		CHECK(efi.size_bytes == fixtures::mib);
		CHECK(efi.type_name == "EFI system");
		CHECK(storage::to_string(efi.type_guid) == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
		CHECK(storage::to_string(efi.unique_guid) == "11111111-2222-4333-8444-555555555555");
		CHECK(efi.name == "EFI system partition");
		CHECK(efi.filesystem == FileSystem::fat32);

		const auto& data = map.partitions[1];
		CHECK(data.number == 2);
		CHECK(data.first_lba == 2 * per_mib);
		CHECK(data.size_bytes == 4 * fixtures::mib);
		CHECK(data.type_name == "Microsoft basic data");
		CHECK(data.attributes == 0x8000000000000000);
		CHECK(data.name == reinterpret_cast<const char*>(u8"Daten é \U0001F43A"));
		CHECK(data.filesystem == FileSystem::ntfs);

		const auto& linux_part = map.partitions[2];
		CHECK(linux_part.number == 3);
		CHECK(linux_part.first_lba == 6 * per_mib);
		CHECK(linux_part.first_lba + linux_part.sector_count - 1 == map.gpt_last_usable);
		CHECK(linux_part.type_name == "Linux filesystem");
		CHECK(linux_part.filesystem == FileSystem::ext);
		for (const auto& p : map.partitions)
			CHECK(p.inside_disk);
	}
}

TEST_CASE("the reference gpt disk") {
	const PartitionMap map = scan(fixtures::gpt_disk());
	CAPTURE(all_diagnostics(map));
	check_gpt_disk(map, 512);
	CHECK(map.gpt_first_usable == 34);
	CHECK(map.gpt_last_usable == 16350);
	CHECK_FALSE(map.gpt_from_backup);
	CHECK_FALSE(map.has(Severity::warning));
	CHECK_FALSE(map.has(Severity::error));
}

TEST_CASE("the reference gpt disk with 4096 byte sectors is found without being told") {
	const PartitionMap map = scan(fixtures::gpt_disk(4096));
	CAPTURE(all_diagnostics(map));
	check_gpt_disk(map, 4096);
	CHECK(map.gpt_first_usable == 6);
	CHECK_FALSE(map.has(Severity::warning));
}

TEST_CASE("the reference mbr disk, extended partition and all") {
	const PartitionMap map = scan(fixtures::mbr_disk());
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::mbr);
	CHECK(map.mbr_signature == 0x46454E52u);
	REQUIRE(map.partitions.size() == 4);

	struct t_expected {
		std::uint32_t number;
		std::uint64_t first, count;
		std::uint8_t type;
		bool bootable, logical;
		FileSystem filesystem;
	};
	const t_expected expected[] = {
		{ 1, 2048, 4096, 0x0C, true, false, FileSystem::fat32 },
		{ 2, 6144, 4096, 0x83, false, false, FileSystem::ext },
		{ 5, 12288, 1024, 0x07, false, true, FileSystem::ntfs },
		{ 6, 15360, 1024, 0x83, false, true, FileSystem::unknown },
	};
	for (std::size_t i = 0; i < 4; i++) {
		CAPTURE(i);
		const auto& p = map.partitions[i];
		CHECK(p.number == expected[i].number);
		CHECK(p.first_lba == expected[i].first);
		CHECK(p.sector_count == expected[i].count);
		CHECK(p.offset_bytes == expected[i].first * 512);
		CHECK(p.mbr_type == expected[i].type);
		CHECK(p.bootable == expected[i].bootable);
		CHECK(p.logical == expected[i].logical);
		CHECK(p.filesystem == expected[i].filesystem);
		CHECK(p.inside_disk);
	}
	CHECK(map.partitions[0].type_name == "FAT32");
	CHECK(mentions(map, Severity::info, "holds 2 logical partitions"));
	CHECK_FALSE(map.has(Severity::warning));
}

TEST_CASE("a damaged primary gpt falls back to the backup") {
	fixtures::t_image image = fixtures::gpt_disk();
	image[512 + 48] ^= std::byte{ 1 };
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	check_gpt_disk(map, 512);
	CHECK(map.gpt_from_backup);
	CHECK(mentions(map, Severity::warning, "header CRC doesn't match"));
}

TEST_CASE("a damaged entry array is caught by its crc") {
	fixtures::t_image image = fixtures::gpt_disk();
	image[2 * 512 + 40] ^= std::byte{ 0x10 }; // the first entry's last lba, in the primary array only
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	check_gpt_disk(map, 512); // the backup's array is intact
	CHECK(map.gpt_from_backup);
	CHECK(mentions(map, Severity::warning, "entry array's CRC doesn't match"));
}

TEST_CASE("a truncated gpt image keeps what's left and says what's missing") {
	fixtures::t_image image = fixtures::gpt_disk();
	image.resize(7 * fixtures::mib);
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::gpt);
	CHECK_FALSE(map.gpt_from_backup);
	REQUIRE(map.partitions.size() == 3);
	CHECK(map.partitions[0].inside_disk);
	CHECK(map.partitions[1].inside_disk);
	CHECK_FALSE(map.partitions[2].inside_disk);
	CHECK(map.partitions[2].filesystem == FileSystem::unknown);
	CHECK(mentions(map, Severity::warning, "backup GPT isn't valid"));
	CHECK(mentions(map, Severity::warning, "partition 3"));

	storage::MemorySource source(image);
	auto view = storage::open_partition(source, map.partitions[2]);
	REQUIRE_FALSE(view.has_value());
	CHECK(view.error().code == storage::Errc::out_of_range);
	CHECK(storage::open_partition(source, map.partitions[1]).has_value());
}

TEST_CASE("neither gpt valid under a protective mbr is an error, not a guess") {
	fixtures::t_image image = fixtures::gpt_disk();
	image[512 + 48] ^= std::byte{ 1 };
	image[image.size() - 512 + 48] ^= std::byte{ 1 };
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::gpt);
	CHECK(map.partitions.empty());
	CHECK(mentions(map, Severity::error, "neither GPT is valid"));
}

TEST_CASE("an mbr without a protective entry wins over a leftover gpt") {
	fixtures::t_image image = fixtures::gpt_disk();
	fixtures::put_mbr(image, 0, { fixtures::t_mbr_entry{ 0, 0x07, 2048, 8192 } });
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::mbr);
	REQUIRE(map.partitions.size() == 1);
	CHECK(map.partitions[0].first_lba == 2048);
	CHECK(mentions(map, Severity::warning, "left over"));
}

TEST_CASE("a hybrid mbr is noted and the gpt is used") {
	fixtures::t_image image = fixtures::gpt_disk();
	fixtures::put_mbr(image, 0, { fixtures::t_mbr_entry{ 0, 0xEE, 1, 2047 }, fixtures::t_mbr_entry{ 0, 0x0C, 2048, 2048 } });
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	check_gpt_disk(map, 512);
	CHECK(mentions(map, Severity::info, "hybrid"));
}

TEST_CASE("a filesystem without a partition table") {
	const PartitionMap map = scan(fixtures::superfloppy_disk());
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::none);
	CHECK(map.whole_disk == FileSystem::fat32);
	CHECK(map.partitions.empty());
}

TEST_CASE("blank, tiny and empty images") {
	const PartitionMap blank = scan(fixtures::t_image(64 * 1024));
	CHECK(blank.scheme == PartitionScheme::none);
	CHECK(mentions(blank, Severity::info, "sector 0 is empty"));

	const PartitionMap tiny = scan(fixtures::t_image(100));
	CHECK(tiny.scheme == PartitionScheme::none);
	CHECK(tiny.disk_sectors == 0);
	CHECK(mentions(tiny, Severity::error, "smaller than one sector"));

	CHECK(scan({}).partitions.empty());

	const PartitionMap forced = scan(fixtures::t_image(2048), { .sector_size = 4096 });
	CHECK(mentions(forced, Severity::error, "smaller than one 4096 byte sector"));
}

TEST_CASE("an unsupported sector size is refused") {
	storage::MemorySource source(fixtures::gpt_disk());
	auto map = storage::scan_partitions(source, { .sector_size = 520 });
	REQUIRE_FALSE(map.has_value());
	CHECK(map.error().code == storage::Errc::invalid_argument);
}

TEST_CASE("an extended partition chain that loops is cut off") {
	fixtures::t_image image(8 * fixtures::mib);
	fixtures::put_mbr(image, 0, { fixtures::t_mbr_entry{ 0, 0x0F, 2048, 8192 } });
	// the first boot record links to the second, which links to itself (a link of 0 would just end the chain)
	fixtures::put_mbr(image, 2048 * 512, { fixtures::t_mbr_entry{ 0, 0x83, 64, 64 }, fixtures::t_mbr_entry{ 0, 0x05, 1024, 1024 } });
	fixtures::put_mbr(image, 3072 * 512, { fixtures::t_mbr_entry{ 0, 0x83, 64, 64 }, fixtures::t_mbr_entry{ 0, 0x05, 1024, 1024 } });
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	CHECK(map.scheme == PartitionScheme::mbr);
	CHECK(map.partitions.size() == 2);
	CHECK(mentions(map, Severity::warning, "loops back"));
}

TEST_CASE("overlapping and out of range partitions are flagged") {
	fixtures::t_image image(8 * fixtures::mib);
	fixtures::put_mbr(image, 0, {
		fixtures::t_mbr_entry{ 0, 0x83, 2048, 4096 },
		fixtures::t_mbr_entry{ 0, 0x83, 4096, 4096 },     // overlaps the first
		fixtures::t_mbr_entry{ 0, 0x83, 16000, 1000 },    // past the end of 16384 sectors
		fixtures::t_mbr_entry{ 0, 0x83, 0, 1 },           // on top of the mbr
	});
	const PartitionMap map = scan(image);
	CAPTURE(all_diagnostics(map));
	REQUIRE(map.partitions.size() == 4);
	CHECK(mentions(map, Severity::warning, "partitions 1 and 2 overlap"));
	CHECK(mentions(map, Severity::warning, "partition 3 (sectors 16000 to 16999) runs past the end"));
	CHECK(mentions(map, Severity::warning, "starts at sector 0"));
	CHECK_FALSE(map.partitions[2].inside_disk);
}

TEST_CASE("hostile gpt headers are refused, not followed") {
	auto with_header = [](auto change) {
		fixtures::t_image image = fixtures::gpt_disk();
		change(image);
		// re-sign the primary header so only the change itself can make it invalid, and break the backup
		fixtures::put32(image, 512 + 16, 0);
		fixtures::put32(image, 512 + 16, fixtures::crc32(image, 512, 92));
		image[image.size() - 512] = std::byte{ 0 };
		return scan(image);
	};

	const PartitionMap huge_array = with_header([](fixtures::t_image& image) { fixtures::put32(image, 512 + 80, 0xFFFFFFFF); });
	CAPTURE(all_diagnostics(huge_array));
	CHECK(huge_array.partitions.empty());
	CHECK(mentions(huge_array, Severity::error, "more than the"));

	const PartitionMap odd_entries = with_header([](fixtures::t_image& image) { fixtures::put32(image, 512 + 84, 100); });
	CHECK(mentions(odd_entries, Severity::error, "isn't 128 times a power of 2"));

	const PartitionMap far_array = with_header([](fixtures::t_image& image) { fixtures::put64(image, 512 + 72, 0x00FFFFFFFFFFFFFF); });
	CHECK(mentions(far_array, Severity::error, "runs past the end"));

	const PartitionMap wrong_place = with_header([](fixtures::t_image& image) { fixtures::put64(image, 512 + 24, 7); });
	CHECK(mentions(wrong_place, Severity::error, "it says it's at sector 7"));

	const PartitionMap big_header = with_header([](fixtures::t_image& image) { fixtures::put32(image, 512 + 12, 513); });
	CHECK(mentions(big_header, Severity::error, "header size (513 bytes)"));
}

TEST_CASE("read errors in the first sector fail the scan, later ones become diagnostics") {
	storage::MemorySource disk(fixtures::gpt_disk());

	storage::FaultInjectingSource first_sector(disk);
	first_sector.add_bad_region(0, 512);
	auto failed = storage::scan_partitions(first_sector);
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().code == storage::Errc::io_error);

	storage::FaultInjectingSource backup(disk);
	backup.add_bad_region(disk.size_bytes() - 512, 512);
	backup.add_bad_region(2 * fixtures::mib, 512); // partition 2's boot sector
	auto partial = storage::scan_partitions(backup);
	REQUIRE(partial.has_value());
	CAPTURE(all_diagnostics(*partial));
	CHECK(partial->partitions.size() == 3);
	CHECK(mentions(*partial, Severity::warning, "couldn't be read"));
	CHECK(partial->partitions[1].filesystem == FileSystem::unknown);

	storage::FaultInjectingSource short_reads(disk); // the same map from a source that reads 1 byte at a time
	short_reads.set_max_read(1);
	auto slow = storage::scan_partitions(short_reads);
	REQUIRE(slow.has_value());
	check_gpt_disk(*slow, 512);
}

TEST_CASE("random damage never crashes the scanner") {
	// a cheap stand-in for fuzzing until a libFuzzer target exists: flip bytes in the tables of both reference disks and
	// check every scan returns, with partitions that at least have sane byte ranges
	std::mt19937_64 random(0xF3E4121);
	const fixtures::t_image disks[] = { fixtures::gpt_disk(), fixtures::mbr_disk() };
	for (const fixtures::t_image& original : disks) {
		for (int round = 0; round < 400; round++) {
			fixtures::t_image image = original;
			const int flips = 1 + static_cast<int>(random() % 16);
			for (int i = 0; i < flips; i++) {
				const std::size_t region = random() % 3;
				const std::size_t at = region == 0 ? random() % (34 * 512)                       // mbr, header, entries
					: region == 1 ? image.size() - 1 - random() % (33 * 512)                      // the backup gpt
					: (10240 + random() % 6144) * 512 + 0x1BE + random() % 66;                     // the boot records
				image[at] = static_cast<std::byte>(random());
			}
			storage::MemorySource source(image);
			auto map = storage::scan_partitions(source);
			REQUIRE(map.has_value());
			for (const auto& p : map->partitions)
				if (p.inside_disk)
					REQUIRE(p.offset_bytes + p.size_bytes <= image.size());
		}
	}
	// and pure noise
	for (int round = 0; round < 200; round++) {
		fixtures::t_image image(64 * 1024);
		for (auto& b : image)
			b = static_cast<std::byte>(random());
		storage::MemorySource source(image);
		REQUIRE(storage::scan_partitions(source).has_value());
	}
}

TEST_CASE("scanning an image file leaves it exactly as it was") {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / ("fenrir_scan_" + std::to_string(std::random_device{}()));
	fs::create_directories(dir);
	const fs::path path = dir / "gpt.img";
	{
		const fixtures::t_image image = fixtures::gpt_disk();
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
	}
	const auto written = fs::last_write_time(path);

	auto hash_file = [&] {
		auto opened = storage::FileImageSource::open(path);
		REQUIRE(opened.has_value());
		auto digest = storage::sha256_source(**opened);
		REQUIRE(digest.has_value());
		return *digest;
	};
	const auto before = hash_file();
	{
		auto opened = storage::FileImageSource::open(path);
		REQUIRE(opened.has_value());
		auto map = storage::scan_partitions(**opened);
		REQUIRE(map.has_value());
		CHECK(map->partitions.size() == 3);
		for (const auto& p : map->partitions) { // and open each partition and read all of it
			auto view = storage::open_partition(**opened, p);
			REQUIRE(view.has_value());
			REQUIRE(storage::sha256_source(**view).has_value());
		}
	}
	CHECK(hash_file() == before);
	CHECK(before == storage::Sha256::of(fixtures::gpt_disk()));
	CHECK(fs::last_write_time(path) == written);

	std::error_code ignored;
	fs::remove_all(dir, ignored);
}
